#!/usr/bin/env python3
"""Batch-based preview for rpi-highspeed-camera (run on a Pi desktop)."""
import argparse
from io import BytesIO
from pathlib import Path
import queue
import shutil
import subprocess
import tempfile
import threading
import time
import tkinter as tk

from PIL import Image, ImageTk


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames", type=int, default=10, help="frames captured per batch")
    parser.add_argument("--frame", type=int, default=5, help="zero-based frame to display")
    parser.add_argument("--mode", default="9767e16d3bd1ba1d")
    parser.add_argument("--size", default="640x128")
    parser.add_argument("--frame-duration-us", type=int, default=3333)
    parser.add_argument("--exposure-us", type=int, default=3000)
    parser.add_argument("--gain", type=float, default=1.0)
    parser.add_argument("--scale", type=int, default=2, help="integer preview enlargement")
    args = parser.parse_args()
    if not (0 <= args.frame < args.frames) or args.frames < 1 or args.scale < 1:
        parser.error("require 0 <= --frame < --frames, positive --frames and --scale")
    if args.frame_duration_us < 1 or args.exposure_us < 1 or args.gain <= 0:
        parser.error("frame duration and exposure must be positive; gain must be > 0")

    # The application lives in apps/hscam-live/; resolve repository root.
    repo = Path(__file__).resolve().parents[2]
    capture = repo / "build/hscam-capture"
    export = repo / "build/hscam-export"
    for executable in (capture, export):
        if not executable.is_file():
            parser.error(f"missing executable: {executable}")

    messages = queue.Queue(maxsize=1)
    stopping = threading.Event()
    proc_lock = threading.Lock()
    active = [None]

    def run_command(argv):
        # Serialize process startup with window shutdown: no orphan capture.
        with proc_lock:
            if stopping.is_set():
                return
            proc = subprocess.Popen(
                [str(item) for item in argv], cwd=repo,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            active[0] = proc
        try:
            stdout, stderr = proc.communicate(timeout=30)
            if proc.returncode:
                raise RuntimeError(
                    f"{Path(argv[0]).name} exited {proc.returncode}:\n{stderr or stdout}"
                )
        except subprocess.TimeoutExpired as exc:
            proc.kill()
            proc.communicate()
            raise RuntimeError(f"{Path(argv[0]).name} timed out") from exc
        finally:
            with proc_lock:
                if active[0] is proc:
                    active[0] = None

    def publish(item):
        # Only the newest preview matters. Never grow a backlog of old images.
        try:
            messages.put_nowait(item)
        except queue.Full:
            try:
                messages.get_nowait()
            except queue.Empty:
                pass
            messages.put_nowait(item)

    def worker():
        try:
            with tempfile.TemporaryDirectory(prefix="hscam-live-") as tmp:
                tmp = Path(tmp)
                number = 0
                while not stopping.is_set():
                    start = time.monotonic()
                    bundle = tmp / f"capture-{number}.hscap"
                    png = tmp / f"frame-{number}.png"
                    run_command([
                        capture, "--mode", args.mode,
                        "--raw", "--size", args.size,
                        "--representation", "uncompressed",
                        "--frame-duration-us", args.frame_duration_us,
                        "--exposure-us", args.exposure_us,
                        "--gain", args.gain,
                        "--frames", args.frames, "--output", bundle,
                    ])
                    if stopping.is_set():
                        break
                    run_command([
                        export, "image", bundle,
                        "--frame", args.frame, "--output", png,
                    ])
                    image_bytes = png.read_bytes()
                    shutil.rmtree(bundle)
                    png.unlink()
                    number += 1
                    publish(("frame", image_bytes, number, time.monotonic() - start))
        except Exception as exc:
            if not stopping.is_set():
                publish(("error", str(exc)))

    root = tk.Tk()
    root.title("HSCAM batch live view")
    image_label = tk.Label(root, text="Starting camera...", bg="black", fg="white")
    image_label.pack(fill="both", expand=True)
    status = tk.Label(root, text="Waiting for first batch", anchor="w")
    status.pack(fill="x")

    def on_close():
        stopping.set()
        with proc_lock:
            proc = active[0]
            if proc is not None and proc.poll() is None:
                proc.terminate()
        worker_thread.join(timeout=2.0)  # Give temporary-file cleanup a chance.
        if proc is not None and proc.poll() is None:
            proc.kill()
        root.destroy()

    def refresh():
        latest = None
        try:
            while True:
                latest = messages.get_nowait()
        except queue.Empty:
            pass
        if latest:
            if latest[0] == "error":
                status.config(text="Capture stopped: " + latest[1])
                stopping.set()
            else:
                _, data, count, seconds = latest
                with Image.open(BytesIO(data)) as decoded:
                    image = decoded.convert("RGB")
                image = image.resize(
                    (image.width * args.scale, image.height * args.scale),
                    Image.Resampling.NEAREST,
                )
                preview = ImageTk.PhotoImage(image)
                image_label.config(image=preview, text="")
                image_label.image = preview  # Keep a Tk reference alive.
                status.config(text=f"Batch {count} | frame {args.frame}/{args.frames} "
                                   f"| {seconds * 1000:.0f} ms/batch "
                                   f"| {1 / seconds:.1f} preview FPS")
        if not stopping.is_set():
            root.after(30, refresh)

    root.protocol("WM_DELETE_WINDOW", on_close)
    worker_thread = threading.Thread(target=worker, daemon=True)
    worker_thread.start()
    root.after(30, refresh)
    root.mainloop()


if __name__ == "__main__":
    main()

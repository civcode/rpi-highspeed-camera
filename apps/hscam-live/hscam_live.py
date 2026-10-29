#!/usr/bin/env python3
"""Batch-based preview for rpi-highspeed-camera (run on a Pi desktop)."""
import argparse
from dataclasses import dataclass
from io import BytesIO
import math
from pathlib import Path
import queue
import shutil
import subprocess
import tempfile
import threading
import time
import tkinter as tk
from tkinter import ttk

from PIL import Image, ImageTk



@dataclass(frozen=True)
class BatchSettings:
    frames: int
    frame: int
    frame_duration_us: int
    exposure_us: int
    gain: float

    @classmethod
    def parse(cls, values):
        """Validate typed GUI values before publishing a new batch snapshot."""
        def integer(name):
            try:
                return int(values[name])
            except (ValueError, TypeError) as exc:
                raise ValueError(f"{name} must be a whole number") from exc

        try:
            gain = float(values["gain"])
        except (ValueError, TypeError) as exc:
            raise ValueError("gain must be a number") from exc

        result = cls(
            frames=integer("frames"),
            frame=integer("frame"),
            frame_duration_us=integer("frame_duration_us"),
            exposure_us=integer("exposure_us"),
            gain=gain,
        )
        if result.frames < 1:
            raise ValueError("frames must be at least 1")
        if not 0 <= result.frame < result.frames:
            raise ValueError(f"frame must be between 0 and {result.frames - 1} (zero-based)")
        if result.frame_duration_us < 1 or result.exposure_us < 1:
            raise ValueError("frame duration and exposure must be positive")
        if not math.isfinite(result.gain) or result.gain <= 0:
            raise ValueError("gain must be a positive finite number")
        return result

    def summary(self):
        return (f"{self.frames} frames, frame {self.frame}, "
                f"duration {self.frame_duration_us} µs, "
                f"exposure {self.exposure_us} µs, gain {self.gain:g}")


class SharedSettings:
    """Only immutable snapshots cross from Tk's thread into the worker."""

    def __init__(self, initial):
        self._lock = threading.Lock()
        self._settings = initial
        self._revision = 0

    def snapshot(self):
        with self._lock:
            return self._settings, self._revision

    def update(self, settings):
        with self._lock:
            self._settings = settings
            self._revision += 1
            return self._revision


def capture_command(executable, bundle, mode, size, settings):
    return [
        executable, "--mode", mode,
        "--raw", "--size", size,
        "--representation", "uncompressed",
        "--frame-duration-us", settings.frame_duration_us,
        "--exposure-us", settings.exposure_us,
        "--gain", settings.gain,
        "--frames", settings.frames, "--output", bundle,
    ]


def export_command(executable, bundle, png, settings):
    return [executable, "image", bundle, "--frame", settings.frame, "--output", png]


class PngSaver:
    """Number manual snapshots in /tmp, preserving files from earlier runs."""

    def __init__(self, directory=Path("/tmp")):
        self.directory = Path(directory)
        self.next_number = 1
        for path in self.directory.glob("hscam-live-*.png"):
            number = path.name.removeprefix("hscam-live-").removesuffix(".png")
            if number.isascii() and number.isdecimal():
                self.next_number = max(self.next_number, int(number) + 1)

    def save(self, image_bytes):
        if not image_bytes.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError("the displayed frame does not contain PNG data")
        while True:
            path = self.directory / f"hscam-live-{self.next_number:06d}.png"
            self.next_number += 1
            try:
                # Exclusive creation: never overwrite a snapshot, even when a
                # second viewer is running or a file appeared after startup.
                with path.open("xb") as output:
                    try:
                        output.write(image_bytes)
                    except OSError:
                        path.unlink(missing_ok=True)
                        raise
                return path
            except FileExistsError:
                continue


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames", type=int, default=10, help="initial frames per batch")
    parser.add_argument("--frame", type=int, default=5, help="initial zero-based preview frame")
    parser.add_argument("--mode", default="9767e16d3bd1ba1d")
    parser.add_argument("--size", default="640x128")
    parser.add_argument("--frame-duration-us", type=int, default=3333)
    parser.add_argument("--exposure-us", type=int, default=3000)
    parser.add_argument("--gain", type=float, default=1.0)
    parser.add_argument("--scale", type=int, default=2, help="integer preview enlargement")
    args = parser.parse_args()
    if args.scale < 1:
        parser.error("--scale must be positive")
    try:
        initial = BatchSettings.parse(vars(args))
    except ValueError as exc:
        parser.error(str(exc))

    # The application lives in apps/hscam-live/; resolve repository root.
    repo = Path(__file__).resolve().parents[2]
    capture = repo / "build/hscam-capture"
    export = repo / "build/hscam-export"
    for executable in (capture, export):
        if not executable.is_file():
            parser.error(f"missing executable: {executable}")

    shared = SharedSettings(initial)
    messages = queue.Queue(maxsize=1)
    stopping = threading.Event()
    proc_lock = threading.Lock()
    active = [None]

    def run_command(argv, timeout=30):
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
            stdout, stderr = proc.communicate(timeout=timeout)
            if proc.returncode and not stopping.is_set():
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
                    # One immutable snapshot governs BOTH capture and export.
                    settings, revision = shared.snapshot()
                    start = time.monotonic()
                    bundle = tmp / f"capture-{number}.hscap"
                    png = tmp / f"frame-{number}.png"
                    timeout = max(30, settings.frames * settings.frame_duration_us / 1e6 * 4 + 15)
                    run_command(capture_command(capture, bundle, args.mode, args.size, settings), timeout)
                    if stopping.is_set():
                        break
                    run_command(export_command(export, bundle, png, settings))
                    if stopping.is_set():
                        break
                    image_bytes = png.read_bytes()
                    shutil.rmtree(bundle)
                    png.unlink()
                    number += 1
                    publish(("frame", image_bytes, number, time.monotonic() - start,
                             settings, revision))
        except Exception as exc:
            if not stopping.is_set():
                publish(("error", str(exc)))

    root = tk.Tk()
    root.title("HSCAM batch live view")

    controls = ttk.LabelFrame(root, text="Capture settings — apply to next batch")
    controls.pack(fill="x", padx=8, pady=(8, 4))
    fields = (
        ("Frames / batch", "frames", initial.frames, 1, 100000, 1),
        ("Preview frame (0-based)", "frame", initial.frame, 0, 99999, 1),
        ("Frame duration (µs)", "frame_duration_us", initial.frame_duration_us, 1, 10000000, 100),
        ("Exposure (µs)", "exposure_us", initial.exposure_us, 1, 10000000, 100),
        ("Analogue gain", "gain", initial.gain, 0.1, 128, 0.5),
    )
    field_vars = {}
    spinboxes = []
    for column, (title, name, value, minimum, maximum, step) in enumerate(fields):
        ttk.Label(controls, text=title).grid(row=0, column=column, sticky="w", padx=5, pady=(4, 1))
        variable = tk.StringVar(value=str(value))
        field_vars[name] = variable
        spin = ttk.Spinbox(controls, from_=minimum, to=maximum, increment=step,
                           textvariable=variable, width=13)
        spin.grid(row=1, column=column, sticky="ew", padx=5, pady=(0, 5))
        spinboxes.append(spin)
        controls.columnconfigure(column, weight=1)

    controls_message = tk.Label(root, text="Edit values, then Apply next batch.", anchor="w")
    controls_message.pack(fill="x", padx=10)

    def apply_settings(_event=None):
        try:
            candidate = BatchSettings.parse({key: value.get() for key, value in field_vars.items()})
        except ValueError as exc:
            controls_message.config(text=f"Not applied: {exc}", fg="firebrick")
            return
        revision = shared.update(candidate)
        controls_message.config(
            text=f"Queued settings #{revision} for the next batch: {candidate.summary()}",
            fg="darkgreen",
        )

    apply_button = ttk.Button(controls, text="Apply next batch", command=apply_settings)
    apply_button.grid(row=1, column=len(fields), sticky="ew", padx=8, pady=(0, 5))
    for spin in spinboxes:
        spin.bind("<Return>", apply_settings)
        spin.bind("<KP_Enter>", apply_settings)
    for variable in field_vars.values():
        variable.trace_add("write", lambda *_: controls_message.config(
            text="Values edited — click Apply next batch to use them.", fg="black"))

    image_label = tk.Label(root, text="Starting camera...", bg="black", fg="white")
    image_label.pack(fill="both", expand=True, padx=8, pady=5)

    saver = PngSaver()
    displayed_png = None

    def save_current_image():
        if displayed_png is None:
            return
        try:
            path = saver.save(displayed_png)
        except (OSError, ValueError) as exc:
            save_message.config(text=f"Save failed: {exc}", foreground="firebrick")
        else:
            save_message.config(text=f"Saved {path}", foreground="darkgreen")

    save_bar = ttk.Frame(root)
    save_bar.pack(fill="x", padx=8, pady=(0, 4))
    save_button = ttk.Button(save_bar, text="Save image", command=save_current_image,
                             state="disabled")
    save_button.pack(side="left")
    save_message = ttk.Label(save_bar, text="Save displayed PNG to /tmp (numbered files)")
    save_message.pack(side="left", padx=10)

    status = tk.Label(root, text="Waiting for first batch", anchor="w")
    status.pack(fill="x", padx=8, pady=(0, 8))

    def on_close():
        stopping.set()
        with proc_lock:
            proc = active[0]
            if proc is not None and proc.poll() is None:
                proc.terminate()
        worker_thread.join(timeout=2.0)
        if proc is not None and proc.poll() is None:
            proc.kill()
        root.destroy()

    def refresh():
        nonlocal displayed_png
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
                _, data, count, seconds, settings, revision = latest
                with Image.open(BytesIO(data)) as decoded:
                    image = decoded.convert("RGB")
                image = image.resize(
                    (image.width * args.scale, image.height * args.scale),
                    Image.Resampling.NEAREST,
                )
                preview = ImageTk.PhotoImage(image)
                image_label.config(image=preview, text="")
                image_label.image = preview
                displayed_png = data  # The original PNG, not the scaled Tk preview.
                save_button.config(state="normal")
                status.config(
                    text=f"Batch {count} (settings #{revision}) | {settings.summary()} | "
                         f"{seconds * 1000:.0f} ms/batch | {1 / max(seconds, 1e-9):.1f} preview FPS"
                )
        if not stopping.is_set():
            root.after(30, refresh)

    root.protocol("WM_DELETE_WINDOW", on_close)
    worker_thread = threading.Thread(target=worker, daemon=True)
    worker_thread.start()
    root.after(30, refresh)
    root.mainloop()


if __name__ == "__main__":
    main()

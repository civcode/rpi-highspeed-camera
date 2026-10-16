#!/usr/bin/env python3
"""Run ONE fail-stop OV5647 640x240 FPS probe using the hscam capture library.

Requires the Pi 5 experimental kernel module already loaded and the matching
readout mode advertised. No automatic ramp or kernel/module/I2C changes.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import pathlib
import re
import subprocess
import sys

KERNEL = "6.18.50+rpt-rpi-2712"
PROFILE = "ov5647-640x240-rev1-unvalidated"
MODULE_SHA256 = "f8a5b3a6a85b5f1da9d31e95716bdef40d0bc0ce4003620f577de16266217777"
CANDIDATE_RATES = (60, 90, 120, 150, 170)
SUMMARY_RE = re.compile(
    r"\bframes=(\d+)\s+gaps=(\d+)\s+overruns=(\d+)(?:\s+fps=([\d.]+))?"
)
ROOT = pathlib.Path(__file__).resolve().parents[2]
TAG_RE = re.compile(r"[a-z0-9][a-z0-9-]{0,39}\Z")


def validate_exposure(fps: int, exposure_us: int) -> int:
    """Reject nonphysical exposure requests before touching the camera."""
    duration = round(1_000_000 / fps)
    if not 1 <= exposure_us < duration:
        raise ValueError(
            f"exposure must be 1..{duration - 1} us at {fps} FPS "
            "(strictly shorter than frame duration)"
        )
    return duration


def evidence_basename(fps: int, tag: str) -> str:
    if tag and not TAG_RE.fullmatch(tag):
        raise ValueError("--tag must be 1-40 lowercase letters/digits/hyphens")
    return f"ov5647-640x240-{fps}fps" + (f"-{tag}" if tag else "")


def parse_capture_summary(output: str) -> tuple[int, int, int, float]:
    match = SUMMARY_RE.search(output)
    if match is None or match.group(4) is None:
        raise ValueError("capture produced no complete FPS/sequence statistics")
    frames, gaps, overruns = (int(match.group(i)) for i in (1, 2, 3))
    fps = float(match.group(4))
    if not (0 < fps < 10000):
        raise ValueError("invalid measured FPS")
    return frames, gaps, overruns, fps


def get_sensor_mode(inspect_output: dict) -> str:
    cameras = [
        c for c in inspect_output.get("cameras", [])
        if c.get("sensor_model", c.get("model")) == "ov5647"
    ]
    if len(cameras) != 1:
        raise RuntimeError("Expected exactly one OV5647 camera")
    candidates = [
        m for m in cameras[0].get("modes", [])
        if m.get("width") == 640 and m.get("height") == 240
        and m.get("bit_depth") == 10
    ]
    if len(candidates) != 1:
        raise RuntimeError("Experimental OV5647 640x240 RAW10 mode not uniquely advertised")
    return candidates[0]["id"]


def require_experimental_module() -> str:
    running = subprocess.check_output(["uname", "-r"], text=True).strip()
    if running != KERNEL:
        raise RuntimeError(f"Incorrect kernel {running}; expected {KERNEL}")
    module = pathlib.Path("/lib/modules") / KERNEL / "updates/hscam/ov5647.ko"
    if not module.is_file():
        raise RuntimeError("Experimental override not installed: " + str(module))
    digest = hashlib.sha256(module.read_bytes()).hexdigest()
    if digest != MODULE_SHA256:
        raise RuntimeError(f"Experimental override hash does not match known compiled candidate: {digest}")
    result = subprocess.check_output(
        ["modinfo", "-F", "hscam_experimental", str(module)], text=True
    ).strip()
    if result != PROFILE:
        raise RuntimeError("Module profile marker mismatch")
    bound_driver = pathlib.Path("/sys/bus/i2c/devices/10-0036/driver")
    if not bound_driver.is_symlink() or bound_driver.resolve().name != "ov5647":
        raise RuntimeError("Expected bound OV5647 I2C driver is absent")
    # A static modinfo check cannot attest the exact bytes currently executing.
    # Combined with mode enumeration after the user's reboot, this is the
    # best conservative preflight available without instrumenting the kernel.
    return digest


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--fps", type=int, required=True, choices=CANDIDATE_RATES)
    p.add_argument("--frames", type=int, default=0,
                   help="Default: three seconds at the requested FPS")
    p.add_argument("--save-payload", action="store_true",
                   help="Store HSCAP evidence; otherwise discard payload for throughput")
    p.add_argument("--exposure-us", type=int, default=1000,
                   help="Manual exposure in us (default 1000; must be less than frame period)")
    p.add_argument("--gain", type=float, default=None,
                   help="Optional manual analogue gain, 1.0 to 16.0")
    p.add_argument("--tag", default="",
                   help="Evidence suffix (lowercase letters/digits/hyphens); use for retests")
    p.add_argument("--work-dir", type=pathlib.Path,
                   default=ROOT / "qualification/work")
    args = p.parse_args()
    if args.frames < 0 or args.frames > 10000:
        p.error("--frames must be between 0 and 10000")
    frames = args.frames or args.fps * 3
    try:
        frame_duration_us = validate_exposure(args.fps, args.exposure_us)
        base = evidence_basename(args.fps, args.tag)
    except ValueError as e:
        p.error(str(e))
    if args.gain is not None and not (math.isfinite(args.gain) and 1.0 <= args.gain <= 16.0):
        p.error("--gain must be finite and between 1.0 and 16.0")
    capture = ROOT / "build/hscam-capture"
    inspect = ROOT / "build/hscam-inspect"
    if not capture.is_file() or not inspect.is_file():
        p.error("Missing built hscam-capture or hscam-inspect; compile CMake targets first")

    try:
        digest = require_experimental_module()
        info = subprocess.run([str(inspect), "--json"], check=True,
                              capture_output=True, text=True, timeout=20)
        mode = get_sensor_mode(json.loads(info.stdout))
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError,
            subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        print(f"PRECHECK FAILED: {e}", file=sys.stderr)
        return 2

    args.work_dir.mkdir(parents=True, exist_ok=True)
    logfile = args.work_dir / f"{base}-probe.txt"
    bundle = args.work_dir / f"{base}.hscap"
    if logfile.exists() or (args.save_payload and bundle.exists()):
        print("Refusing to overwrite existing experiment evidence; choose a fresh --tag",
              file=sys.stderr)
        return 2
    cmd = [
        str(capture), "--mode", mode, "--raw", "--size", "640x240",
        "--representation", "uncompressed",
        "--frame-duration-us", str(frame_duration_us),
        "--exposure-us", str(args.exposure_us), "--frames", str(frames),
    ]
    if args.gain is not None:
        cmd.extend(["--gain", str(args.gain)])
    if args.save_payload:
        cmd.extend(["--output", str(bundle)])
    else:
        cmd.append("--discard-payload")

    lines = [
        "OV5647 Pi 5 640x240 experimental single-rate probe",
        f"UTC: {dt.datetime.now(dt.timezone.utc).isoformat()}",
        f"kernel: {KERNEL}",
        f"module_profile: {PROFILE}",
        f"module_sha256: {digest}",
        f"mode_id: {mode}",
        f"requested_fps: {args.fps}",
        f"requested_duration_us: {frame_duration_us}",
        f"requested_exposure_us: {args.exposure_us}",
        f"requested_analogue_gain: {args.gain}",
        f"frames_requested: {frames}",
        f"payload_saved: {args.save_payload}",
        "command: " + " ".join(cmd),
    ]
    print(f"Probe: {args.fps} FPS, {frames} frames, 640x240, RAW10 unpacked")
    print(f"Exposure: {args.exposure_us} us; analogue gain: {args.gain if args.gain is not None else 'camera default'}")
    print(f"Log: {logfile}")
    print("No driver, boot or sensor-I2C changes. One FPS target per invocation.")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=max(30, int(frames / (args.fps * 0.5) + 20)))
        lines.append(f"process_returncode: {result.returncode}")
        lines.append("stdout:\n" + result.stdout)
        lines.append("stderr:\n" + result.stderr)
        print(result.stdout, end="")
        if result.returncode:
            print(result.stderr[-5000:], file=sys.stderr)
            print(f"CAPTURE FAILED ({result.returncode}); stop ramp and inspect logs", file=sys.stderr)
            outcome = 1
        else:
            frames_done, gaps, overruns, measured = parse_capture_summary(result.stdout)
            valid_size = "configured 640x240 " in result.stdout
            valid_raw = "representation=uncompressed" in result.stdout
            close_to_target = measured >= args.fps * 0.90
            passing = frames_done >= frames and not gaps and not overruns and valid_size and valid_raw and close_to_target
            lines.append(f"measured_fps: {measured}")
            lines.append(f"all_probe_gates_passed: {passing}")
            print(f"Result: requested={args.fps} measured={measured:.3f} "
                  f"gaps={gaps} overruns={overruns}")
            if not passing:
                print("PROBE GATE FAILED: check geometry, stream, gaps, overruns and actual FPS; stop ramp")
            else:
                print("PROBE GATES PASSED: inspect image evidence before moving to a higher rate")
            outcome = 0 if passing else 1
    except (subprocess.TimeoutExpired, ValueError) as e:
        lines.append(f"probe_exception: {e}")
        print(f"PROBE FAILED: {e}. Stop ramp and inspect camera logs.", file=sys.stderr)
        outcome = 1

    logfile.write_text("\n".join(lines) + "\n")
    print(f"Evidence log written: {logfile}")
    return outcome


if __name__ == "__main__":
    sys.exit(main())

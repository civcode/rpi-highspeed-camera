# HSCAM batch live view

A simple desktop preview for the experimental Raspberry Pi camera modes. It repeatedly runs `hscam-capture` for a short RAW burst, exports one frame using `hscam-export`, and refreshes a persistent Tkinter window. This is a **batch preview**, not uninterrupted video: the camera is reopened for every burst, so preview FPS is lower than the requested sensor FPS.

## Set up (Raspberry Pi OS desktop)

From the **repository root**, first build `build/hscam-capture` and `build/hscam-export` as described in the [project README](../../README.md). Install Python's Tkinter bindings and virtual-environment support:

```bash
sudo apt update
sudo apt install python3-venv python3-tk

python3 -m venv apps/hscam-live/.venv
apps/hscam-live/.venv/bin/python -m pip install -r apps/hscam-live/requirements.txt
```

Only Pillow is installed from PyPI, **inside the virtual environment**. Tkinter comes from the OS package. The `.venv/` directory is ignored by Git. Run from a graphical desktop session (a headless SSH shell without an available display cannot show the window).

## Run

From the repository root:

```bash
apps/hscam-live/.venv/bin/python apps/hscam-live/hscam_live.py \
  --mode 9767e16d3bd1ba1d \
  --frames 10 --frame 5 \
  --frame-duration-us 3333 --exposure-us 3000 --gain 1 \
  --scale 2
```

The defaults already use the listed OV5647 mode, `640x128` RAW/uncompressed, 10 frames per burst, zero-based frame index 5, 3333 µs frame duration, 3000 µs exposure, gain 1, and 2× nearest-neighbor zoom. Override them with the corresponding command-line options (`--size` is also available). Close the window to stop.

The app creates HSCAP bundles and PNGs in a temporary directory and removes each after display. It checks for `build/hscam-capture` and `build/hscam-export` relative to the repository, regardless of the current working directory. The status bar reports **batch time** and **preview FPS**, not measured sensor FPS. `hscam-capture` must have exclusive camera access, and a failed capture/export stops the preview with an error message.

For performance-sensitive continuous live view, a future implementation should keep a single `libhscam` capture session open and convert selected frames in memory rather than restarting the camera and writing HSCAP/PNG files every burst.

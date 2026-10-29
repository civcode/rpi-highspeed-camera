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

The defaults already use the listed OV5647 mode, `640x128` RAW/uncompressed, 10 frames per burst, zero-based frame index 5, 3333 µs frame duration, 3000 µs exposure, gain 1, and 2× nearest-neighbor zoom. Command-line options provide the **initial** GUI values (`--size` and `--mode` remain launch-time settings). Close the window to stop.

## Change settings while previewing

The controls above the image let you edit:

- **Frames / batch**: number of RAW frames captured in each burst
- **Preview frame (0-based)**: frame exported as PNG, from `0` to `frames - 1`
- **Frame duration (µs)**: requested sensor frame interval (e.g. `3333` for approximately 300 FPS)
- **Exposure (µs)**: requested exposure duration (e.g. `3000`)
- **Analogue gain**: requested gain multiplier (e.g. `1` or `2.5`)

Enter values with the spinboxes and select **Apply next batch** (or press Enter in a field). The app validates all five values together; an invalid setting is rejected and **does not change the running capture**. A validated change is queued atomically: the currently running capture/export finishes with its original settings, and the worker uses the new settings when it starts its next batch. If that next batch has already started, the change takes effect one batch later. The preview status shows the settings revision and values used for the **displayed image**, so it is clear when a change has taken effect.

There is no need to restart the GUI to change those five parameters. Camera settings remain subject to sensor/libcamera constraints; the preview stops and displays the capture error if a requested combination is rejected.

## Save a preview image

Click **Save image** below the preview to write the **currently displayed frame** as a PNG directly under `/tmp`. No file-selection dialog appears. The button becomes available after the first frame is displayed.

Files are named `/tmp/hscam-live-000001.png`, `/tmp/hscam-live-000002.png`, and so on. The numbering resumes after the highest existing matching file when the app restarts, and an existing file is never overwritten. The full-resolution PNG from `hscam-export` is saved unchanged; the GUI's `--scale` zoom is **not** baked into the saved image. The UI shows the saved path or any error.

Only images explicitly saved with this button are kept. Unsaved intermediate HSCAP/PNG files remain temporary and are cleaned up after each batch. Note that `/tmp` is temporary storage; these snapshots may be deleted on reboot or by system cleanup. Copy files elsewhere if you want to retain them.

## Local validation

From the repository root, after installing the virtual environment:

```bash
apps/hscam-live/.venv/bin/python -m unittest discover -s apps/hscam-live -p 'test_*.py' -v
```

These tests validate settings changes, CLI argument construction, and numbered snapshot saving without using the camera or starting the GUI.

The app creates intermediate HSCAP bundles and PNGs in a temporary directory and removes each after display. It checks for `build/hscam-capture` and `build/hscam-export` relative to the repository, regardless of the current working directory. The status bar reports **batch time** and **preview FPS**, not measured sensor FPS. `hscam-capture` must have exclusive camera access, and a failed capture/export stops the preview with an error message.

For performance-sensitive continuous live view, a future implementation should keep a single `libhscam` capture session open and convert selected frames in memory rather than restarting the camera and writing HSCAP/PNG files every burst.

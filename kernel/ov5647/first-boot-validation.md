# OV5647 H5 — first Pi 5 boot and RAW validation

**Hardware field observation (2026-10-16):** experimental OV5647 module
`f8a5b3a6a85b5f1da9d31e95716bdef40d0bc0ce4003620f577de16266217777`
was staged as `updates/hscam/ov5647.ko` and the Pi rebooted successfully
into `6.18.50+rpt-rpi-2712`. `lsmod` marked `ov5647` as out-of-tree
(`(O)`), the OV5647 remained bound at I²C `10-0036`, and libcamera
registered the sensor on PiSP/RP1 CFE. `hscam-inspect --json` discovered
the fifth, new sensor mode **640×240 RAW10**:

- stable mode ID (this specific build):
  `3fc920b29f2defb0`
- the existing 640×480, 1296×972, 1920×1080, and 2592×1944 modes
  remained enumerated;
- libcamera accepted sensor-mode *discovery* but **no RAW frames or FPS
  have been validated**.

`hscam-inspect` reported `sensor_subdevice: null` and crop capability
flags false, despite a V4L2 OV5647 subdevice being present. This is a
**separate media-device association/discovery issue** worth diagnosing
later. It should not be treated as evidence that `.get_selection` does
not exist. The experimental 640×240 mode itself is accessed through the
normal libcamera mode enumeration.

## 1. Build missing userspace discovery executable

The Pi had an existing `build/hscam-inspect` executable, but the newly
added `build/hscam-highspeed` target had not yet been built:

```bash
git pull origin dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHSCAM_REQUIRE_LIBCAMERA=ON
cmake --build build --target hscam-highspeed hscam-capture hscam-export -j2
build/hscam-highspeed --json
```

Expected read-only classification: sensor `ov5647` and
`strategy=experimental_modes_advertised`, with
`reference_modes_advertised=true`, **not**
`experimentally_validated=true`. This command does not program
the sensor or capture anything.

## 2. Before capture, preserve live driver state

```bash
bash kernel/ov5647/manage-module-override.sh status
build/hscam-inspect --json
```

The module resolver should still point to
`/lib/modules/6.18.50+rpt-rpi-2712/updates/hscam/ov5647.ko`.
The `ov5647 (O)` out-of-tree module flag and the fifth mode strongly
support experimental driver activation, but file hashes of the on-disk
module alone do not prove the exact in-memory image. Log `dmesg`
if driver identity becomes ambiguous.

**Do not** write I²C registers or `rmmod` a running camera driver.

## 3. First conservative RAW trial (30 fps request)

Use the exact **sensor mode ID**, not merely an output size. A
~33.3 ms requested frame duration keeps this first test well below the
high-FPS stress target; note that the 87.5 MHz candidate sensor clock
is programmed even when frames are spaced further apart.

```bash
mkdir -p qualification/work

build/hscam-capture \
  --mode 3fc920b29f2defb0 \
  --raw \
  --size 640x240 \
  --representation uncompressed \
  --frame-duration-us 33333 \
  --exposure-us 1000 \
  --frames 32 \
  --output qualification/work/ov5647-640x240-initial.hscap
```

**Do not add `--exact` for this first run:** PiSP may legitimately
convert requested unpacked Bayer samples into `SGBRG16` from the
10-bit sensor. The capture program reports negotiated geometry, format,
representation, adjustments, request count, sequence gaps, queue overruns
and sensor-timestamp-derived FPS.

**Acceptance for this step:**

- The configured RAW output is exactly **640×240** (not resized to a
  stock mode); format is unpacked, correct bit depth noted.
- There are actual completed buffers with plausible per-frame timestamps.
- No sequence gaps, overruns, driver probe errors or CFE stream errors.
- This confirms RAW data through the driver/CFE pipeline, but is
  **not yet** independent proof of actual pixel-window bounds or ~179 FPS.

If capture fails, **stop**, save the complete error and collect read-only
logs. Do not immediately request more FPS:

```bash
dmesg --ctime | grep -Ei 'ov5647|cfe|pisp|csi|stream|i2c' | tail -100
```

## 4. Decode a RAW frame

After the HSCAP bundle is finalized successfully:

```bash
build/hscam-export inspect qualification/work/ov5647-640x240-initial.hscap

build/hscam-export image \
  qualification/work/ov5647-640x240-initial.hscap \
  --frame 0 \
  --output qualification/work/ov5647-640x240-frame0.png
```

Review the rendered PNG for real scene structure, aspect ratio,
correct Bayer phase, row validity, black/missing areas, line shifts
and repeated rows. Use a test scene with edges and a moving object.
A plausible PNG is **necessary but insufficient** evidence of high FPS.

## 5. Only then increase the requested FPS

If and only if the first conservative capture works, measure a short
`--discard-payload` run with a smaller frame duration, inspect actual
sensor timestamps and confirm no drops or CSI-2 errors. The theoretical
candidate ceiling is approximately `178.96 FPS` based on the driver's
`pixel_rate/HTS/VTS`, **not a measurement**.

Avoid launching the full automatic FPS-search campaign before a short
unpacked RAW run and visual verification.

## 6. Rollback if camera streaming fails

The working stock module is preserved at:

```text
/lib/modules/6.18.50+rpt-rpi-2712/kernel/drivers/media/i2c/ov5647.ko.xz
```

Stop all camera applications. If the Pi remains reachable:

```bash
sudo bash kernel/ov5647/manage-module-override.sh unstage
bash kernel/ov5647/manage-module-override.sh status
```

A **separate explicit reboot** is needed to restore the packaged stock
driver if the experimental module is already loaded. If the Pi cannot
boot or be reached, recovery from another boot medium may be required.

## Reporting

Keep all host outputs under ignored `qualification/work/` and post
configuration, measured FPS, gaps/overruns, selected RAW format, PNG
appearance, and any relevant CFE errors before declaring H5 hardware
capture validated. Do not commit `ov5647.ko`, HSCAP blobs or generated
kernel sources.

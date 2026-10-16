# OV5647 experimental 640×240 — H5 candidate revision 1

**Status: source transformer committed; NOT compiled or hardware validated.**
This is a genuine experimental *sensor driver mode-table candidate*.
It is neither an ISP crop nor a measured 180-FPS claim. Do not install the
generated module until its compilation, runtime behavior and rollback plan
have been reviewed. The user's active stock driver remains untouched.

## Pinned baseline and provenance

Only the exact source extracted from Raspberry Pi OS kernel package
`linux-source-6.18=1:6.18.50-1+rpt1` is accepted by the transformer.

- Running kernel: `6.18.50+rpt-rpi-2712`
- Sensor: `ov5647` at I²C `10-0036`
- Module: `/lib/modules/6.18.50+rpt-rpi-2712/kernel/drivers/media/i2c/ov5647.ko.xz`
- Stock driver SHA256:
  `da354174e9a9aba57fb59bd72650c199a7ab5fc03b3e1cdc65557a35c571a75c`
- Target: 640×240 true 10-bit Bayer sensor-output readout
- Historical reference:
  [raspiraw `tools/640x240`](https://github.com/raspberrypi/raspiraw/blob/master/tools/640x240),
  [`ov5647_mode7`](https://github.com/raspberrypi/raspiraw/blob/master/ov5647_modes.h)
- Current driver reference:
  [Raspberry Pi Linux OV5647](https://github.com/raspberrypi/linux/blob/rpi-6.18.y/drivers/media/i2c/ov5647.c)
  (for comparison only; patching is guarded by the **packaged source SHA**).

The historical `raspiraw` script uses mode 7, shortens output height,
specifies vertical increment 0x17 (and on that firmware/MMAL path requests
180 FPS). The Linux driver's VGA timing differs and applies HTS/VTS
through standard V4L2 controls. The conversion is therefore **an engineering
candidate, not a byte-for-byte port** of the original capture pipeline.

## Candidate register/timing profile

The generator copies the *entire unmodified VGA register sequence*, makes
the following constrained changes only in a separate 640×240 copy, and
appends a new fifth mode-table entry:

| Register / field | Candidate | Reason |
| --- | --- | --- |
| `0x3036` | `0x69` | Historical mode 7 faster PLL setting |
| `0x3814` | `0x71` | Historical mode 7 horizontal increment |
| `0x3815` | Stock `0x35` | Retains stock effective vertical subsampling |
| `0x3806:0x3807` | `0x03df` | Hypothesized 992 source-addressed rows including overscan |
| `0x380a:0x380b` | `0x00f0` | Set 240 RAW output rows |
| `0x380c:0x380d` | `0x073c` | 1,852 horizontal pixel clocks |
| `0x380e:0x380f` | `0x0108` | 264 total vertical lines (240 + 24 minimum vblank) |
| `0x370c` | `0x0f` | Historical mode 7 value |
| `0x4837` | `0x17` | Historical mode 7 MIPI clock-period setting |
| Mode `pixel_rate` | 87,500,000 | Faster clock candidate from historical mode/modern driver |
| Mode `link_freq_index` | `FREQ_INDEX_FULL` | Advertised 218.5 MHz for full-rate CSI path |
| Mode `crop` | 2560×960 | Candidate active sensor readout corresponding to 4× reduction |
| Mode `hts` / `vts` | 1852 / 264 | Matches programmable H/V blanking |

Predicted nominal frame rate from V4L2 timing fields:

```text
87,500,000 pixel clocks/s ÷ (1,852 clocks/line × 264 lines/frame)
≈ 178.96 FPS
```

**The readout end line, effective crop metadata, CSI timing and Bayer
orientation have NOT been validated on the Pi 5.** In particular, the 32
overscan rows are inferred from the stock VGA register range, not from a new
sensor readout trace. A 640×240 image successfully negotiated by libcamera
will be necessary but not sufficient evidence: the CFE output, timestamps,
no-drop statistics and visual samples must agree.

## Read-only source generation, then compile-only

Run from the repository root:

```bash
git pull origin dev
python3 -m unittest discover -s kernel/tests -v
python3 kernel/ov5647/prepare-experimental-module.py
```

The generator verifies the audited SHA256 before writing anything. It
creates only files inside the ignored
`kernel/work/ov5647-6.18.50/experimental-module/`, notably
`ov5647.c` and `Makefile`. It refuses to overwrite an existing generated
source tree so manually reviewed diffs are never silently replaced.
The original `linux-source-6.18` tree and loaded kernel module are unchanged.

For a **compile-only** check, if your installed kernel headers correspond
exactly to the currently running kernel:

```bash
bash kernel/ov5647/build-experimental-module.sh
```

This uses `make -C /lib/modules/$(uname -r)/build M=<work> modules`.
If matching prepared headers aren't installed, it exits without changing
the system. The output is a local `ov5647.ko` with module metadata
`hscam_experimental=ov5647-640x240-rev1-unvalidated`; it is **not**
installed or loaded. Successful compilation is not successful capture.

## Do not install or test live yet

The normal driver is in use by the Pi's libcamera camera stack. Do not:

- `insmod` the candidate over the existing bound `ov5647` driver;
- run `rmmod`/`modprobe -r` while libcamera has the sensor open;
- copy `ov5647.ko` into the system module tree or invoke `depmod`;
- write registers over userspace I²C to a kernel-owned sensor.

The next gate is review of a compiled `modinfo` and matching header
version, and a **separate opt-in installation/recovery design** before
live capture. Stock mode behavior must be preserved; exactly one driver
must bind the sensor at a time.

See [kernel integration](../../docs/kernel-driver-integration.md) and
[H5 acceptance criteria](../../docs/experimental-high-fps-roadmap.md).

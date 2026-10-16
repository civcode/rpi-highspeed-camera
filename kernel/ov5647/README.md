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

## Pi 5 experimental boot observed

The experimental module was successfully staged and activated on the
Raspberry Pi 5 on 2026-10-16. libcamera enumerated a fifth OV5647 mode,
`640×240-SGBRG10_CSI2P`, with mode ID `3fc920b29f2defb0`.
**No actual 640×240 RAW frame or high-FPS measurement has been recorded
yet.** See the [first-boot RAW validation procedure](first-boot-validation.md)
for CMake rebuilding (`hscam-highspeed` was missing), initial 30-FPS
capture, preview and rollback.

## Safe module override: opt-in, reboot-based

**User hardware evidence:** the Pi built `ov5647.ko` successfully
for `6.18.50+rpt-rpi-2712` and `modinfo` reported the experimental
profile marker. Six host unit tests passed. The compiled module SHA-256
was `f8a5b3a6a85b5f1da9d31e95716bdef40d0bc0ce4003620f577de16266217777`.

The alternative to a live `rmmod`/`insmod` swap is a **staged override**:

1. Keep packaged `/lib/modules/6.18.50+rpt-rpi-2712/kernel/drivers/media/i2c/ov5647.ko.xz` intact.
2. Copy **only** the checksum-pinned candidate into
   `/lib/modules/6.18.50+rpt-rpi-2712/updates/hscam/ov5647.ko`.
3. Refresh the module dependency index with `depmod`.
4. Verify the module resolver selects the new `.ko` before *any* reboot.
5. After an explicit reboot, the kernel may load the replacement. Confirm
   whether the camera actually advertises 640×240 before capture.

The helper intentionally changes neither the current live driver nor the
boot configuration. **Staging does modify the installed module index and
changes which driver will be selected after reboot.** If camera-driver
initialization is early in the boot process or a failing driver destabilizes
the board, local recovery access may be necessary. It is not a guarantee
against driver or sensor failure.

### Step 1 — inspect without root

Run from the project root:

```bash
git pull origin dev
python3 -m unittest discover -s kernel/tests -v
bash -n kernel/ov5647/manage-module-override.sh
bash kernel/ov5647/manage-module-override.sh status
bash kernel/ov5647/manage-module-override.sh check
```

The `check` subcommand is read-only and refuses to proceed if the kernel
release, module SHA-256/metadata, existing override, resolver or I²C driver
binding do not match the audited state.

Before staging, the resolver should point at the **stock `.ko.xz`**
and `Override present: no`. Confirm that SSH/local-console access works,
that the system is recoverable from external boot media if necessary,
and that no automatic kernel update is underway.

### Step 2 — explicit optional staging, no reboot

**Only after reviewing the first status output** and deciding the risk
is acceptable:

```bash
sudo bash kernel/ov5647/manage-module-override.sh stage
bash kernel/ov5647/manage-module-override.sh status
```

The script fails closed unless the running kernel, original packaged
module, experimental profile metadata and SHA-256 all match. It refuses
to overwrite an existing override. It checks that the resolver selects
the staged file and removes the file automatically if resolution fails.

The module remains *stock and loaded* until a subsequent reboot. No live
`rmmod`, `insmod`, I²C write or camera application is invoked.

You can undo staging **before reboot** using:

```bash
sudo bash kernel/ov5647/manage-module-override.sh unstage
```

### Step 3 — controlled test reboot (separate deliberate action)

When you have a known way back into the Pi and have reviewed the staged
module status, you may **choose** to reboot. The staging script will
never do this itself.

After the system boots, collect only read-only diagnostics first:

```bash
uname -r
bash kernel/ov5647/manage-module-override.sh status
modinfo -F hscam_experimental /lib/modules/$(uname -r)/updates/hscam/ov5647.ko
build/hscam-inspect --json
build/hscam-highspeed --json
```

A selected on-disk module path is **not sufficient evidence** that the
experimental module is currently bound. Inspect the live OV5647 I²C
binding, and require the new *sensor* 640×240 mode to appear in
`hscam-inspect` and `hscam-highspeed`. Only then proceed to
low-rate RAW capture and subsequently isolated high-FPS probes. If the
camera is missing, kernel logs and media topology should be reviewed.
Do not immediately run a 179 FPS stress test.

### Step 4 — rollback

If the experiment fails, or you wish to stop testing:

```bash
sudo bash kernel/ov5647/manage-module-override.sh unstage
bash kernel/ov5647/manage-module-override.sh status
# Reboot separately to restore the packaged stock module.
```

The unstage command removes **only** the file whose checksum matches
this experiment; it never touches the packaged stock `.ko.xz`.
It runs `depmod` and checks the resolver again. If a replacement is
currently loaded, removing its file does not unload it: the stock driver
is used only **after reboot**.

If the Pi is inaccessible after reboot, recover using independent
boot/root filesystem access to remove the override and rebuild the
module dependency index. Do not assume a broken camera driver can
always be corrected over SSH.

### Conflicts to avoid

- Never load two `ov5647` drivers simultaneously.
- Never force-unload an in-use camera driver.
- Never replace the stock package-managed module file.
- Never use userspace I²C register writes while the kernel driver owns it.
- Never interpret predicted 178.96 FPS as a measurement.

See [kernel integration](../../docs/kernel-driver-integration.md) and
[H5 acceptance criteria](../../docs/experimental-high-fps-roadmap.md).

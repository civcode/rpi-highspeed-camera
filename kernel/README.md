# Experimental Linux sensor drivers

This tree is reserved for **opt-in, reviewable extensions** to the Raspberry
Pi Linux V4L2 sensor drivers, written in C. The current repository does
**not** include a custom OV5647 driver or claim 180–750 FPS on Pi 5.

See [kernel driver integration](../docs/kernel-driver-integration.md) and
[experimental development roadmap](../docs/experimental-high-fps-roadmap.md).

## Before modifying the Pi

Record the current state (read-only, no sudo needed for most files):

```bash
bash kernel/tools/preflight.sh > kernel-preflight.txt
```

Also capture `media-ctl -p`, exact kernel/source commit, `libcamera`
version, matching `/boot/firmware/config.txt` overlays, and a normal RAW
camera capture. Export/backup the boot configuration separately.

`kernel/tools/preflight.sh` does **not** bind/unbind devices, poke I²C,
write registers, build a module or modify the boot partition. It checks
what is currently bound and what compiler/kernel version must be targeted.

## Pinned Pi 5 / OV5647 environment (2026-10-16)

The actual development Pi reported:

| Property | Value |
| --- | --- |
| Board | Raspberry Pi 5 Rev 1.1, ARM64 |
| Running kernel | `6.18.50+rpt-rpi-2712` |
| Debian/Raspberry Pi kernel package | `6.18.50-1+rpt1` |
| libcamera | `0.7.2` |
| Kernel I²C driver | `ov5647`, bound to `10-0036` |
| V4L2 sensor subdevice | `/dev/v4l-subdev2` |

**Pinned kernel source is essential:** the moving `rpi-6.18.y` branch
may no longer correspond to this packaged binary. The Raspberry Pi package
archive lists `linux-source-6.18` **version `6.18.50-1+rpt1`**.

Run this **non-root** command from the project checkout to fetch and
unpack the matching Debian-patched Linux source and record a SHA256 of
the stock OV5647 driver:

```bash
bash kernel/tools/prepare-ov5647-source.sh
```

This verifies the running kernel release and fails if the exact matching
source package isn't available from the configured apt indexes. It
downloads approximately 150 MB of source; it **does not install a kernel**.
It creates files under `kernel/work/`, which should be excluded from Git.

The next review step is to compare the installed kernel build configuration
and driver source before modifying an experimental mode table. A read-only
audit is available and checks the stored stock SHA256, actual I²C driver
binding, kernel configuration and current OV5647 source mode table:

```bash
bash kernel/tools/audit-ov5647-source.sh | tee ov5647-source-audit.txt
```

The audit is informational. It does not compile, install, bind/unbind, or
program a sensor driver. Retain the output for the first experimental patch.
This output file is machine-specific, so keep it out of source commits.

Additional manual diagnostic commands:

```bash
dpkg-query -W 'linux-image-6.18.50+rpt-rpi-2712' 2>/dev/null || true
modinfo ov5647 2>/dev/null | head -25 || true
media-ctl -p -d /dev/media0
v4l2-ctl -d /dev/v4l-subdev2 --all
```

Media-controller numbering is dynamic: select the `/dev/mediaN` containing
`ov5647 10-0036`, not necessarily `/dev/media0`. These queries are
read-only and do not activate experimental capture. Preserve boot
configuration details, especially `kernel=`, `auto_initramfs` and active
camera overlays before trial installations.

Building and booting an alternate kernel is a **separate, explicit step**.
Raspberry Pi's current kernel documentation warns that `auto_initramfs`
pairs initramfs filenames with selected custom kernel filenames; ensure
the boot initramfs and module tree match or the system may not boot.

Sources:
- https://archive.raspberrypi.com/debian/pool/main/l/linux/
- https://www.raspberrypi.com/documentation/computers/linux_kernel.html

## First OV5647 experimental driver milestone

A reviewable **unvalidated** 640×240 candidate source transformer is now
available at [`kernel/ov5647/`](ov5647/README.md). It creates an isolated
`ov5647.c` with a new mode entry only when the stock source hash matches the
user's tested Pi 5 package. It includes Python offline regression tests and a
compile-only helper, **but no live module installation, module replacement,
or measured high-FPS results**.



The first actual patch must be based on the **matching** running Raspberry
Pi kernel source, not indiscriminately applied to a different branch.
Patch `drivers/media/i2c/ov5647.c` by adding an opt-in reduced-height
mode whose full sensor register table comes from vetted OV5647 references.

The mode must account for at least:

- Sensor readout region `X_START/Y_START/X_END/Y_END` and output size
  registers `0x3800`–`0x380b`;
- HTS/VTS timing `0x380c`–`0x380f`;
- horizontal/vertical subsampling or increment settings
  (including `0x3814/0x3815` where relevant);
- PLL/link rate, pixel rate, Bayer code, blanking/exposure, CSI-2
  packet width/height and libcamera negotiation;
- power management, stop/restart, and a stock mode fallback.

These are **register categories**, not an executable patch or safe
arbitrary values. Do not make direct userspace I²C writes while the
stock driver is bound.

Build/install on a **separate boot-selectable kernel** with matching modules
and the working stock image intact. The first hardware test is a real
640×240 reduced-height RAW capture with correct CFE dimensions, timing
metadata and a decodable PNG. See H5 for the complete acceptance gate.

## Binding conflicts

One driver binds each physical I²C camera device. Do not load/bind a
second driver for the same Device Tree node. A separate experimental
module can be installed side-by-side on disk, but Device Tree/driver
matching must select **one** at boot; unloading an in-use sensor driver
while libcamera is active is not a supported deployment method.

## Kernel source references

- https://github.com/raspberrypi/linux/blob/rpi-6.18.y/drivers/media/i2c/ov5647.c
- https://github.com/raspberrypi/linux/blob/rpi-6.18.y/drivers/media/i2c/imx296.c
- https://github.com/raspberrypi/raspiraw

No prebuilt kernel modules or driver binaries are distributed here.

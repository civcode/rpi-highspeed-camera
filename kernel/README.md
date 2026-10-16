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

## First OV5647 experimental driver milestone

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

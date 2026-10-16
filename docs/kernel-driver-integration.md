# Experimental sensor driver integration

## Ownership and conflict model

Raspberry Pi camera sensors are normally described in Device Tree and bound
by the Linux I²C subsystem to one sensor driver. **Do not load a second
driver that controls the same sensor at the same time.** Two drivers contending
for I²C/registers, GPIO/reset, clocks and regulators will not create a valid
high-FPS capture path.

Prefer a patched **existing** sensor driver in an opt-in alternate kernel
for initial OV5647 work. Later a distinct driver/module can coexist on disk,
but must have a deliberate Device Tree match/selection: it does not co-own
the live sensor. A bootable stock kernel must remain available for rollback.

Kernel sensor drivers use **C** and the V4L2 subdevice interface. The C++20
library runs in userspace and never assumes privileged register access.

## Reuse the stock Raspberry Pi 5 capture pipeline

```text
libhscam C++20 → libcamera → Raspberry Pi PiSP pipeline → RP1 CFE
                     ↓ standard V4L2/media ioctls
               ONE sensor driver (C)
                     ↓ I²C writes + CSI-2 data
               OV5647 / other sensor
```

Start with the existing sensor driver's regulators, clocks, request lifecycle,
runtime PM, read-only metadata and ordinary mode fallback. Experimental
sensor modes must have consistent register tables **and** exposed V4L2 mode
size, Bayer code, pixel rate, link frequency, blanking, exposure/timing
limits. libcamera and the CFE must agree about the actual CSI-2 packet size
and format; output scaler dimensions alone are insufficient.

## Verified interface facts in upstream Raspberry Pi kernel

The inspected `rpi-6.18.y` implementations of OV5647, IMX219, IMX708,
IMX477 and IMX500 expose `.get_selection` but **no `.set_selection`**.
IMX296 exposes both; this makes its current V4L2 crop route the reference
without a patched driver.

OV5647 `.set_fmt` chooses the nearest entry in its static
`ov5647_modes[]` table. Its `get_selection` reports static mode crop. A
`VIDIOC_SUBDEV_S_SELECTION` request cannot invent a new 640×64 readout.
The earlier `raspiraw` Bash scripts called C code that programmed OV5647
registers via I²C and used legacy MMAL `vc.ril.rawcam`; they did not
simply ask the modern sensor driver to crop.

Sources:
- https://github.com/raspberrypi/linux/blob/rpi-6.18.y/drivers/media/i2c/ov5647.c
- https://github.com/raspberrypi/linux/blob/rpi-6.18.y/drivers/media/i2c/imx296.c
- https://github.com/raspberrypi/raspiraw

## Versioning and rollout

1. Capture `uname -a`, `/proc/version`, running modules, Device Tree
   camera node/compatible value and `media-ctl -p` output.
2. Pin the exact Raspberry Pi kernel Git commit corresponding to that
   running kernel. Do not apply a patch based on an unrelated branch.
3. Build an alternate kernel/image **without overwriting the working
   installation**. Keep matching modules and a boot configuration that can
   select the stock kernel.
4. Add only one opt-in sensor high-FPS mode at a time; inspect source
   diff, `modinfo`, device binding and boot logs.
5. Confirm ordinary stock capture as rollback regression; then request the
   experimental mode through libcamera/V4L2 and check actual CSI geometry.
6. Log the driver commit/hash with every high-speed qualification result.
7. If the camera fails to start, select the unmodified boot kernel and
   revert driver changes before further diagnosis.

A user's kernel version, compiler, firmware and secure-boot setup may alter
the exact installation commands; therefore no universal script that
automatically replaces the boot kernel is provided.

## How to prove a sensor readout hack

- Driver reports the experimental readout geometry and timing.
- V4L2 format readback and libcamera negotiation agree.
- Sensor/CFE log or independent subdevice state confirms true ROI/reduced
  lines; `ScalerCrop` must not count.
- A correctly decoded frame proves the Bayer path survived.
- Multiple independent captures validate rate, loss and jitter; measured FPS
  is never inferred from `--fps` alone.
- Driver version and source tree are attached to the result.

## Next kernel implementation gate

See [H5 in the active roadmap](experimental-high-fps-roadmap.md) for the
OV5647 mode-table port and required hardware validation. This document
specifies the conflict-safe rollout, **not** a claim that an experimental
driver has already been compiled or installed.

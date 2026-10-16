# Project goals and scope

> **Current charter — supersedes older v0.1 scope and release criteria.**
> The historical v0.1 documents remain valuable as records of the existing
> capture implementation but do not constrain experimental driver work.

## Mission

Build a clean C++20 library and repeatable hardware experimentation platform
that discovers, configures, captures, and **classifies genuinely high-frame-rate
sensor readout** on **all six official Raspberry Pi camera sensor families**:
OV5647, IMX219, IMX708, IMX477, IMX296 and IMX500.

Reproduce the *capabilities* demonstrated historically by `raspiraw`,
experimental sensor modes, and `GScrop`—not their obsolete MMAL dependencies
or shell-script-based architecture. Existing advertised/stock frame rates are
**regression baselines only**, not the central research objective.

## Three independent layers

1. **Kernel sensor drivers (C):** one driver owns each physical sensor. The
   stock driver, a deliberately selected experimental replacement, or a
   compatible patched driver programs the sensor registers and advertises
   coherent geometry, Bayer format, pixel/link rates, blanking and controls to
   V4L2. The Pi 5's CSI-2 receiver (RP1 CFE) and libcamera/PiSP pipeline
   remain in place where compatible.
2. **`libhscam` (C++20):** runtime discovery, strategy selection, requested
   versus negotiated readout, capture and zero-copy frame leases, statistics,
   HSCAP persistence, and immutable evidence. It must **not** secretly poke
   registers via userspace I²C while the kernel owns the sensor.
3. **Experiment/qualification engine (C++):** safe, sensor-specific *candidate
   generation* followed by bounded, isolated hardware probes, objective FPS
   and loss measurements, recovery, reproducible reports and visual evidence.

Driver details are sensor-specific; capture/result semantics remain generic.
The same ABI is not forced onto different physical sensors. Third-party
cameras can use the generic path where possible.

## High-FPS mechanisms

- **Native mutable sensor ROI** (e.g. IMX296): V4L2 subdevice TRY/ACTIVE
  selection with post-libcamera re-verification.
- **Experimental driver mode tables** (initial OV5647 approach): validated
  register sequences define reduced-height readouts, binning/skipping and
  coherent clock/blanking/CSI link data, exposed as genuine sensor modes.
- **Validated dynamic driver ROI/timing** (later): optional sensor-specific
  V4L2 selection and control support with documented safety constraints.
- **Stock fixed modes:** baseline-only; never label ISP `ScalerCrop` or
  output scaling as high-speed sensor-window readout.

## Classification contract

Distinguish **capability discovery** from **hardware proof**:

- `native_roi_available`: current driver advertises mutable sensor ROI;
  geometry/FPS still need verification.
- `requires_driver_extension`: a known official family without mutable ROI
  or recognized experimental-sized modes advertised by the running driver.
- `experimental_modes_advertised`: exact historical/reference readout-sized
  modes appear in the driver's enumeration. Their presence is **not** proof
  of a driver modification, actual sensor readout or achieved FPS.
- `uncharacterized`: no validated strategy known for this detected sensor.
- `experimentally_validated`: can be claimed only for a particular
  kernel/module revision, sensor, board, readout configuration, RAW encoding,
  and measured lossless run with visual evidence.

An experimental driver must be independently identifiable by the running
system. Never infer that a driver is experimental merely because the sensor
model is recognized, or claim an FPS based only on published experiments.

## Reproducibility, security and lifecycle

- Kernel patches must target a named kernel base revision and be reviewable,
  deterministic and **opt-in**. Do not overwrite the user's boot configuration
  or build modules implicitly.
- Exactly one sensor driver may bind each I²C device at a time. Leave a
  bootable stock-kernel path and document rollback.
- Experiments require bounded geometry, duration, requested FPS and timeout;
  failures must never trigger arbitrary register sweeps.
- Use hardware timestamps and sequence numbers, classify frame loss and jitter,
  reject silent sensor-mode clamping, retain lossless provenance.
- RAW previews, PNG and slow-motion exports verify content but are **not**
  substitutes for timing evidence.
- Do not silently report a baseline mode as a successful high-FPS hack.
- Host tests can verify planning, parsing, and policy; only real hardware
  capture can validate sensor/CSI settings and achieved FPS.

## Existing assets retained

`Context`, `Camera`, `CaptureSession`, HSCAP, `hscam-export`,
the qualified RAW Bayer path, the resume/recovery machinery and official-camera
inventory remain valuable. Ordinary-mode qualification becomes a small
compatibility/regression campaign, not a release-completion milestone.

## Key documents

- [Experimental high-FPS roadmap](experimental-high-fps-roadmap.md)
- [Kernel driver integration](kernel-driver-integration.md)
- [Camera strategy and classification](camera-support-and-crop-testing.md)
- [High-FPS capture methods](high-fps-capture-method.md)
- [Qualification result contract](qualification-results-v1.md)
- [Legacy v0.1 roadmap](v0.1-implementation-roadmap.md) — historical baseline

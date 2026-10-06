# Camera support and automated crop testing

## Purpose

This document defines how `rpi-highspeed-camera` should support Raspberry Pi camera sensors generically and how it should automatically discover, exercise, benchmark, and report crop/mode performance.

The project must not assume that every sensor behaves like IMX296. The high-FPS IMX296 experiments are a valuable reference implementation, but the public API and test infrastructure should be camera-agnostic.

The core rule is:

> Detect capabilities from libcamera, the media graph, and V4L2 at runtime. Use sensor-specific knowledge only as an optional compatibility layer, never as the primary source of truth.

This allows the project to work with current official Raspberry Pi cameras, upstream-supported third-party sensors, and future cameras without requiring the core architecture to be rewritten.

---

## Support target

### Official Raspberry Pi cameras

As of 2026, Raspberry Pi's camera stack supports these official sensor families:

| Product | Sensor |
| --- | --- |
| Camera Module v1 | OV5647 |
| Camera Module v2 | IMX219 |
| Camera Module 3 | IMX708 |
| High Quality Camera | IMX477 |
| Global Shutter Camera | IMX296 |
| AI Camera | IMX500 |

NoIR, Wide, autofocus, and lens variants that use the same sensor should normally share the same low-level capture backend, although tuning, optics, autofocus, and metadata may differ.

### Third-party cameras

The project should not maintain a closed allow-list.

If a camera is exposed through libcamera and the Linux media/V4L2 stack, the generic capture path should attempt to support it. Raspberry Pi currently documents support for several third-party sensors in addition to the official modules, and the set changes over time.

Examples include IMX290/327/378/519, OV9281, and several ST global-shutter sensors. Raspberry Pi engineers have also documented additional upstream-supported sensors.

The correct architectural boundary is therefore:

```text
libcamera-visible camera
        |
        +--> generic capture support
        |
        +--> media/V4L2 capability probing
        |
        +--> optional sensor-specific quirks
```

rather than:

```text
if sensor == imx296:
    ...
elif sensor == imx219:
    ...
```

---

## What "support all models" means

There are several distinct capabilities and they must not be conflated.

A camera can be supported even if it does not support arbitrary high-FPS sensor cropping.

For every detected camera, the program should report:

- camera identity and libcamera ID;
- sensor model, when discoverable;
- media device and subdevice;
- pixel-array size and active areas;
- advertised sensor modes;
- pixel formats and bit depths;
- maximum advertised FPS per mode;
- whether the sensor subdevice exposes crop/selection controls;
- whether an arbitrary crop can be set exactly;
- whether the driver rounds/adjusts requested crops;
- minimum discovered crop;
- crop alignment/granularity discovered by probing;
- frame-duration controls;
- available raw and processed stream formats;
- tested maximum stable FPS;
- whether a requested configuration is sensor crop, sensor mode crop, or ISP/scaler crop.

A sensor that only exposes fixed modes is still fully supported for those modes.

### Capability levels

The implementation should classify each camera at runtime:

| Level | Meaning |
| --- | --- |
| `capture` | libcamera can stream frames |
| `modes` | advertised sensor modes can be enumerated/tested |
| `sensor_crop_query` | crop bounds/current crop can be queried |
| `sensor_crop_try` | candidate crops can be negotiated with TRY state |
| `sensor_crop_set` | active sensor crop can be changed |
| `sensor_crop_exact` | exact requested rectangles can be accepted |
| `fps_control` | frame duration can be requested |
| `raw` | raw sensor frames can be streamed |
| `processed` | ISP output can be streamed |

These are runtime capabilities, not assumptions attached to a model name.

---

## Important crop terminology

The test harness must distinguish three things.

### 1. Advertised sensor mode

A driver may expose several predefined sensor modes. A mode can already contain binning, skipping, or a fixed sensor crop.

Example:

```text
sensor pixel array
        |
        +--> predefined 640x480 high-FPS mode
```

This is a real sensor/readout mode even though the application did not set an arbitrary crop.

### 2. Sensor/subdevice crop

A mutable V4L2 subdevice crop changes the rectangle the sensor or sensor-side pipeline reads.

This is the mechanism exploited by the IMX296 GScrop work.

It is queried/set through the V4L2 subdevice selection API where supported:

- `V4L2_SEL_TGT_NATIVE_SIZE`
- `V4L2_SEL_TGT_CROP_BOUNDS`
- `V4L2_SEL_TGT_CROP_DEFAULT`
- `V4L2_SEL_TGT_CROP`

The kernel API explicitly permits drivers to reject unsupported targets or to adjust requested rectangles to hardware alignment constraints.

### 3. ISP/scaler crop

libcamera's `ScalerCrop` operates in the camera processing pipeline and does not necessarily reduce sensor readout.

It is useful for framing, but it must never be reported as evidence that a sensor ROI increased the physical sensor frame rate.

Every test result must say which crop type was used.

---

# Architecture

## Camera discovery

The program should begin with libcamera enumeration and then associate each libcamera camera with the underlying media graph.

Suggested objects:

```text
CameraManager
  list()

CameraDescriptor
  libcamera_id
  model
  pipeline_handler
  pixel_array
  active_areas
  system_devices
  sensor_entity
  sensor_subdev
  media_device
  modes[]
  capabilities
```

Do not identify cameras only by `/dev/mediaN`, I2C address, or a model string. Device numbering and bus paths vary between Pi models, camera ports, kernels, and overlays.

Where available, use libcamera's system-device metadata plus media-controller enumeration to establish the relationship.

## Generic sensor interface

The low-level code should expose a generic interface:

```cpp
class SensorControl {
public:
    virtual SensorInfo inspect() = 0;

    virtual CropResult tryCrop(Rect requested) = 0;
    virtual CropResult setCrop(Rect requested) = 0;
    virtual Rect currentCrop() = 0;

    virtual std::vector<SensorMode> modes() = 0;
    virtual void restore(const SensorState &state) = 0;
};
```

A sensor-specific subclass should not be necessary when the kernel driver correctly exposes the standard media/V4L2 APIs.

Optional quirks can exist for:

- driver-specific ordering requirements;
- known mode-switch workarounds;
- link-frequency controls;
- kernels with known behavior changes;
- sensors requiring a restart after a failed configuration.

Quirks must be keyed from detected driver/version/capabilities and kept separate from the generic capture pipeline.

## Capture engine

The C++ capture engine remains generic:

```text
CameraDescriptor
      |
      v
SensorControl ----> negotiate mode/crop
      |
      v
libcamera configuration
      |
      v
request/buffer pool
      |
      +--> CaptureStats
      |
      +--> bounded processing queue
      |
      +--> ring buffer / writer
```

The camera-specific part should end before frame processing begins.

---

# Automated crop tester

The repository should include a hardware test executable, tentatively named `hscam-test`.

It should work locally on the Raspberry Pi and must not require GitHub Actions.

Suggested commands:

```bash
# Inventory all cameras and capabilities
hscam-test inspect

# Test one advertised mode
hscam-test mode --camera 0 --mode 2 --duration 10s

# Test one arbitrary sensor crop
hscam-test crop --camera 0 --crop 0,496,1456,96 --fps auto

# Sweep centered crop sizes
hscam-test sweep --camera 0 --geometry centered

# Sweep every exact size supported by inferred alignment
hscam-test sweep --camera 0 --geometry all-sizes

# Also test crop positions for every accepted size
hscam-test sweep --camera 0 --geometry exhaustive

# Generate Markdown again from stored JSON
hscam-test report results/run.json
```

The exact CLI may change, but these capabilities are requirements.

---

## Testing an arbitrary requested crop

For a single requested rectangle `x,y,width,height`, the test should always produce a result record.

Possible outcomes:

- `pass`: exact crop accepted and stable;
- `adjusted`: driver accepted the request but rounded/changed the rectangle;
- `unsupported`: sensor crop is not mutable or rectangle cannot be represented;
- `unstable`: stream starts but misses the stability criteria;
- `timeout`: camera or pipeline failed to complete within the test timeout;
- `error`: configuration or capture returned an error;
- `recovery_failed`: the camera could not be restored after the test.

The result must contain both:

```text
requested_crop
negotiated_crop
```

because the V4L2 selection API allows drivers to adjust requests.

---

## Discovering crop capabilities

The Linux selection API exposes crop bounds, but there is no universal ioctl that enumerates every valid rectangle.

Therefore the tester must discover the valid geometry by probing.

### Step 1: inspect selection targets

For each relevant sensor pad, query:

```text
NATIVE_SIZE
CROP_BOUNDS
CROP_DEFAULT
CROP
```

Unsupported targets returning `EINVAL` are recorded as unsupported.

### Step 2: prefer TRY state

Where the driver supports it, use:

```text
which = V4L2_SUBDEV_FORMAT_TRY
```

to test candidate formats and crop rectangles without changing the active sensor state.

This should be the default discovery mechanism.

### Step 3: discover exactness and alignment

Probe candidate widths, heights, left offsets, and top offsets and compare the returned rectangle with the requested rectangle.

The tester should infer:

- minimum accepted width;
- minimum accepted height;
- width step/alignment;
- height step/alignment;
- left alignment;
- top alignment;
- whether constraints depend on the selected sensor format/mode.

Do not assume that all sensors require even coordinates or that one global step applies to every mode.

### Step 4: verify candidates actively

TRY-state acceptance is not proof that a configuration streams reliably.

Any candidate included in the final performance table must be applied as ACTIVE, followed by a real capture test.

---

# Sweep strategies

A four-dimensional exhaustive search over `x, y, width, height` can contain millions or billions of rectangles.

The tester should support exhaustive operation, but the default sweep should find useful high-FPS configurations efficiently.

## `advertised`

Test every sensor mode that libcamera advertises.

This must work even when arbitrary sensor cropping is unavailable.

## `centered`

Keep the crop centered and sweep all discovered valid width/height combinations according to the configured resolution policy.

This is the preferred high-FPS characterization because it removes position as a variable.

## `heights`

Use full or selected width and sweep every accepted crop height.

This is especially useful for sensors where vertical readout dominates maximum FPS.

## `all-sizes`

Test every discovered exact `width x height` combination but keep the rectangle centered.

## `positions`

For one requested size, test every valid crop origin.

## `exhaustive`

Test every exact valid origin and every exact valid size.

This mode must print the number of candidate configurations before starting and require a deliberate command-line option such as `--exhaustive`.

It should also accept:

- `--max-tests`;
- `--time-limit`;
- `--resume`;
- `--from-result`.

This makes an interrupted multi-hour or multi-day characterization resumable.

---

# Finding maximum stable FPS

Each crop/mode should have a measured maximum stable FPS, not just a requested FPS.

## Initial limit

Start from information supplied by libcamera/driver controls when available.

If the active mode reports a frame-duration range, use it as the first estimate.

For experimental sensor crops where the normal libcamera mode metadata is no longer authoritative, probe frame duration and measure the result.

## Search procedure

A recommended procedure is:

1. configure crop/mode;
2. warm up for a short period;
3. request a conservative frame duration;
4. measure achieved frame intervals;
5. progressively request shorter frame durations;
6. stop when the camera clamps, rejects the request, or fails stability criteria;
7. refine between the last stable and first unstable/clamped rate;
8. perform a final longer validation at the chosen maximum.

The implementation should detect silent clamping by comparing requested frame duration with sensor/request metadata and measured timestamps.

A rate is never considered valid solely because configuration succeeded.

---

# Stability criteria

Defaults should be configurable, but a strict result should require:

- no request failures;
- no application queue overruns;
- no unexplained sequence gaps;
- no frame interval equivalent to a skipped frame;
- achieved average FPS close to the delivered/negotiated frame duration;
- acceptable timestamp jitter;
- no thermal throttling introduced during the measurement;
- no writer back-pressure when testing storage.

A suggested default classification:

```text
PASS
  zero detected dropped frames
  zero queue overruns
  achieved FPS >= 99% of negotiated FPS
  no timeout/recovery event
```

For high-rate cameras, timestamp distributions should be saved rather than reducing everything to one average.

---

# Test isolation and recovery

Experimental sensor configuration can leave the camera pipeline in a bad state.

Each active crop benchmark should therefore run in an isolated worker process.

Suggested structure:

```text
hscam-test parent
    |
    +--> snapshot original sensor state
    |
    +--> worker(test case)
    |      configure
    |      capture
    |      emit raw result
    |      exit
    |
    +--> timeout / kill if hung
    |
    +--> restore camera state
    |
    +--> verify camera can stream baseline mode
    |
    +--> next test
```

The parent process should survive:

- worker crashes;
- libcamera exceptions;
- invalid crop errors;
- capture startup failure;
- test timeout.

If the camera cannot recover without a reboot, record that condition and stop the sweep. The program should not automatically reboot the machine.

---

# Preventing false results

## Sensor crop vs scaler crop

A result is marked `sensor_crop=true` only when the sensor/subdevice state confirms the requested/negotiated readout rectangle.

Changing only `ScalerCrop` must be reported separately.

## Requested FPS vs achieved FPS

Always calculate FPS from timestamps.

## Requested dimensions vs buffer dimensions

Record:

- requested output size;
- negotiated stream size;
- crop rectangle;
- plane stride;
- plane length;
- total bytes per frame.

Never infer memory bandwidth only from nominal resolution.

## Warm-up frames

Exclude configurable warm-up frames from benchmark statistics.

## Thermal state

Record at least:

- temperature at start/end;
- throttling flags at start/end.

A test run affected by throttling should be tagged accordingly.

---

# Result format

The canonical output should be JSON so that results from multiple Pis/cameras can be aggregated.

The schema should be versioned.

Example:

```json
{
  "schema_version": 1,
  "run_id": "2026-10-06T19-30-00Z-imx296",
  "system": {
    "pi_model": "Raspberry Pi 5 Model B",
    "ram_bytes": 8589934592,
    "os": "...",
    "kernel": "...",
    "libcamera": "...",
    "temperature_start_c": 49.2,
    "temperature_end_c": 52.1,
    "throttled_start": "0x0",
    "throttled_end": "0x0"
  },
  "camera": {
    "libcamera_id": "...",
    "model": "imx296",
    "driver": "imx296",
    "pixel_array": [1456, 1088],
    "media_device": "/dev/media0",
    "sensor_subdev": "/dev/v4l-subdev0"
  },
  "test": {
    "kind": "sensor_crop",
    "requested_crop": [0, 496, 1456, 96],
    "negotiated_crop": [0, 496, 1456, 96],
    "requested_fps": 536.0,
    "duration_s": 10.0
  },
  "stream": {
    "format": "SBGGR10",
    "size": [1456, 96],
    "strides": [1824],
    "bytes_per_frame": 175104
  },
  "result": {
    "status": "pass",
    "frames": 5360,
    "achieved_fps": 535.98,
    "dropped_frames": 0,
    "queue_overruns": 0,
    "mean_interval_us": 1865.7,
    "p99_interval_us": 1868.1,
    "max_interval_us": 1871.2
  }
}
```

The exact numbers above are illustrative, not reference measurements.

## Additional outputs

Each run should produce:

```text
results/<run-id>/
    environment.json
    camera.json
    results.json
    results.csv
    report.md
    raw/
        <optional per-test timing data>
```

`results.json` is canonical.

`results.csv` is convenient for plotting/spreadsheets.

`report.md` is the human-readable summary and can be committed or attached to an issue.

---

# Markdown report

The generated Markdown report should start with the environment and camera fingerprint, followed by a sortable-style table such as:

```text
| Crop | Type | Format | Max stable FPS | Drops | MB/s | Status |
| --- | --- | --- | ---: | ---: | ---: | --- |
| 1456x96 | sensor | RAW10 | 535.98 | 0 | ... | PASS |
| 688x136 | sensor | RAW10 | 399.97 | 0 | ... | PASS |
| ... | ... | ... | ... | ... | ... | ... |
```

It should separately list:

- unsupported crops;
- adjusted crops;
- tests that caused timeouts;
- recovery failures;
- thermal/throttling warnings;
- tests limited by processing or storage rather than sensor timing.

This keeps failures visible instead of silently omitting them.

---

# Cross-camera result database

A useful long-term feature is a repository of result files generated by real hardware.

Suggested hierarchy:

```text
results/
  raspberry-pi-5/
    imx296/
    imx708/
    imx477/
    imx219/
    ov5647/
    imx500/
```

However, generated results should only be committed intentionally. Normal local benchmark runs should remain untracked by default.

Each result needs enough environment metadata to distinguish kernel/libcamera changes. A crop that works on one software version must not automatically be assumed valid on another.

---

# Model-specific expectations

These are starting expectations, not hard-coded limits.

## IMX296 / Global Shutter Camera

Reference sensor for arbitrary sensor-crop high-FPS testing.

Published experiments demonstrate very high frame rates with reduced readout height, including approximately 536 fps at 96 rows.

## IMX219 / Camera Module 2

Historical libcamera configurations have exposed a 640x480 mode around 206 fps. Older `raspiraw` experiments reached substantially higher rates through a different legacy path.

The modern implementation should first test current advertised modes and driver capabilities rather than assume old modes remain present.

## IMX708 / Camera Module 3

The driver exposes predefined high-frame-rate modes, and community work has experimented with modified sensor modes. Treat standard driver modes as supported; classify non-standard mode-table changes as a separate experimental backend.

## IMX477 / HQ Camera

Current Raspberry Pi engineer guidance states that arbitrary cropping is not supported by the IMX477 driver. On Pi 5, newer driver/link-frequency work can expose significantly higher FPS for predefined sensor modes.

Therefore this camera is an important example of why the project must support **mode benchmarking even when arbitrary sensor crop is unavailable**.

## OV5647 / Camera Module v1

Treat the current libcamera modes and V4L2 capabilities as authoritative. Do not depend on legacy camera-stack behavior.

## IMX500 / AI Camera

Support normal image-stream capture and mode characterization first.

The IMX500 has additional firmware and inference streams. Those should be reported as camera-specific capabilities, but the high-FPS crop tester must not require an inference workload.

---

# Implementation milestones

1. Generic libcamera camera inventory.
2. Media-graph association for each camera.
3. Generic V4L2 subdevice selection inspection.
4. Advertised-mode benchmark runner.
5. Single arbitrary-crop probe and result JSON.
6. TRY-state crop geometry discovery.
7. Centered crop sweep.
8. Maximum-stable-FPS search.
9. Markdown/CSV report generator.
10. Worker-process isolation and recovery.
11. All-size sweep.
12. Optional exhaustive origin + size sweep.
13. Cross-camera result aggregation.
14. Optional sensor-specific quirks only where the standard APIs are insufficient.

The first usable version should complete steps 1-10 before adding processing plugins or compression experiments.

---

# References

Raspberry Pi camera software and supported sensors:

- https://www.raspberrypi.com/documentation/computers/camera_software.html
- https://www.raspberrypi.com/documentation/accessories/ai-camera.html

Linux V4L2 selection APIs:

- https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/vidioc-g-selection.html
- https://docs.kernel.org/userspace-api/media/v4l/vidioc-subdev-g-selection.html
- https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/v4l2-selection-targets.html

libcamera properties:

- https://docs.libcamera.org/master/public-api/property__ids_8h.html

Raspberry Pi high-FPS references:

- https://stamm-wilbrandt.de/GS/
- https://forums.raspberrypi.com/viewtopic.php?t=345883
- https://gist.github.com/Hermann-SW/e6049fe1a24fc2b5a53c654e0e9f6b9c
- https://forums.raspberrypi.com/viewtopic.php?t=350491
- https://forums.raspberrypi.com/viewtopic.php?t=392835

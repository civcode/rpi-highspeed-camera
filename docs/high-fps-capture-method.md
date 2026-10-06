# High-FPS capture method for the Raspberry Pi Global Shutter Camera

## Status

This document specifies the capture method this repository intends to reproduce in a clean, maintainable implementation.

It is based on published Raspberry Pi forum experiments, Hermann Stamm-Wilbrandt's `GScrop` work, later Raspberry Pi 5 tests, and related high-frame-rate Raspberry Pi camera projects. The method has **not yet been independently validated by this repository**. The first implementation milestone should reproduce the published results before adding new processing or storage features.

## Goal

Use the Raspberry Pi Global Shutter Camera (Sony IMX296) at frame rates far above its normal full-frame rate by reducing the **sensor readout window**, while keeping the capture path compatible with the modern libcamera/rpicam stack.

The target implementation should eventually provide the same capability from a small C++ library/application, with optional Python bindings, instead of depending on shell scripts and a chain of command-line programs.

The important distinction is:

- **sensor crop / readout crop**: fewer sensor rows are read, reducing frame time and increasing the possible frame rate;
- **ISP/scaler crop**: the sensor still reads the original frame and pixels are discarded later, so this does not create the same frame-rate increase.

The high-FPS method depends on the first kind.

---

## Hardware and software assumptions

Primary target:

- Raspberry Pi 5
- Raspberry Pi Global Shutter Camera
- Sony IMX296
- current Raspberry Pi OS camera stack
- libcamera / rpicam-apps
- Linux media-controller and V4L2 subdevice APIs

Pi 4 is also relevant because the original demonstrations were performed there, but Pi 5 is the preferred development target because later tests showed more processing and larger cropped frames at the same high frame rates.

The official camera is a 1456x1088, 1.6 MP global-shutter camera. Raspberry Pi specifies exposure times as low as 30 us when there is enough light.

## Core mechanism

The core observation is that the IMX296 frame rate rises dramatically when the active sensor readout height is reduced.

The original `GScrop` tool changes the active format/crop on the IMX296 V4L2 sensor subdevice through the Linux media-controller interface. In simplified form, it performs an operation equivalent to:

```text
media-ctl -d /dev/mediaN --set-v4l2 \
  "'imx296 ...':0 [fmt:SBGGR10_1X10/WxH crop:(X,Y)/WxH]"
```

After that, `rpicam-vid` / `libcamera-vid` is started with the same output dimensions and a requested high frame rate.

For example, the published GScrop script configures a centered crop and then captures with denoising disabled:

```text
sensor (IMX296)
    |
    | sensor-level Bayer crop
    v
CSI-2 / receiver
    |
    +--> optional ISP conversion to YUV
    |
    v
application buffers
    |
    +--> processing
    +--> RAM ring buffer
    +--> encoder / storage
```

Reducing the number of rows reduces sensor readout time. This is why a 96-row image can run around 536 fps even when the crop is very wide.

### This is not ScalerCrop

A normal libcamera/Picamera2 `ScalerCrop` is not sufficient by itself. That control represents cropping in the processing pipeline; it does not necessarily reduce the number of sensor rows read.

The method must cause the IMX296 sensor subdevice itself to expose the smaller active frame.

---

## Published results to reproduce

The following results are useful reference points. They should be treated as external experimental results until reproduced in this repository.

| Crop / output | Reported rate | Platform / notes |
| --- | ---: | --- |
| 720x540 | ~116 fps captured | Early libcamera IMX296 experiment |
| 300x200 | ~293 fps | Pi 4, reported with 0 frame skips over the test |
| 128x96 | ~536 fps | Pi 4, GScrop, reported 0 frame skips |
| 224x96 | ~535 fps | Pi 5 examples; later processing pipeline averaged ~527 fps |
| 320x96 | ~536 fps | Pi 5, reported frameskipless |
| 688x136 | ~400 fps | Pi 5; a 60 s recording was reported with one skip |
| 1456x96 | ~536 fps | Pi 5, reported frameskipless at full sensor width |
| 96x88 | ~572 fps mode reported | Near the sensor's minimum crop; see limit below |

The especially important result is **1456x96 at 536 fps**: the method is not limited to a tiny horizontal strip. For this sensor and configuration, reducing vertical readout is the dominant lever.

### Sensor minimum

Raspberry Pi engineer Dave Stevenson / forum user `6by9` and engineer `naushir` have discussed the crop limits in the Raspberry Pi forums. In the specific IMX296 crop-limit discussion, Raspberry Pi engineer `naushir` states that the IMX296 will not operate with a crop below **96x88**. That is a sensor hardware limit, not just a userspace restriction.

Therefore, the project should not assume that arbitrarily small crops can push the IMX296 to 1,000 fps. Around 500-570 fps is the relevant range for the published small-height IMX296 modes.

---

## Proven shell-based sequence

The current reference implementation is GScrop. Its behavior is useful as an executable specification.

### 1. Locate the IMX296 media graph

The camera can appear on different `/dev/mediaN` nodes. GScrop historically tries media devices until a `media-ctl` operation succeeds.

A better implementation should enumerate the media graph and identify the entity whose name starts with `imx296`, rather than guessing device numbers or I2C bus identifiers.

### 2. Set the sensor subdevice format and crop

The sensor entity pad is configured as Bayer 10-bit:

```text
SBGGR10_1X10
```

with the requested width/height and crop rectangle.

The reference script requires even width and height and centers the crop by default.

### 3. Start capture with the same dimensions

The rpicam/libcamera application is then asked for the same width and height and the desired frame rate.

The successful high-FPS examples normally disable colour denoise:

```text
--denoise cdn_off
```

On Bookworm-era configurations GScrop uses `--no-raw` as a workaround. Later Picamera2 experiments similarly configure `raw=None`. This matters because asking libcamera for an additional raw stream can cause a different sensor configuration or otherwise interfere with the externally selected cropped mode.

This behavior must be tested carefully in the native implementation rather than assumed.

### 4. Use a short exposure

At 536 fps the frame interval is only about:

```text
1 / 536 s = 1.8657 ms
```

Exposure must fit inside the frame timing, and fast-moving subjects generally require much shorter exposure.

Published examples include:

- 29 us for a fast rotating propeller;
- 100 us for an airsoft pellet moving above 40 m/s.

Short exposures require strong illumination.

### 5. Verify timestamps and frame skips

Requested FPS is not proof of delivered FPS.

Every benchmark must use per-frame timestamps and report:

- total frames;
- capture duration;
- average FPS;
- frame-delta distribution;
- dropped/skipped frames;
- maximum frame interval;
- requested versus achieved FPS.

The existing work uses `ptsanalyze` or Raspberry Pi's `rpicam-apps/utils/timestamp.py`.

Our implementation should collect equivalent data directly from libcamera request metadata, preferably using the sensor timestamp when available, and should preserve sequence information so skips are detectable.

---

## Proposed native implementation

The repository should reproduce the method in two layers.

### Layer 1: C++ capture engine

C++ should own the high-rate data path.

Suggested components:

```text
MediaGraph
  discover IMX296 media device/entity/pad

Imx296Sensor
  query crop bounds
  set sensor format
  set crop rectangle
  validate resulting format

Camera
  configure libcamera streams
  set frame duration / exposure
  allocate buffers
  queue requests
  collect metadata

FramePipeline
  callback -> bounded queue/ring buffer
  optional processing stages
  writer / consumer

CaptureStats
  sensor timestamps
  frame intervals
  sequence gaps
  queue overruns
  processing latency
```

### Layer 2: optional Python API

Python should configure and control the camera, but it should not be required to copy or process every 500+ fps frame in Python code.

A future API might look like:

```python
import hscam

cam = hscam.Camera()
cam.configure(width=1456, height=96, fps=536, exposure_us=100)
cam.start()
```

The capture callback, ring buffer and performance-sensitive processing should remain in C++.

A 2025 community conversion of GScrop to Python demonstrates that the method can be controlled from Python/Picamera2, but that version still shells out to `media-ctl`. It is useful as confirmation of the control sequence, not as the desired final architecture.

---

## Direct media-controller/V4L2 implementation

The first clean implementation should remove the dependency on invoking `media-ctl` as a subprocess.

The Linux media-controller API should be used to:

1. enumerate `/dev/media*`;
2. enumerate media entities and pads;
3. find the IMX296 sensor subdevice;
4. open the associated `/dev/v4l-subdev*`;
5. query supported selection/crop bounds;
6. set the active subdevice format;
7. set the active crop/selection rectangle;
8. read the values back and verify them.

At the subdevice level this is expected to involve the normal V4L2 subdevice operations such as:

- `VIDIOC_SUBDEV_G_FMT` / `VIDIOC_SUBDEV_S_FMT`;
- `VIDIOC_SUBDEV_G_SELECTION` / `VIDIOC_SUBDEV_S_SELECTION`.

Do not hard-code `/dev/media3`, an I2C bus number, or an entity string such as `imx296 10-001a`. Published GScrop comments show that these values vary by board, camera connector and software configuration.

### Configuration ownership caveat

libcamera also configures the sensor as part of camera startup. Therefore the implementation must explicitly verify that the cropped sensor mode survives the complete libcamera configuration/start sequence.

The safest development sequence is:

1. reproduce the existing external-`media-ctl` behavior;
2. replace only the `media-ctl` call with equivalent ioctls;
3. compare media graph state and achieved FPS;
4. only then refactor deeper into libcamera-specific sensor configuration.

This keeps the first native version behaviorally equivalent to the proven method.

---

## Capture formats and processing

### Raw Bayer

The sensor outputs 10-bit Bayer data.

Keeping data in Bayer form is attractive for:

- scientific capture;
- custom image processing;
- simple brightness/motion/centroid algorithms;
- avoiding unnecessary debayer cost;
- preserving sensor data.

However, actual memory layout, stride and packing must be measured from the libcamera buffers rather than inferred only from `width * height * 10 / 8`.

### YUV420 through the Raspberry Pi ISP

Published Pi 5 work also demonstrates:

```text
GScrop -> rpicam-vid --codec yuv420 -> custom processing -> ffmpeg
```

At 224x96 and a requested 536 fps, a simple real-time brightness-processing pipeline was reported at about 527 fps.

That is valuable because it proves that useful per-frame work can be done without necessarily destroying the high capture rate.

YUV output has alignment/padding requirements. One published implementation notes that a requested width of 224 pixels is delivered with a 256-byte luma stride. Native code must always use the stride/plane metadata reported by the buffer configuration.

### Debayering

Debayering is not a storage optimization by itself.

A packed 10-bit Bayer pixel is much smaller than a 24-bit RGB pixel. Converting every frame to RGB can increase memory bandwidth and reduce the time that a RAM ring buffer can hold.

Therefore:

- do not debayer on the critical path unless the processing algorithm needs RGB;
- prefer Bayer-domain or luma-domain processing when possible;
- if display/preview is needed, produce it on a secondary/asynchronous path;
- if YUV is adequate, prefer the Pi ISP instead of a CPU debayer loop.

### Processing and storage should be decoupled from capture

The capture callback must never perform unbounded work.

Preferred structure:

```text
libcamera request completion
        |
        v
bounded zero/low-copy queue
        |
        +--> real-time processing worker
        |
        +--> RAM ring buffer / writer
        |
        +--> optional preview/encoder
```

If a consumer cannot keep up, the program must expose the overrun explicitly rather than silently blocking capture.

---

## Storage and bandwidth

High FPS makes memory and storage bandwidth part of the design.

For 1456x96 at 536 fps, the ideal packed RAW10 payload alone is approximately:

```text
1456 * 96 * 536 * 10 / 8 ~= 93.6 MB/s
```

Ideal, unpadded YUV420 is approximately:

```text
1456 * 96 * 536 * 1.5 ~= 112.4 MB/s
```

Actual buffer bandwidth can be higher because of stride, alignment, metadata, copies and container overhead.

This means a fast USB 3 SSD or NVMe device may be able to sustain the raw stream, while microSD storage often will not. The implementation should benchmark **sustained end-to-end writes while capturing**, not rely on a standalone disk benchmark.

RAM should be treated as a ring buffer that absorbs latency and allows pre/post-trigger capture, rather than automatically assuming that the entire recording must live in RAM.

---

## Validation plan

The first implementation milestone is successful only when it reproduces high frame rates **without hidden frame loss**.

### Minimum test matrix

Run at least:

1. 128x96 @ 536 fps
2. 224x96 @ 536 fps
3. 688x136 @ 400 fps
4. 1456x96 @ 536 fps

For each mode:

- capture at least 10 seconds;
- record every frame timestamp;
- count frame intervals near 1x, 2x, 3x expected duration;
- report dropped-frame percentage;
- report average and minimum/maximum interval;
- report CPU load;
- report buffer queue overruns;
- report output bytes/second;
- repeat with no processing;
- repeat with the intended processing stage enabled.

A longer 60-second 688x136 @ 400 fps run is useful for comparison with the published Pi 5 result.

### Environment data to record

Every benchmark result should include:

- Pi model and RAM size;
- Raspberry Pi OS release;
- kernel version;
- libcamera version;
- rpicam-apps version if used;
- camera firmware / detected sensor;
- sensor crop;
- requested stream format;
- actual stride and buffer sizes;
- exposure;
- requested FPS;
- storage target;
- thermal state / throttling status.

Published GScrop testing noted that frame skips became more likely after long Pi 5 uptimes, with rebooting used as a practical workaround. We should measure this rather than bake a reboot requirement into the design.

---

## Related Raspberry Pi high-FPS work

### GScrop / IMX296

This is the primary reference for this repository.

It established that the Global Shutter Camera can be driven far beyond the normal full-frame rate using sensor cropping while remaining on the libcamera stack.

### Pi 5 real-time 500+ fps processing

Nick Reyntjens' `500_fps_raspberry_pi_global_shutter_cam` project builds on GScrop and demonstrates a YUV420 pipeline with lightweight processing and encoding around the 500 fps range. It also documents an important practical issue: YUV plane stride/padding must be handled correctly.

### Python / Picamera2 reproduction

A 2025 GScrop Python conversion configures the crop using `media-ctl`, then uses Picamera2 with `raw=None`. This supports the idea that Python can be a clean control layer, although the high-throughput data path should still be native code.

### IMX219 / Camera Module v2

High-frame-rate experiments are not unique to IMX296.

The modern libcamera stack has been demonstrated around 207 fps with Camera Module v2 at 640x480 in specific configurations.

Earlier `raspiraw` work went much further. The deprecated Raspberry Pi `raspiraw` repository documents high-speed raw capture modes, including hundreds of FPS and experiments around 1000 fps with reduced/stretched IMX219 readout.

That work is historically important because it shows the same basic principle: reduce sensor readout and bypass expensive processing. However, `raspiraw` belongs to the legacy camera era, is deprecated, and is not the architecture this project should revive.

### IMX708 / Camera Module 3

Forum experiments have also pushed IMX708 above its standard advertised video rates by changing driver sensor modes. Examples include approximately 196 fps at 1536x512 and experimental higher-rate modes with more invasive driver modification.

This reinforces the general sensor-timing principle, but those modes require sensor-driver changes and are outside the initial scope of this IMX296 project.

---

## Initial project scope

### In scope

- Raspberry Pi Global Shutter Camera / IMX296;
- clean sensor-crop discovery and configuration;
- libcamera C++ capture;
- high-rate request/buffer handling;
- timestamp and frame-skip validation;
- raw Bayer and/or YUV420 capture;
- RAM ring buffering;
- pluggable processing stages;
- storage throughput measurement;
- optional Python bindings.

### Out of scope for the first milestone

- kernel patches for unrelated sensors;
- reviving the legacy camera stack;
- `raspiraw` compatibility;
- arbitrary IMX296 register hacking;
- assuming 1,000 fps is possible on IMX296;
- synchronous debayer/encoding inside the capture callback;
- Movidius/OpenVINO integration.

Those can be evaluated only after the base capture path is repeatable.

---

## Recommended implementation order

1. Add a diagnostic tool that enumerates media devices, entities, pads and the IMX296 subdevice.
2. Reproduce GScrop using `media-ctl` plus a minimal native capture/validation program.
3. Replace `media-ctl` with direct media-controller/V4L2 subdevice ioctls.
4. Reproduce 128x96 @ 536 fps and validate frame timestamps.
5. Reproduce 1456x96 @ 536 fps.
6. Add a bounded RAM ring buffer.
7. Benchmark direct SSD/NVMe writing.
8. Add YUV420/ISP output and measure stride/padding.
9. Add lightweight processing plugins.
10. Add optional Python bindings after the C++ capture path is stable.

The key project rule should be: **every optimization is measured by achieved sensor-frame timestamps and dropped frames, not just requested FPS or encoder output FPS.**

---

## References

Primary sources:

- Hermann Stamm-Wilbrandt, high-framerate Raspberry Pi Global Shutter Camera examples:  
  https://stamm-wilbrandt.de/GS/
- Raspberry Pi forum, "high framerate libcamera video capturing":  
  https://forums.raspberrypi.com/viewtopic.php?t=345883
- Hermann Stamm-Wilbrandt, GScrop:  
  https://gist.github.com/Hermann-SW/e6049fe1a24fc2b5a53c654e0e9f6b9c
- Raspberry Pi forum, IMX296 minimum-crop discussion:  
  https://forums.raspberrypi.com/viewtopic.php?t=359313
- Raspberry Pi, Global Shutter Camera product information:  
  https://www.raspberrypi.com/products/raspberry-pi-global-shutter-camera/
- libcamera camera sensor model:  
  https://github.com/raspberrypi/libcamera/blob/main/Documentation/camera-sensor-model.rst

Related implementations and historical work:

- Nick Reyntjens, 500+ fps Pi 5 processing experiment:  
  https://github.com/nickreyntjens/500_fps_raspberry_pi_global_shutter_cam
- paprikodlak, Python conversion of GScrop / Picamera2 example:  
  https://gist.github.com/paprikodlak/0f55438ec000157a43e1d3612a00b919
- Raspberry Pi raspiraw (deprecated, historical reference):  
  https://github.com/raspberrypi/raspiraw
- Raspberry Pi forum, Camera Module v2 around 206/207 fps:  
  https://forums.raspberrypi.com/viewtopic.php?t=346359
- Raspberry Pi Picamera2:  
  https://github.com/raspberrypi/picamera2

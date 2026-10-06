# Raw visualization and export concept

## Purpose

The core capture library must remain timing-clean and raw-first, but the project also needs a deterministic way to verify that captured data represents a real, correctly oriented image and to turn high-speed captures into viewable still images and slow-motion video.

This functionality should be implemented as a **separate conversion/export layer**, not inside the libcamera request-completion path.

The default workflow is:

```text
camera
  |
  v
hscam capture core
  |
  v
self-describing capture bundle
  |
  +--> image export (PNG / DNG)
  |
  +--> video export (RGB/YUV -> encoder)
  |
  +--> analysis tools
```

The converter can run after capture, on another machine if desired. This ensures that debayering or encoding performance cannot affect the measured capture rate.

---

# 1. Design goals

The first visualization/export implementation should:

- reproduce the captured pixels deterministically;
- support all Bayer orders exposed by Raspberry Pi/libcamera;
- support monochrome sensors;
- support common unpacked and CSI-2 packed raw bit depths encountered on Raspberry Pi cameras;
- correctly honor per-plane stride and frame size;
- preserve capture metadata;
- create a viewable still from any stored frame;
- create a slow-motion video from a high-FPS sequence;
- optionally export standards-based DNG for inspection in external raw tools;
- never be required for high-FPS capture itself;
- make all image-processing steps explicit and reproducible.

It is a verification tool first, not a photographic-quality ISP.

---

# 2. Why raw bytes alone are insufficient

A file containing Bayer bytes is not self-describing.

Correct reconstruction can depend on:

- sensor model;
- pixel format;
- Bayer order;
- bit depth;
- packed versus unpacked representation;
- width and height;
- stride;
- plane length;
- crop rectangle;
- black level;
- white level;
- analogue gain;
- exposure;
- colour gains;
- colour correction matrix;
- orientation;
- frame timestamp.

The capture path must therefore store raw payload together with the exact negotiated configuration and relevant per-frame metadata.

This also prevents a common failure mode where a file appears corrupt simply because the viewer assumed tightly packed rows while the real libcamera buffer had padding.

---

# 3. Capture bundle

Introduce a project capture artifact, tentatively called an **HSCAP bundle**.

For the first implementation it should be a directory rather than a custom binary container. A directory is easier to inspect, recover after interruption, and process with ordinary tools.

Example:

```text
capture.hscap/
    manifest.json
    frames.bin
    frames.idx
    metadata.jsonl
```

Optional generated material:

```text
capture.hscap/
    preview/
        frame-000000.png
        preview.mp4
```

Generated preview files are derived artifacts and are not part of the canonical capture.

## 3.1 manifest.json

Contains capture-wide immutable information:

```json
{
  "schema_version": 1,
  "camera": {
    "id": "...",
    "model": "imx296"
  },
  "sensor": {
    "mode_id": "...",
    "crop": [0, 496, 1456, 96]
  },
  "stream": {
    "pixel_format": "SBGGR10_CSI2P",
    "width": 1456,
    "height": 96,
    "planes": [
      {
        "stride": 1824,
        "length": 175104
      }
    ]
  },
  "colour": {
    "black_levels": null,
    "white_level": null,
    "colour_gains": null,
    "colour_correction_matrix": null
  }
}
```

Values above are illustrative.

The manifest records what was **negotiated**, not merely what was requested.

## 3.2 frames.bin

Contains frame payload bytes exactly as delivered by the selected stored stream.

No repacking or debayering is performed while capturing.

Every frame occupies a range identified by `frames.idx`.

## 3.3 frames.idx

A compact fixed-size binary index is preferable to scanning the entire payload.

Each record contains at least:

```text
frame_number
file_offset
payload_length
metadata_record_number
```

A simple versioned binary struct is sufficient initially.

Do not serialize native C++ structs directly; define byte order and field sizes explicitly.

## 3.4 metadata.jsonl

One JSON object per frame, containing human-readable metadata such as:

```json
{
  "frame": 1234,
  "sequence": 1234,
  "sensor_timestamp_ns": 2303456789000,
  "exposure_us": 100,
  "analogue_gain": 1.0,
  "frame_duration_us": 1866
}
```

JSONL is intentionally convenient for inspection and offline analysis.

If metadata volume later becomes significant, a more compact representation can be added without changing the raw payload format.

---

# 4. Writer behavior

The capture bundle writer is outside the critical request-completion callback.

Conceptually:

```text
libcamera completion
      |
      v
FrameLease
      |
      +--> optional payload writer thread
      |
      v
frames.bin + index + metadata
```

If the writer cannot keep up, that must be reflected in capture statistics.

For qualification, storage-backed capture and discard-payload timing tests are separate measurements.

The capture library should not pretend that disk throughput is part of sensor capability.

---

# 5. Export tool

Add a separate executable:

```text
hscam-export
```

Initial commands:

```bash
# Inspect capture metadata
hscam-export inspect capture.hscap

# Convert one raw frame to PNG
hscam-export image capture.hscap \
  --frame 1000 \
  --output frame.png

# Export a raw frame as DNG
hscam-export dng capture.hscap \
  --frame 1000 \
  --output frame.dng

# Convert the entire sequence to slow-motion video
hscam-export video capture.hscap \
  --output preview.mp4 \
  --playback-fps 30

# Export every Nth frame for quick visual inspection
hscam-export images capture.hscap \
  --every 100 \
  --output-dir preview/
```

A high-speed capture should default to **playback** at a normal viewing rate rather than attempting to preserve the original 500+ fps as the encoded video frame rate.

For example, a 536 fps capture exported at 30 fps plays approximately 17.9 times slower than real time.

The original sensor timestamps remain in the HSCAP bundle and should be summarized in the video metadata/report where practical.

---

# 6. Conversion pipeline

The first converter should implement a clear, deterministic pipeline.

For Bayer sensors:

```text
stored raw plane
    |
    v
remove row padding / respect stride
    |
    v
unpack raw samples
    |
    v
uint16 Bayer image
    |
    v
black-level subtraction
    |
    v
normalize against white level
    |
    v
demosaic
    |
    v
white balance
    |
    v
colour matrix
    |
    v
tone/gamma mapping
    |
    v
RGB8/RGB16
    |
    +--> PNG
    +--> video encoder
```

For monochrome sensors:

```text
raw
  -> unpack
  -> black/white normalization
  -> grayscale
  -> PNG/video
```

Each stage must be independently testable.

---

# 7. Raw unpacking

Raw unpacking is a project requirement because Raspberry Pi camera streams may use CSI-2 packed formats.

The first implementation should support at least:

- 8-bit Bayer;
- 10-bit unpacked;
- 10-bit CSI-2 packed;
- 12-bit unpacked;
- 12-bit CSI-2 packed;
- 16-bit Bayer where exposed;
- monochrome equivalents where required.

Do not infer packing only from bit depth.

The negotiated libcamera pixel format is authoritative.

Internally normalize raw input to:

```cpp
struct BayerImage16 {
    Size size;
    BayerOrder order;
    unsigned sourceBitDepth;
    std::vector<std::uint16_t> pixels;
};
```

This intermediate is for offline conversion, so the memory expansion is acceptable.

Raspberry Pi's current `rpicam-apps` DNG writer is a useful reference: it explicitly identifies packed Bayer formats and unpacks 10- and 12-bit data before writing DNG.

---

# 8. Bayer order

Support all four conventional patterns:

```text
RGGB
GRBG
GBRG
BGGR
```

The order comes from the negotiated pixel format.

Cropping can affect apparent Bayer phase if the sensor/driver does not compensate for an odd crop origin. Therefore the converter must use the **actual negotiated pixel format after crop/configuration**, not assume the sensor's full-array Bayer order.

The qualification dataset should include a visual/color sanity frame capable of detecting a phase error.

---

# 9. Demosaic

The first implementation should contain one small deterministic demosaicer.

Use **bilinear demosaic** initially.

Reasons:

- easy to verify;
- no external computer-vision dependency;
- fast enough for offline preview generation;
- deterministic across platforms;
- adequate to prove that Bayer order, stride, crop, and raw unpacking are correct.

This is not intended to match Raspberry Pi ISP image quality.

Define an interface so a better implementation can be added later:

```cpp
class Demosaicer {
public:
    virtual RgbImage16 process(const BayerImage16 &) = 0;
};
```

Do not build a plugin framework for v0.1; one concrete bilinear implementation behind an internal function/interface is enough.

---

# 10. Black level, white level, and normalization

The converter should distinguish:

- metadata supplied by libcamera;
- values known from camera/static configuration;
- fallback values inferred only for preview purposes.

Never silently treat an inferred value as measured metadata.

Suggested processing:

```text
linear = clamp(raw - black, 0, white - black)
normalized = linear / (white - black)
```

If reliable black/white metadata is unavailable, a `--preview-auto-level` mode may estimate display levels from percentiles.

That mode must be labelled as a visualization transform and must never alter the canonical raw capture.

---

# 11. White balance and colour

There should be two explicit preview modes.

## 11.1 Neutral verification

```text
--render neutral
```

Purpose:

- prove geometry;
- detect corrupt rows;
- detect Bayer phase;
- inspect motion;
- avoid pretending to reproduce the Raspberry Pi ISP.

Use simple normalization plus demosaic, with minimal colour transformation.

## 11.2 Colour preview

```text
--render colour
```

Where metadata is available:

1. apply black-level subtraction;
2. apply captured colour gains;
3. apply a colour correction matrix;
4. convert to display RGB;
5. apply a documented gamma/tone curve.

The result is still a project preview, not a bit-identical reproduction of Raspberry Pi's ISP.

That distinction should appear in generated reports.

---

# 12. Image output

First required image outputs:

## PNG

PNG is the normal visual-verification format.

Support:

- 8-bit RGB;
- optionally 16-bit RGB/gray later.

PNG output can be implemented with a small dedicated dependency such as libpng or another well-maintained encoder that is isolated to the export target.

It must not become a dependency of the capture core.

## DNG

DNG is valuable because it preserves Bayer data in a standard raw-image container that can be opened by tools such as RawTherapee or dcraw-compatible software.

Raspberry Pi's own `rpicam-still --raw` uses DNG for this reason and includes capture metadata such as exposure, gain, white balance information, and colour matrix.

DNG support can be the second exporter after PNG.

The project should study the BSD-licensed `rpicam-apps/image/dng.cpp` implementation rather than inventing sensor-specific DNG behavior blindly.

---

# 13. Video output

The raw-to-video path should be:

```text
HSCAP raw sequence
      |
      v
raw unpack + demosaic
      |
      v
RGB/YUV frames
      |
      v
video encoder
      |
      v
MP4/MKV
```

The encoder is part of the export program, never the high-FPS capture core.

## 13.1 First implementation

Prefer linking FFmpeg libraries in the export target:

- libavcodec;
- libavformat;
- libswscale where appropriate.

This avoids shell-command quoting/lifetime issues and gives the exporter full control over timestamps.

These remain optional build dependencies:

```text
libhscam core      -> libcamera + Linux APIs only
hscam-export       -> libhscam + image/video dependencies
```

If FFmpeg development libraries are unavailable, the first exporter may support image sequences only.

## 13.2 Video codec

For ordinary visual verification, H.264/H.265 is sufficient.

For diagnostic preservation of rendered frames, optionally support a lossless codec/container later.

The raw HSCAP data remains the source of truth, so the preview video does not need to be archival-quality.

---

# 14. Timestamps and playback

Do not encode a 536-fps capture as if normal video playback at 536 fps were the only representation.

The exporter has two timing modes:

## Real time

```text
--timing sensor
```

Frame presentation timestamps are derived from captured sensor timestamps.

This preserves real event timing, but a normal display may not visibly present every frame.

## Slow motion

```text
--playback-fps 30
```

Every captured frame becomes one 30-fps video frame.

This is the preferred visual verification mode for high-speed captures.

The report should state:

```text
capture FPS: 535.98
playback FPS: 30
slowdown: 17.87x
```

If input timestamps show a missing frame, the exporter must not silently invent one. The visual report should retain a dropped-frame marker/count.

---

# 15. Live preview

Live preview is useful during development, but it should not be part of the first high-FPS correctness path.

There are two possible live-preview architectures:

## Processed libcamera stream

Request a processed stream from libcamera/ISP alongside or instead of the raw stream.

Advantages:

- Raspberry Pi ISP handles demosaic/colour;
- minimal custom image-processing code.

Disadvantages:

- an additional stream can alter sensor/pipeline configuration;
- ISP work and memory bandwidth may influence the high-FPS experiment;
- some experimental crop setups may not coexist with the extra stream.

Therefore any capture using an ISP preview stream must be benchmarked separately from raw-only qualification.

## Sampled software preview

Copy, for example, every 50th or 100th raw frame to a low-priority preview worker.

Advantages:

- does not require another libcamera stream;
- uses the same raw frame being qualified.

Disadvantages:

- additional memory copy/CPU load;
- custom demosaic path.

This can be added later.

For v0.1, **offline preview is sufficient**.

---

# 16. Verification artifacts for the official-camera campaign

The one-time official Raspberry Pi camera qualification should preserve enough imagery for human review without committing enormous raw datasets.

For every camera, preserve at least:

- one baseline full/advertised-mode DNG or raw sample;
- one PNG rendered from that baseline;
- representative PNGs from important high-FPS modes/crops;
- a short slow-motion video from the fastest validated mode;
- the exact HSCAP manifest and per-frame timing data used to generate the preview.

Large full raw sequences do not necessarily need to live in Git if the timing dataset and representative raw frames are sufficient.

The published result page should make clear that PNG/video files are derived verification artifacts and the JSON/timing/raw metadata are authoritative for performance claims.

---

# 17. Visual test scene

To make human validation rigorous, use a repeatable scene rather than arbitrary footage.

A useful target should contain:

- high-contrast vertical lines;
- high-contrast horizontal lines;
- diagonal edges;
- fine text;
- red, green, blue, white, gray, and black areas;
- a frame counter or moving element for temporal verification.

This makes it easy to spot:

- incorrect stride;
- truncated rows;
- Bayer order errors;
- odd/even crop phase errors;
- incorrect orientation;
- black-level problems;
- gross colour errors;
- repeated/missing frames.

For high-speed temporal verification, a hardware-timed LED pattern or rotating/moving target can be used in addition to the static chart.

---

# 18. Library boundaries

The project should be split roughly as:

```text
libhscam
  camera discovery/configuration/capture only

libhscam_raw
  raw format descriptions
  unpacking
  Bayer order
  basic demosaic
  preview colour conversion

hscam-capture
  writes HSCAP bundles

hscam-export
  reads HSCAP
  PNG/DNG/video export
```

`libhscam_raw` is deliberately separate from `libhscam`.

An application that only needs maximum-performance raw capture should not link image codecs or FFmpeg.

The raw library may later become header/internal-only if a separate public API proves unnecessary.

---

# 19. Proposed source tree additions

```text
include/hscam/
    ...

include/hscam/raw/
    pixel_layout.hpp
    bayer.hpp
    unpack.hpp
    render.hpp

src/raw/
    unpack_8.cpp
    unpack_10.cpp
    unpack_12.cpp
    unpack_16.cpp
    demosaic_bilinear.cpp
    render.cpp

src/capture_format/
    manifest.cpp
    frame_index.cpp
    bundle_writer.cpp
    bundle_reader.cpp

tools/export/
    main.cpp
    image_export.cpp
    dng_export.cpp
    video_export.cpp

tests/unit/raw/
    unpack_10_test.cpp
    unpack_12_test.cpp
    bayer_phase_test.cpp
    stride_test.cpp
    demosaic_test.cpp
```

---

# 20. Required raw-conversion tests

The conversion path needs strong synthetic tests because image corruption can look superficially plausible.

At minimum test:

- known packed 10-bit byte pattern -> expected 16-bit samples;
- known packed 12-bit byte pattern -> expected samples;
- every Bayer order;
- odd/even crop-origin Bayer phase handling;
- padded input stride;
- width not aligned to raw packing group;
- black-level subtraction;
- clipping at white level;
- deterministic bilinear demosaic;
- monochrome path;
- truncated frame rejected;
- manifest format/payload mismatch rejected.

Add small synthetic Bayer fixtures whose expected RGB output can be calculated exactly.

Do not rely only on photographs for correctness tests.

---

# 21. Revised first implementation milestones

The capture-core milestones remain unchanged through high-FPS raw capture.

Then add:

## Milestone E — self-describing capture

- HSCAP manifest;
- frame payload file;
- frame index;
- per-frame metadata;
- interrupted-capture recovery/finalization.

Success:

a captured sequence can be reopened without supplying width, height, pixel format, stride, or timestamps manually.

## Milestone F — still verification

- raw unpacking;
- Bayer-order support;
- bilinear demosaic;
- neutral render;
- PNG export.

Success:

```bash
hscam-export image capture.hscap --frame 100 --output frame.png
```

produces a geometrically correct image from each official camera family that outputs Bayer/mono raw.

## Milestone G — DNG

- metadata mapping;
- standards-based raw still export.

Success:

representative DNG files open correctly in at least two independent raw-image applications.

## Milestone H — slow-motion video

- frame conversion;
- timestamp handling;
- optional FFmpeg library integration;
- configurable playback FPS.

Success:

a high-FPS HSCAP sequence can be exported to a normal 30/60-fps slow-motion video while preserving every stored frame in order.

Only after these milestones should the official-camera qualification dataset be considered visually auditable.

---

# 22. Core principle

The project should maintain this separation:

```text
capture correctness != render quality
```

The raw payload and timestamps prove what the camera delivered.

The PNG/DNG/video pipeline exists so a human can verify that those bytes reconstruct into sensible images and motion.

A visually attractive preview must never be used as evidence that the high-FPS capture itself was lossless or correctly timed.

---

# References

Raspberry Pi raw capture documentation:

- https://www.raspberrypi.com/documentation/computers/camera_software.html
- https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/camera/rpicam_raw.adoc
- https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/camera/rpicam_still.adoc
- https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/camera/rpicam_options_still.adoc

Raspberry Pi DNG implementation:

- https://github.com/raspberrypi/rpicam-apps/blob/main/image/dng.cpp

FFmpeg pixel formats:

- https://ffmpeg.org/doxygen/trunk/pixfmt_8h.html

Project design documents:

- [C++ library: first implementation concept](cpp-library-first-implementation.md)
- [High-FPS capture method](high-fps-capture-method.md)
- [Camera support and automated crop testing](camera-support-and-crop-testing.md)

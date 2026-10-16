# RAW representation in camera qualification

> This records the functioning OV5647 **baseline RAW path**. Experimental
> reduced-height readout is the new priority and needs a modified kernel
> driver for sensors whose stock driver lacks mutable ROI.
> See [roadmap](experimental-high-fps-roadmap.md).
> The same RAW policy can test true experimental modes once exposed.

The qualification runner supports a campaign-wide RAW capture policy. The same
policy is frozen into each advertised-mode case, passed to isolated FPS workers,
and reused when recording HSCAP/PNG/video evidence. Processed sensor-crop
cases are **not** converted to RAW; this is essential for testing mutable
sensor crops without unintentionally adding another stream.

## Why this matters on Raspberry Pi 5

The PiSP pipeline can negotiate `GBRG_PISP_COMP1` by default for the OV5647.
That format can be captured and timed, but `hscam-export` cannot currently
decode it. Therefore a campaign using the default RAW representation may have
valid timing probes yet fail its required visual-evidence stage.

The OV5647 was independently observed to support this *unpacked* Pi 5 path:

```text
sensor: 640x480-SGBRG10_1X10 (10 sensor bits)
CFE:    640x480-GB16
RAW:    640x480-SGBRG16, stride 1280 (16 storage bits)
```

The tested `rpicam-raw --mode 640:480:10:U` configuration demonstrates
hardware support for unpacked Bayer. A separate `hscam-capture` experiment
with an explicit `SBGGR10` request yielded a visually correct PNG.

## Manifest syntax

Add an optional top-level `raw_capture` object:

```json
{
  "raw_capture": {
    "representation": "uncompressed",
    "format": "SBGGR10",
    "sample_alignment": "left"
  }
}
```

The fields are:

- `representation`: `auto` (default), `uncompressed` or `compressed`.
  The requested class must match the negotiated RAW format. In this API
  `compressed` covers both CSI-2-packed Bayer and PiSP COMP1, although these
  are different wire/storage encodings; the negotiated pixel-format string
  disambiguates them.
- `format`: optional explicit Bayer pixel-format string. Specify the
  sensor's native depth (for example `SBGGR10`) when the pipeline advertises
  only a COMP1 mode name that omits that information. The request may negotiate
  to a different Bayer order/container (for example, `SGBRG16`).
- `sample_alignment`: optional `left`, `right` or `native`. Omit it
  unless the platform's container layout is known. On this Pi 5, the 10-bit
  OV5647 samples are left-aligned within the 16-bit CFE output.

There is no need to specify `sample_alignment` for the known Pi 5 environment
unless verifying an explicit override. The backend attempts the platform
default.

**Important:** `raw_capture.format` applies to *every RAW advertised-mode
case* in the campaign. It is appropriate for the OV5647's all-10-bit tested
modes, but is not a generic choice for cameras with mixed native bit depths.
Such cameras need a sensor-appropriate policy and may require per-mode format
selection in a future version.

If the policy is omitted, previous behavior is retained (libcamera's default
RAW role). An undecodable negotiated COMP1 stream is deliberately rejected
by the visual-evidence stage rather than silently counted as a passing PNG.

## Reproducible OV5647 pilot

A bounded experimental manifest is provided:

`qualification/manifests/ov5647-pi5-pilot-v1.json`

It requests `SBGGR10` unpacked RAW, exercises all advertised modes, attempts
sensor-crop TRY discovery, and performs a shorter FPS search. Crop
**performance** sweeps are disabled to limit risk and runtime. The pilot still
requires `ffmpeg` for slow-motion video and checks the board's throttling
state.

Build, test, and run on the actual Raspberry Pi:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHSCAM_REQUIRE_LIBCAMERA=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

build/hscam-qualify run \
  qualification/manifests/ov5647-pi5-pilot-v1.json \
  --output qualification/work/ov5647-pi5-pilot
```

Check `camera.json`, `mode_sensor_crops.json`, `crop_geometry.json`,
`results.json`, `results.csv`, `report.md`, `samples.json` and
`campaign_status.json`.

The frozen `plan.json` and case JSON now include the RAW policy (only when
non-default) so evidence captures use the same RAW request as timing runs.
Each case result exposes the actual negotiated format, requested format,
RAW encoding class, data bits, container bits, alignment and stride.
`samples.json` also identifies the actual sample-stream representation.

Visual sampling warms up automatic exposure before writing frame zero and
rejects obviously black or near-constant previews. This is a **sanity check**,
not a substitute for a human visually inspecting PNG colour order and image
integrity. It is possible to pass the heuristic with a still-invalid image.

## Publication and remaining work

The pilot uses a **different campaign definition** from
`official-rpi-cameras-v1.json`; it is not eligible to masquerade as a
promoted official result. The canonical official manifest was intentionally
left unchanged. The official six-camera campaign still needs real hardware
qualification, camera-specific format policy decisions, and reviewed visual
evidence.

Known limitations:

1. COMP1 captures remain valid for timing but cannot yet be reconstructed
   offline as images.
2. The Pi 5 alignment heuristic uses board identification; a full
   camera/pipeline-specific capability model would be more robust.
3. When libcamera's advertised format omits bit depth, the backend can
   use an explicit `SBGGR10` request as a bit-depth hint. This remains
   a **request-derived inference**, not independent sensor metadata.
4. Uncompressed RAW consumes more capture/storage bandwidth than COMP1.
   FPS measurements for different representations should be reported
   separately rather than pooled.

Do not claim a successful hardware qualification from unit-test results,
source inspection or the earlier independent `hscam-capture` test. The
`hscam-qualify` pilot itself must run and produce valid artifacts on the Pi.

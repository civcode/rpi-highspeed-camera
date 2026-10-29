# rpi-highspeed-camera

**Experimental high-FPS sensor readout for Raspberry Pi cameras.**

The goal is to reproduce—and systematically extend—the sensor-register
and reduced-height readout techniques demonstrated by historical `raspiraw`
scripts, using a clean **C++20 capture/qualification library** and optional
**Linux V4L2 sensor-driver extensions**. It targets all six official Raspberry
Pi camera families with **sensor-specific strategies**, not a claim that
every sensor supports the same ROI or frame rate.

Ordinary documented/advertised modes are baseline regression tests only.
The primary goal is **real sensor readout hacks**: smaller sensor windows,
row skipping/binning, clock/blanking changes and measured maximum stable FPS.
Image resizing or libcamera `ScalerCrop` must never be reported as proof.

The running Linux sensor driver owns I²C control. The C++ library uses
libcamera and V4L2, while experimental kernel drivers (written in C)
can expose new, coherent sensor modes. Only one driver binds a sensor at
a time; changes must be opt-in with a stock-kernel rollback.

**Design:** [Project goals](docs/project-goals.md) |
[Active development roadmap](docs/experimental-high-fps-roadmap.md) |
[Kernel-driver integration](docs/kernel-driver-integration.md).

## Experimental discovery and safe campaign

```bash
# Reports strategy, reference readouts and unmet kernel requirements.
# No camera registers are modified by this command.
build/hscam-highspeed --json

# Run ONLY on a Pi whose modified OV5647 driver advertises one or more
# genuine reduced-height modes; stock drivers fail closed with a clear error.
build/hscam-qualify run \
  qualification/manifests/ov5647-experimental-readout-v1.json \
  --output qualification/work/ov5647-experimental
```

The experimental manifest requests exact 640×240/128/64/32 readout
dimensions; it will **not** silently switch to normal 640×480 capture.
The opt-in [OV5647 multi-mode POC](kernel/ov5647/poc/README.md) now exposes
all four readout heights on Pi 5. Recorded steady-state rates reached
**298.62 FPS at 640×128, 502.14 FPS at 640×64, and 655.95 FPS at 640×32**
in approximately ten-second saved-RAW tests with no reported gaps/overruns.
These are tested timing points, not absolute maxima or verified physical ROI
geometry. The standalone POC runner separates startup from steady timing;
the generic qualification runner still has startup/jitter and visual-evidence
limitations. See the POC notes for exact commands, evidence and rollback.

## First experimental OV5647 mode on Pi 5

A compiled experimental OV5647 module successfully loaded on the Pi 5
running `6.18.50+rpt-rpi-2712` and exposed **640×240** as a fifth
libcamera sensor mode. **The first 32 RAW frames were captured at 29.8551
FPS with zero reported sequence gaps or overruns**, and a PNG was exported
and viewed by the experiment owner. This validates the initial RAW
pipeline, **not** the theoretical ~179 FPS ceiling or sensor-window
image fidelity. See the
[first-boot evidence and stepwise FPS validation checklist](kernel/ov5647/first-boot-validation.md).

If `build/hscam-inspect` exists but `build/hscam-highspeed` is missing,
reconfigure and build the new userspace target:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHSCAM_REQUIRE_LIBCAMERA=ON
cmake --build build --target hscam-highspeed hscam-capture hscam-export -j2
build/hscam-highspeed --json
```

## Existing capture and baseline tooling (v0.1 lineage)


The repository currently contains:

- `libhscam`: camera discovery, configuration, capture, timestamps and sensor-crop control;
- `hscam-inspect`: inspect cameras, modes and crop capabilities;
- `hscam-capture`: measured capture to discard or a self-describing HSCAP bundle;
- `hscam-export`: HSCAP inspection, PNG preview, optional DNG, and slow-motion video export;
- `hscam-qualify`: manifest-driven, resumable hardware qualification with crop discovery and maximum-stable-FPS search;
- the version-controlled official-camera qualification manifest.

The OV5647 baseline RAW→HSCAP→PNG path has been exercised on Pi 5,
and preliminary fixed-mode FPS pilots were run. These are **not**
evidence of the new experimental reduced-height driver modes.
The official six-camera experimental dataset is still pending.

## Build on Raspberry Pi OS

Install a C++20 compiler, CMake, pkg-config and the libcamera development package. DNG export additionally uses libtiff. Slow-motion video export invokes a local `ffmpeg` executable.

A typical build is:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DHSCAM_REQUIRE_LIBCAMERA=ON

cmake --build build -j
ctest --test-dir build --output-on-failure
```

The core library does not depend on OpenCV, FFmpeg, OpenVINO, or an image-processing framework.

For a library-only build:

```bash
cmake -S . -B build \
  -DHSCAM_BUILD_TOOLS=OFF

cmake --build build -j
cmake --install build --prefix /desired/prefix
```

The installed CMake package exports:

- `hscam::hscam` — capture/discovery library;
- `hscam::raw` — HSCAP/raw reconstruction and preview library.

A downstream CMake project can use:

```cmake
find_package(hscam 0.1 REQUIRED CONFIG)
target_link_libraries(my_app PRIVATE hscam::hscam hscam::raw)
```

## Inspect a camera

```bash
build/hscam-inspect
```

Machine-readable inventory:

```bash
build/hscam-inspect --json
```

The inventory reports libcamera-advertised raw modes and the sensor crop capabilities discovered from the media/V4L2 graph.

## Capture

Measure capture without storage as a confounding variable:

```bash
build/hscam-capture \
  --camera CAMERA_ID \
  --mode MODE_ID \
  --frames 10000 \
  --discard-payload
```

Output HSCAP directories must not already exist; there is no `--overwrite`
option. Use a fresh name per run (including when reusing a shell variable):

```bash
OUT="qualification/work/capture-$(date +%Y%m%d-%H%M%S-%N)"
```

For the new fixed-mode IDs, exposure/gain examples and measured timing limits,
see [the POC capture guide](kernel/ov5647/poc/README.md#capture-and-lighting-insights).

Store a self-describing raw capture:

```bash
build/hscam-capture \
  --camera CAMERA_ID \
  --mode MODE_ID \
  --frames 1000 \
  --output example.hscap
```

Request **uncompressed** Bayer explicitly. On the Raspberry Pi 5 the default raw role negotiates to the PiSP compressed `GBRG_PISP_COMP1`; passing `--representation uncompressed` (or an explicit unpacked format such as `--format SBGGR10`) selects the sensor's unpacked stream instead, which negotiates to a 16-bit Bayer container (`SGBRG16`). The bundle records what the pipeline actually negotiated - container format, requested format, representation class, 10-bit/16-bit bit widths, and sample alignment - so `hscam-export` can decode the native sample width exactly. On Raspberry Pi hardware the native sample is left-aligned in the wider container; other platforms use the standard right-aligned convention unless `--alignment left|right|native` overrides it:

```bash
build/hscam-capture \
  --camera CAMERA_ID \
  --mode MODE_ID \
  --representation uncompressed \
  --frames 64 \
  --output uncompressed.hscap
```

For a sensor that exposes mutable cropping, an experimental high-FPS request can use the processed/no-secondary-raw path used by the known GScrop technique:

```bash
build/hscam-capture \
  --camera CAMERA_ID \
  --processed \
  --crop X,Y,WIDTH,HEIGHT \
  --frame-duration-us 1866 \
  --frames 5000 \
  --exact \
  --output cropped.hscap
```

The requested values are not silently treated as hardware state. The library records the negotiated stream and sensor crop and reports adjustments.

## Visual verification

Inspect a capture:

```bash
build/hscam-export inspect cropped.hscap
```

If capture was interrupted, recovery can salvage fully indexed payload frames and explicitly mark metadata that did not survive:

```bash
build/hscam-export recover interrupted.hscap
```

A recovered interrupted bundle remains marked incomplete and must not be used as valid qualification timing data.

Render one frame to PNG:

```bash
build/hscam-export image cropped.hscap \
  --frame 100 \
  --output frame.png
```

Export a Bayer frame as DNG when libtiff support was available at build time:

```bash
build/hscam-export dng cropped.hscap \
  --frame 100 \
  --output frame.dng
```

Create slow-motion video using the local FFmpeg executable:

```bash
build/hscam-export video cropped.hscap \
  --output slow-motion.mp4 \
  --playback-fps 30
```

Or export at the average real-time rate derived from captured sensor timestamps:

```bash
build/hscam-export video cropped.hscap \
  --output realtime.mp4 \
  --timing sensor
```

`hscam-export inspect` also reports timestamp-derived FPS when the capture contains enough sensor timestamps. The sensor-timing mode preserves the measured average elapsed rate in a constant-frame-rate video; the HSCAP per-frame timestamps remain authoritative for jitter/drop analysis.

Rendering is deliberately offline by default. Image processing and encoding therefore cannot make a high-FPS timing test pass or fail.

## Legacy official-camera baseline qualification

The canonical campaign definition is:

```text
qualification/manifests/official-rpi-cameras-v1.json
```

Run it with exactly one camera attached, or specify a camera ID:

```bash
build/hscam-qualify run \
  qualification/manifests/official-rpi-cameras-v1.json \
  --output qualification/work/imx296
```

For an initial **Raspberry Pi 5 + OV5647** validation, use the smaller pilot
before the exhaustive official campaign. It requests `SBGGR10` unpacked
Bayer so PiSP's default COMP1 encoding does not prevent PNG/video evidence:

```bash
build/hscam-qualify run \
  qualification/manifests/ov5647-pi5-pilot-v1.json \
  --output qualification/work/ov5647-pi5-pilot
```

See [qualification RAW capture policy](docs/qualification-raw-policy.md) for
`raw_capture` manifest options, limitations and the distinction between a
pilot and an official result.

The campaign is intentionally much more rigorous than an interactive benchmark. It:

- fingerprints the board, OS, kernel, libcamera version and source revision;
- freezes a deterministic plan ID and refuses to resume into output from a
  different plan;
- rejects the official run if the Pi reports a non-clean throttling state;
- enumerates all advertised raw modes;
- discovers mutable sensor-crop geometry with V4L2 TRY negotiation and keeps
  every requested/negotiated probe as JSONL evidence;
- records the active sensor crop associated with every advertised raw mode;
- checks width/height combinations and origin alignment;
- measures crop performance across every accepted height and representative widths;
- samples corner/center crop positions;
- searches for the smallest stable frame duration rather than trusting requested FPS;
- rejects a boundary when a single sensor-timestamp interval exceeds the
  campaign jitter limit, even if average FPS looks acceptable;
- performs a longer final validation at the discovered boundary;
- runs risky captures in isolated worker processes with hard timeouts;
- performs a known-good recovery capture after failures;
- resumes completed cases after interruption;
- writes JSON, CSV and Markdown results plus per-frame timing JSONL for every
  completed capture probe;
- captures representative raw/PNG/DNG/video evidence only after timing tests.

This campaign can be long. It is intended to be run once per official camera/software environment to produce the public dataset, not as a routine application command.

The official v1 campaign requires a local `ffmpeg` executable because a
slow-motion video is part of the visual publication evidence. DNG generation
remains optional and depends on libtiff.

## Qualification output

A completed run contains approximately:

```text
qualification/work/<camera>/
    campaign.json
    plan.json
    environment.json
    environment_end.json
    camera.json
    mode_sensor_crops.json
    crop_geometry.json
    crop_geometry_probes.jsonl
    campaign_status.json
    results.json
    results.csv
    report.md
    cases/
        <case-id>.json
        <case-id>.timing.jsonl
    search/
        <case-id>/
            <probe>.json
            <probe>.timing.jsonl
    samples/
        baseline.hscap/
        baseline-frame-0.png
        fastest.hscap/
        fastest-frame-*.png
        fastest-slow-motion.mp4
```

Only valid, clean-revision runs are eligible for the checked-in results dataset. Promote them with:

```bash
build/hscam-qualify promote \
  qualification/work/imx296 \
  --results-root qualification/results
```

The promotion command recomputes plan provenance, verifies the aggregate
against the frozen case plan, requires per-frame timing evidence for passing
cases, verifies visual files, retains case/search/crop-probe audit data, and
omits only the large raw `.hscap` sample bundles. Full promotion rules live in
[qualification/results/README.md](qualification/results/README.md).

After promoting runs, check Phase 10 completeness with:

```bash
build/hscam-qualify audit-results \
  --results-root qualification/results
```

The audit requires each official v1 sensor family to be represented by a valid
promotion or an explicit reviewable `blocked.json` record.

## Design documents

- [v0.1 implementation roadmap](docs/v0.1-implementation-roadmap.md)
- [v0.1 release status](docs/v0.1-release-status.md)
- [HSCAP v1 format](docs/hscap-v1-format.md)
- [qualification results v1](docs/qualification-results-v1.md)
- [C++ library first implementation](docs/cpp-library-first-implementation.md)
- [raw visualization and export](docs/raw-visualization-and-export.md)
- [project goals and mission](docs/project-goals.md)
- [experimental high-FPS roadmap](docs/experimental-high-fps-roadmap.md)
- [kernel sensor-driver integration](docs/kernel-driver-integration.md)
- [camera support and crop testing](docs/camera-support-and-crop-testing.md)
- [high-FPS capture method](docs/high-fps-capture-method.md)

## Project rules

The implementation follows a few deliberate constraints:

- sensor crop and ISP/scaler crop are never conflated;
- requested FPS is never reported as achieved FPS;
- frame timing comes from captured metadata/timestamps;
- no hidden frame dropping or queue overwrite;
- the capture callback performs no debayering, encoding, CV, or disk I/O;
- raw storage preserves actual plane sizes/stride metadata;
- no `media-ctl`, `rpicam-vid`, or FFmpeg subprocess is used by the core capture library;
- qualification hardware execution is local and explicit, not GitHub Actions.

The repository is pre-1.0 and the public API/HSCAP schema may still change while hardware validation proceeds. The implementation remains `0.1.0-dev`; the physical six-camera campaign and a project-owner license decision are still required before the final v0.1 release. See [v0.1 release status](docs/v0.1-release-status.md).

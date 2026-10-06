# Changelog

This project has not published its first stable release yet.

## 0.1.0-dev — unreleased

### Capture core

- Added a C++20 `libhscam` API for camera discovery, configuration and
  capture.
- Added runtime libcamera camera/mode discovery.
- Added generic Linux V4L2 subdevice crop capability discovery and
  TRY/ACTIVE negotiation.
- Added exact-vs-adjusted configuration reporting.
- Added persistent request/buffer ownership with move-only `FrameLease`.
- Added sensor timestamps, sequence-gap and queue-overrun statistics.
- Kept high-cost processing out of the request-completion path.
- Added the processed/no-secondary-raw capture path needed to reproduce
  sensor-crop high-FPS experiments without relying on shell tools.

### Capture format and visual verification

- Added self-describing HSCAP v1 bundles.
- Added interrupted-capture recovery.
- Added exact per-frame timestamp/exposure/gain metadata reading.
- Added RAW8/10/12/16 unpacking and Bayer/mono/YUV preview reconstruction.
- Added dependency-free PNG preview output.
- Added optional DNG export through libtiff.
- Added slow-motion video export through a local FFmpeg executable.
- Added timestamp-derived real-time video-rate export.

### Qualification

- Added a version-controlled official-camera campaign manifest.
- Added resumable isolated test execution with hard timeouts.
- Added environment/source provenance and Raspberry Pi throttling checks.
- Added sensor-crop geometry discovery and alignment probing.
- Added width/height/position characterization.
- Added maximum-stable-FPS search with longer boundary validation.
- Added known-good camera recovery checks after failed experimental cases.
- Added JSON, CSV and Markdown reports.
- Added post-qualification visual sample capture.
- Added guarded promotion of valid clean-revision runs into
  `qualification/results/`.
- Added strict resume-plan identity so stale cases from another campaign or
  revision cannot be reused accidentally.
- Added mode-specific sensor-crop snapshots and full TRY-crop probe logs.
- Added per-frame timing JSONL evidence for qualification probes.
- Added a single-frame interval stability limit in addition to measured FPS
  and sequence-gap checks.
- Added deterministic plan provenance and publication-time evidence
  verification for case results, timing traces and visual artifacts.
- Reused the campaign's libcamera context for post-run visual sampling.

### Build and packaging

- Added host-safe unit tests.
- Added qualification promotion tests for provenance, visual evidence,
  recovery failure and pass-case timing evidence.
- Expanded raw reconstruction tests with padded stride, all Bayer orders,
  crop-phase handling, exact bilinear fixtures, monochrome and truncation
  checks.
- Added an installable CMake package exporting `hscam::hscam` and
  `hscam::raw`.
- Added optional command-line tool builds.

### Documentation

- Added high-FPS capture-method research.
- Added generic camera/crop qualification design.
- Added C++ core implementation design.
- Added raw visualization/export design.
- Added HSCAP v1 and qualification-results v1 format contracts.
- Added the v0.1 implementation roadmap and release-status document.

### Remaining before final 0.1.0

- Build and validate the Pi-specific backend on the canonical Raspberry Pi OS
  environment.
- Run and promote the official campaign for OV5647, IMX219, IMX708, IMX477,
  IMX296 and IMX500.
- Select and add the project software license.
- Review the resulting reference dataset and then deliberately change the
  development version/tag to final `0.1.0`.

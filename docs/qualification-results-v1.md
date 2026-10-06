# Qualification result format

The official-camera qualification runner writes a directory of versioned machine-readable artifacts plus human-readable summaries.

This document describes the **v1 qualification output contract** used by `hscam-qualify`.

The campaign is designed for the one-time official Raspberry Pi camera characterization effort. It is not a general benchmark service.

## Canonical run

A run is created with:

```bash
hscam-qualify run \
  qualification/manifests/official-rpi-cameras-v1.json \
  --output qualification/work/<camera>
```

The runner writes cases incrementally so an interrupted campaign can resume.

## Directory layout

A completed run contains approximately:

```text
<run>/
    campaign.json
    plan.json
    environment.json
    environment_end.json
    camera.json
    crop_geometry.json
    campaign_status.json
    results.json
    results.csv
    report.md
    samples.json

    cases/
        <case-id>.json
        ...

    search/
        <case-id>/
            <frame-duration>-<duration>.json
            ...

    work/
        ...

    samples/
        baseline.hscap/
        baseline-frame-0.png
        fastest.hscap/
        fastest-frame-*.png
        fastest-slow-motion.mp4
```

`recovery_failure.json` is written if a failed experimental case leaves the camera unable to complete the known-good recovery check.

## Versioning rule

Every published standalone JSON artifact written by the v1 runner carries:

```json
{
  "schema_version": 1
}
```

Case result files are embedded into the versioned `results.json` aggregate. Their shape is part of the v1 results contract even though the individual worker file is primarily an internal/resume artifact.

Future incompatible changes require a new schema version.

## campaign.json

This is an exact copy of the campaign manifest supplied to the runner.

For the official v1 campaign the canonical source is:

```text
qualification/manifests/official-rpi-cameras-v1.json
```

The committed manifest controls:

- advertised-mode tests;
- sensor-crop geometry discovery;
- crop width/height/position sampling;
- worker timeouts;
- drop requirements;
- FPS-search range and convergence;
- final validation duration;
- throttling requirements;
- visual evidence generation.

Published results should keep this file unchanged so the exact experimental policy is preserved beside the output.

## plan.json

`plan.json` freezes the generated test plan before execution.

Core fields:

```json
{
  "schema_version": 1,
  "plan_id": "...",
  "campaign": "official-rpi-cameras-v1",
  "camera_id": "...",
  "hscam_version": "0.1.0-dev",
  "source_revision": "...",
  "cases": []
}
```

The plan ID is deterministic for the combination of:

- manifest text;
- selected camera ID;
- hscam version;
- source revision.

A source revision ending in `-dirty` indicates the executable was built from a modified working tree and must not be promoted as an official reference result.

### Case shape

Each planned case records:

```json
{
  "case_id": "...",
  "camera_id": "...",
  "mode_id": null,
  "crop": [0, 496, 1456, 96],
  "frame_duration_us": null,
  "stream_kind": "processed",
  "duration_ms": 3000,
  "exact": true,
  "require_zero_drops": true
}
```

`stream_kind` is `raw` or `processed`.

The processed path is significant for experimental sensor crops: it permits qualification of the sensor-level crop without requiring a secondary raw stream that could force a different pipeline configuration.

## environment.json / environment_end.json

These capture the host state at campaign start and end.

v1 records at least:

- `schema_version`;
- hscam version;
- source revision;
- Raspberry Pi board model when available;
- OS release text;
- kernel version;
- machine architecture;
- thermal-zone temperature in millidegrees Celsius when available;
- libcamera version from pkg-config when available;
- `vcgencmd get_throttled` output when available.

The official campaign can require a clean Raspberry Pi throttling state.

A run that starts or ends in a disallowed throttling state is not valid reference data.

## camera.json

This is the inventory snapshot for the selected camera.

It contains:

- `schema_version`;
- camera ID and model;
- sensor model;
- pixel-array size;
- sensor subdevice;
- discovered capabilities;
- advertised raw sensor modes.

Capabilities distinguish raw/processed capture and sensor-crop query/TRY/ACTIVE support.

This file describes what the runtime stack exposed during the campaign; it should not be treated as a timeless specification of the sensor.

## crop_geometry.json

Written when crop discovery is enabled.

The geometry result records the sensor driver's observed TRY-selection behavior, including:

- crop bounds;
- accepted widths;
- accepted heights;
- accepted x origins for the minimum size;
- accepted y origins for the minimum size;
- derived x/y alignment steps;
- Cartesian width/height verification count;
- exceptions when width and height acceptance is not independent.

Geometry acceptance is not the same as stable high-FPS capture. Performance is measured in separate cases.

## cases/<case-id>.json

A direct capture case normally includes:

```json
{
  "case_id": "...",
  "configuration": {
    "mode_id": null,
    "crop": [0, 496, 1456, 96],
    "stream": {
      "width": 1456,
      "height": 96,
      "pixel_format": "YUV420",
      "frame_bytes": 209664,
      "buffer_count": 4
    },
    "adjustments": []
  },
  "stats": {
    "requests_completed": 2680,
    "requests_cancelled": 0,
    "sequence_gaps": 0,
    "consumer_starvations": 0,
    "queue_overruns": 0,
    "measured_fps": 535.9,
    "min_interval_ns": 1860000,
    "max_interval_ns": 1880000
  },
  "frames_consumed": 2680,
  "last_frame_duration_us": 1866,
  "first_sensor_timestamp_ns": 123,
  "last_sensor_timestamp_ns": 456,
  "status": "pass",
  "error": null
}
```

Values are illustrative.

### Status interpretation

The important status classes are:

- `pass`: the case satisfied the configured stability requirements;
- `unstable`: capture ran but failed the stability/drop/rate criteria;
- `error`: configuration or capture raised an error;
- `timeout`: the isolated worker exceeded its hard timeout;
- `worker_error`: the worker process failed without producing a normal result.

A non-pass experimental result triggers a known-good camera recovery capture. If that recovery fails, the campaign aborts and writes `recovery_failure.json`.

## FPS-search result

When FPS search is enabled, the final case result summarizes a set of isolated probe runs.

Important fields include:

- `best_frame_duration_us`;
- `best_measured_fps`;
- `final_result`;
- `probes`;
- `status`;
- `error`.

The search does not assume that a requested frame duration was achieved.

A probe is considered stable only when it satisfies the campaign's requirements, including delivered FPS relative to the requested frame duration and the zero-drop policy.

The discovered boundary is then re-run for the longer `final_duration_ms` validation period.

Intermediate probe results are retained under `search/<case-id>/`.

## results.json

`results.json` is the canonical aggregate for convenient programmatic consumption.

Shape:

```json
{
  "schema_version": 1,
  "plan_id": "...",
  "summary": {
    "pass": 10,
    "unstable": 2,
    "error_or_other": 1
  },
  "results": []
}
```

The `results` array contains the final case result objects.

Timing/performance analysis should use `results.json` or the underlying case/search JSON, not values parsed from Markdown.

## results.csv

The CSV is a convenience projection for plotting and spreadsheets.

v1 columns are:

```text
case_id
type
mode_id
crop_x
crop_y
crop_width
crop_height
status
best_frame_duration_us
best_measured_fps
requests_completed
sequence_gaps
queue_overruns
error
```

The CSV is derived from the JSON result set. JSON remains authoritative when a field is not represented in the CSV.

## report.md

A human-readable table and summary generated deterministically from the case results.

It reports:

- case ID;
- test type;
- mode/crop;
- status;
- maximum stable FPS;
- frame duration;
- completed frames;
- sequence gaps;
- queue overruns.

It is a presentation artifact, not the canonical data source.

## samples.json and samples/

Visual samples are generated **after** timing qualification.

`samples.json` has the v1 wrapper:

```json
{
  "schema_version": 1,
  "samples": []
}
```

The runner attempts to preserve:

- one baseline sample from a passing large advertised mode;
- one sample from the fastest passing case.

The sample capture reproduces the qualified stream kind, mode/crop and discovered frame duration.

PNG/DNG/video files are derived evidence. The corresponding qualification JSON remains authoritative for FPS/drop claims.

## campaign_status.json

This is the final promotion gate:

```json
{
  "schema_version": 1,
  "valid": true,
  "reason": null
}
```

Only a run with `valid: true` is eligible for promotion to the checked-in reference dataset.

A valid campaign status does not mean every tested geometry passed. Unsupported, unstable or rejected cases are legitimate scientific results. It means the campaign environment itself remained acceptable.

## Recovery failure

If an experimental failure is followed by a failed known-good recovery capture, the runner writes:

```text
recovery_failure.json
```

and aborts.

A run containing this file must not be promoted as complete official data.

## Promotion requirements

Use the guarded promotion command rather than manually copying a run:

```bash
hscam-qualify promote RUN_DIR \
  --results-root qualification/results
```

The promoter checks schema versions, campaign validity, clean committed source
provenance, required start/end environment artifacts, absence of
`recovery_failure.json`, and enabled visual-sample representation. It copies
derived visual evidence while intentionally omitting large raw `.hscap`
sample directories and records omissions in `published.json`.

Reference results under `qualification/results/` should satisfy all of the following:

1. `campaign_status.json` exists and contains `"valid": true`.
2. `plan.json` identifies the intended committed campaign.
3. `source_revision` identifies a clean source tree and does not end in `-dirty`.
4. `environment.json` and `environment_end.json` are present.
5. `camera.json` identifies the tested camera.
6. `results.json`, `results.csv` and `report.md` are present.
7. `recovery_failure.json` is absent.
8. Visual evidence is present when enabled by the campaign, or the failure to generate it is explicitly represented in `samples.json`.
9. No result files are manually rewritten to make a test appear successful.

Rejected and unstable test cases should remain in the dataset. They define the measured boundary and are part of the result.

## Reproducibility boundary

Qualification data is valid for the recorded combination of:

- Raspberry Pi hardware;
- camera/sensor;
- OS/kernel;
- libcamera version;
- hscam source revision;
- campaign manifest;
- power/thermal conditions.

A future kernel or libcamera update may change the observed capabilities. New runs should coexist with the older dataset rather than silently replacing its provenance.


## published.json

A promoted result contains a small publication receipt:

```json
{
  "schema_version": 1,
  "campaign": "official-rpi-cameras-v1",
  "plan_id": "...",
  "source_revision": "...",
  "camera_id": "...",
  "camera_model": "imx296",
  "sensor": "imx296",
  "platform": "Raspberry Pi 5 Model B",
  "raw_sample_bundles_omitted": [
    "fastest.hscap"
  ]
}
```

This file is generated by the promotion command. It documents the identity of
the published run and which large raw sample bundles were intentionally not
copied into Git. It is not a replacement for `plan.json`,
`environment.json`, or `results.json`.

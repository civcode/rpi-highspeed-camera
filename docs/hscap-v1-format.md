# HSCAP v1 capture bundle format

HSCAP is the native capture artifact used by `rpi-highspeed-camera`.

The format is intentionally a directory rather than a monolithic container. The raw payload remains byte-for-byte camera data, while geometry, format and timing information are stored separately so a capture can be reconstructed without command-line hints.

This document describes **schema version 1** as implemented by v0.1.

## Directory layout

```text
capture.hscap/
    manifest.json
    frames.bin
    frames.idx
    metadata.jsonl
```

Derived PNG, DNG or video files are not part of the canonical HSCAP bundle.

## manifest.json

The manifest contains capture-wide state.

Example:

```json
{
  "schema_version": 1,
  "complete": true,
  "frame_count": 120,
  "camera": {
    "id": "...",
    "model": "imx296"
  },
  "sensor": {
    "mode_id": null,
    "crop": [0, 496, 1456, 96]
  },
  "stream": {
    "kind": "processed",
    "pixel_format": "YUV420",
    "width": 1456,
    "height": 96,
    "frame_bytes": 209664,
    "planes": [
      {
        "stride": 1472,
        "size": 209664
      }
    ]
  },
  "timing": {
    "requested_frame_duration_us": 1866,
    "exposure_us": null,
    "analogue_gain": null
  }
}
```

Values above are illustrative.

### Fields

- `schema_version`: HSCAP manifest schema. v0.1 writes `1`.
- `complete`: `true` only when the writer reached normal finalization. An interrupted or recovered bundle remains `false`.
- `frame_count`: number of indexed frames considered part of the bundle.
- `camera.id`: libcamera camera identifier recorded at capture time.
- `camera.model`: camera model string reported by the camera inventory.
- `sensor.mode_id`: selected advertised mode identifier, or `null` when no advertised mode ID was explicitly selected.
- `sensor.crop`: negotiated sensor crop as `[x, y, width, height]`, or `null`.
- `stream.kind`: `raw` or `processed`.
- `stream.pixel_format`: negotiated libcamera pixel-format string.
- `stream.width` / `height`: negotiated stream dimensions.
- `stream.frame_bytes`: negotiated frame size from libcamera.
- `stream.planes`: **observed** plane layout from delivered buffers. Each object records `stride` and `size`.
- `timing.requested_frame_duration_us`: requested fixed frame duration, if one was supplied.
- `timing.exposure_us`: requested exposure, if supplied.
- `timing.analogue_gain`: requested analogue gain, if supplied.

The observed plane layout is persisted before the first frame payload is written. This is deliberate: a hard-killed capture can still be reconstructed during recovery.

## frames.bin

`frames.bin` is the concatenation of captured frame payloads.

For each frame, every delivered plane is written in plane order. The bytes are not:

- debayered;
- normalized;
- re-packed;
- stripped of row padding;
- compressed.

The pixel format and observed plane layout in `manifest.json` define how those bytes are interpreted.

HSCAP does not impose byte order on the sensor payload itself. Payload representation is whatever the negotiated libcamera pixel format defines.

## frames.idx

`frames.idx` provides random access to `frames.bin`.

All integer fields in the index are **unsigned 64-bit little-endian** values.

### Header

The file begins with the eight ASCII bytes:

```text
HSCIDX01
```

There is no terminating NUL byte.

### Record

Each frame then has one fixed 32-byte record:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | frame number |
| 8 | 8 | byte offset in `frames.bin` |
| 16 | 8 | payload length |
| 24 | 8 | metadata line number |

For v1:

- frame numbers start at zero and increase by one;
- payload offsets are contiguous;
- metadata line number equals frame number.

Readers must reject index/manifest count mismatches during normal opening.

## metadata.jsonl

There is one JSON object per frame, one object per line.

A normal metadata record has this shape:

```json
{
  "frame": 123,
  "sequence": 123,
  "request_cookie": 2,
  "sensor_timestamp_ns": 2303456789000,
  "exposure_us": 100,
  "frame_duration_us": 1866,
  "analogue_gain": 1.0,
  "status": "complete"
}
```

Nullable fields are written as JSON `null`.

Large integer timestamps are serialized as exact JSON integers. They are not routed through floating-point storage.

Normal frame status values are:

- `complete`
- `cancelled`
- `error`

Recovery may additionally write:

- `recovered_without_metadata`

A frame with `recovered_without_metadata` has valid indexed payload bytes, but its original per-frame timing/control metadata did not survive. Such a frame is suitable for visual salvage but **must not be used as timing evidence**.

## Finalization

A normal writer:

1. writes an initial manifest with `complete: false`;
2. persists observed plane layout before writing the first payload;
3. appends payload, index and metadata records;
4. flushes the three stream files;
5. closes them;
6. atomically replaces the manifest with `complete: true` and the final frame count.

Consumers should treat `complete: false` as an explicit warning that the capture did not finish normally.

## Interrupted-capture recovery

Recovery is explicit and destructive:

```bash
hscam-export recover capture.hscap
```

The recovery algorithm treats the index and payload as the primary evidence.

It:

1. validates the `HSCIDX01` header;
2. scans only complete 32-byte index records;
3. requires frame numbers, metadata indexes and payload offsets to be contiguous;
4. stops before an index record whose payload extends beyond `frames.bin`;
5. truncates any partial index record;
6. truncates unreferenced trailing payload bytes;
7. retains complete metadata lines that correspond to recovered frames;
8. discards a partial trailing metadata line;
9. synthesizes `recovered_without_metadata` lines for valid payload frames whose metadata was lost;
10. rewrites `manifest.json` with the recovered frame count.

A repaired interrupted bundle stays `complete: false`. Recovery is intended to make surviving data readable, not to convert an interrupted experiment into a valid qualification run.

The recovery command reports:

- whether the original manifest was complete;
- recovered frame count;
- discarded index bytes;
- discarded payload bytes;
- discarded metadata bytes;
- number of synthesized metadata records.

## Reader invariants

A v1 reader should reject at least:

- unsupported `schema_version`;
- invalid index magic;
- partial index records during normal opening;
- index record count different from `manifest.frame_count`;
- out-of-range frame requests;
- indexed payload shorter than the requested frame.

`recoverBundle()` is the explicit mechanism for repairing an interrupted bundle before normal opening.

## Schema evolution

HSCAP is pre-1.0 and the format may evolve.

Rules for future changes:

- incompatible manifest/index changes require a new schema/index version;
- existing v1 meanings must not silently change;
- derived rendering information must not replace the canonical raw payload;
- new metadata fields should be additive where possible;
- qualification results must record the software/source revision that produced a bundle.

## Relationship to verification output

HSCAP is the source capture artifact. PNG, DNG and slow-motion video are derived verification artifacts.

A visually plausible PNG or video does not establish timing correctness. High-FPS claims must use the original per-frame sensor timestamps, sequence information and capture statistics.

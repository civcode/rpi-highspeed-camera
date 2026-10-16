# Experimental high-FPS development roadmap

> **Active roadmap as of October 2026.** This replaces the v0.1 fixed-mode
> completion roadmap as the project plan. The milestones are dependencies, not
> a promise that hardware results already exist. Only mark each acceptance
> gate complete after the named evidence is available.

## Acceptance definition

This project is successful when it can run independently verified experimental
readout on Raspberry Pi 5, preserve the RAW/image and timing evidence, and
extend the same C++ interface to all six official Raspberry Pi sensor families.
"Supports all" means detection, truthful strategy/capability classification
and at least one experimentally characterized path **where physically possible**
—not an unsupported claim that every camera can reach the same FPS.

| Phase | Deliverables | Acceptance gate | Status |
| --- | --- | --- | --- |
| H0 — Recharter | Canonical goals, source/doc audit, historical docs marked | No current document misstates the research goal | Implemented in repository |
| H1 — Strategy API | Public C++ experiment strategy/target types; all official sensors identified; host tests | Six model classification fixtures pass without camera or false FPS claims | Implemented; standalone C++20 host smoke passed, full repository CTest pending |
| H2 — Discover CLI | Separate `hscam-highspeed` dry-run inventory; JSON/human; no register writes | Works on no-camera host gracefully, identifies current driver limitations on Pi | Implemented in repository; Pi execution pending |
| H3 — Experimental-only tests | Exact candidate-size matching and fail-closed manifest; no normal-mode fallback | Stock OV5647 finds zero experimental modes, never mislabels baseline | Implemented in repository; Pi execution pending |
| H4 — Kernel development safety | Documented kernel source pin, preflight, patch/build/rollback procedure, driver binding rules | Alternate kernel can be selected and restored on test Pi | **Pi 5 preflight confirmed**: OV5647 bound at `10-0036` on `6.18.50+rpt-rpi-2712`; pinned non-root source preparation added; alternate kernel boot/rollback **not yet tested** |
| H5 — First OV5647 high-FPS driver | Actual OV5647 register/timing profiles for 640×240, 640×128 etc in kernel mode table; correct V4L2/CSI metadata | Compile verified candidate; reversible single-driver deployment; capture visually valid, frame-complete reduced-height RAW on Pi 5 | **640×240 candidate source transformer and compile-only helper committed**; module compilation, driver deployment and Pi measurements pending; 640×128/64/32 not implemented |
| H6 — FPS discovery & proof | Search + long-run validation on new profiles, image validation, reproducible logs | Real 10–60 s high-FPS run, drop/jitter results, full provenance and no throttling | Engine exists; **experimental hardware evidence pending** |
| H7 — Dynamic ROI driver | Optional sensor-specific `.set_selection` + TRY/ACTIVE and timing constraints; no broad unsafe register sweeps | Exact ROI survives configure/start, readback matches, measured FPS changes with sensor height | Not implemented |
| H8 — Additional cameras | IMX219, IMX708, IMX477, IMX500 sensor strategies, kernel profiles where necessary; IMX296 native ROI | Each is independently characterized; unsupported limitations documented | Detection/strategy foundations implemented; hardware work pending |
| H9 — Release/reproducibility | Per-kernel patches, golden visual/timing samples, release docs, licensing, published hardware profiles | Hardware matrix, rollback, provenance and CI/host tests complete | Not implemented |

### H5 experimental implementation procedure

1. Pin installed Raspberry Pi kernel commit and build configuration; capture
   unmodified `ov5647.c` and V4L2/media graph state.
2. Derive one **coherent** reduced-height register list from the historical
   `raspiraw` profile and the **current** Linux driver: start/end registers,
   output width/height, horizontal/vertical increments, HTS/VTS, pixel and
   link rates, exposure range, CSI packet/line expectations.
3. Port as one **opt-in experimental mode** while keeping stock modes intact.
   Compile and install on an alternate bootable kernel; never bind competing
   modules simultaneously. Verify power-management/start/stop order.
4. On Pi 5, request the new exact sensor mode, confirm RP1 CFE format/size,
   use unpacked RAW for image proof, then measure FPS with correct timestamps.
5. If libcamera refuses or overrides the mode, inspect the V4L2 media graph
   and libcamera sensor-mode metadata; fix coherency rather than pretending
   that a resized processed frame is sensor-level high speed.
6. Only then add shorter heights, vertical skipping profiles, longer captures
   and optional dynamic ROI controls.

No fabricated register values are included here. Register-level experiments
and kernel module replacement must be tested on the actual board.

### H8 supported-family strategy targets

| Sensor | Starting strategy | Notes |
| --- | --- | --- |
| OV5647 | Experimental mode table | Legacy `raspiraw` readout profiles are research inputs, not directly portable code |
| IMX219 | Experimental mode table | Legacy modes may require different PLL/CSI settings |
| IMX708 | Experimental mode table | Autofocus/HDR interactions must be checked |
| IMX477 | Experimental mode table | Large RAW bandwidth and driver timing restrictions |
| IMX296 | Native mutable sensor ROI | Validate GScrop-style reduced-height readout, no custom driver required initially |
| IMX500 | Experimental support investigation | Firmware/inference requirements, no assumption that arbitrary ROI is feasible |

### Measurement requirements

For every candidate retain: camera ID/model, stock vs experimental driver
identity, kernel and module hashes, Pi variant, I²C/CSI sensor mode metadata,
actual sensor readout geometry, requested vs measured FPS, inter-frame
distribution, sequence gaps, queue drops, latency, bytes/s, thermals,
capture duration, and representative unmodified HSCAP/PNG evidence.

Benchmarks are **experimental** only if their reduced-height sensor mode or
ROI is actually verified—not solely because a small output was requested.
Regular advertised stock modes remain regression tests.

### Dependency and risk notes

- H5 requires **physical Pi 5 access and kernel rollback**; host tests cannot
  finish it.
- User-space I²C register poking while `ov5647` is bound is not supported.
- Sensor register/table details must be established per model, not generalized
  blindly from OV5647 to IMX500.
- H3 mode matching is intentionally conservative; a later explicit driver
  attestation protocol is needed before trusting arbitrary experimental modes.
- The legacy official six-camera qualification/promotion campaign remains
  available but no longer constitutes the central release gate.

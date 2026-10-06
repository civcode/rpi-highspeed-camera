# Contributing

The project is pre-1.0 and still completing its first physical Raspberry Pi
qualification campaign. Changes should preserve the distinction between the
capture library, offline rendering/export, and the one-time qualification
protocol.

## Development workflow

Use the existing branch/repository workflow. Keep changes small enough to
review and validate independently.

For implementation work:

1. build locally;
2. run the relevant host tests;
3. run hardware tests locally on Raspberry Pi when the change touches
   libcamera, media/V4L2, buffer handling or timing;
4. keep benchmark/result artifacts separate from source changes;
5. document anything that remains unverified.

Do not use GitHub Actions as a substitute for local builds or Raspberry Pi
hardware validation. The repository's qualification campaign is intentionally
local and explicit.

## Build and host tests

A host-safe library/test build can be run without libcamera:

```bash
cmake -S . -B build \
  -DHSCAM_BUILD_TOOLS=OFF \
  -DHSCAM_BUILD_TESTS=ON

cmake --build build -j
ctest --test-dir build --output-on-failure
```

On Raspberry Pi OS, configure with:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DHSCAM_REQUIRE_LIBCAMERA=ON
```

Then build and run CTest locally.

## Capture-path rules

Capture changes must not introduce hidden work into the libcamera completion
path.

In particular:

- do not debayer, encode, perform CV or write files in the completion callback;
- do not silently copy payloads;
- do not silently overwrite unconsumed frames;
- do not claim requested FPS as achieved FPS;
- preserve sensor timestamps and observable frame-loss statistics;
- keep sensor crop distinct from ISP/scaler crop;
- do not hard-code `/dev/mediaN`, `/dev/v4l-subdevN` or a specific I2C bus;
- prefer runtime capabilities to sensor-name conditionals.

A sensor-specific quirk should only be added when the standard API path is
demonstrably insufficient and the reason is documented.

## HSCAP changes

HSCAP v1 is documented in
[docs/hscap-v1-format.md](docs/hscap-v1-format.md).

Changes must preserve:

- exact integer timing metadata;
- explicit negotiated pixel format and plane layout;
- deterministic indexed frame access;
- interrupted-capture recovery behavior.

An incompatible format change requires a new schema/index version rather than
silently changing v1 semantics.

## Qualification changes

The official campaign is defined by the committed manifest under
`qualification/manifests/`.

Qualification changes should be reproducible and auditable:

- tests come from the manifest, not undocumented interactive choices;
- risky cases run in isolated worker processes;
- timeouts and recovery failures remain visible;
- rejected/unstable configurations are retained as results;
- environment and source provenance are preserved;
- only a valid clean-revision campaign may be promoted.

Do not manually edit numeric result files to make a case pass.

## Published results

Working output belongs in `qualification/work/`.

Use the promotion command for a valid run:

```bash
hscam-qualify promote RUN_DIR \
  --results-root qualification/results
```

Large raw HSCAP sample bundles are intentionally not promoted by default.
Derived PNG/DNG/video evidence and the result/provenance files are retained.

## Coding style

- C++20;
- RAII for owned resources;
- move-only types for exclusive resource handles;
- public headers should avoid leaking libcamera/V4L2 implementation types;
- use ordinary value types, `std::chrono`, `std::optional` and
  `std::span` where appropriate;
- prefer a straightforward measured implementation over speculative
  lock-free/zero-copy complexity.

Compile with the warning flags already configured by CMake and address new
warnings.

## Licensing

The repository currently has no explicit software license. Do not assume an
open-source license applies.

The repository owner should select a license before normal external
contribution/reuse is encouraged. Until then, contribution policy is
necessarily provisional.

# Raspberry Pi OS camera-stack ABI mismatch on a new Pi 5 installation

> **Historical field report, not high-FPS experimental evidence.**
> Still useful for diagnosing libcamera mismatches during kernel-driver
> experimentation. See [current project goals](project-goals.md).

> **Field report — 16 October 2026.** Reproduced on a Raspberry Pi 5 running
> Raspberry Pi OS Trixie (arm64), using the Raspberry Pi package repository.
> **Resolved for this installation** by updating the `rpicam-apps` application
> package set to match the installed libcamera ABI.
>
> This document records a concrete installation failure. It does **not** imply
> that all fresh Raspberry Pi OS installations are affected, or establish why
> the original image contained a mismatched package set.

## Summary

The camera sensor and `libcamera` were functional, and this project's
`hscam` capture backend could build and capture frames, but the preinstalled
`rpicam-hello` and `rpicam-raw` applications failed:

- `rpicam-hello --list-cameras` terminated with an **undefined C++ symbol**
  from `librpicam_app.so.1`.
- `rpicam-raw` enumerated the OV5647 but then **segfaulted in
  `OptsInternal::Parse()`**, before configuring its requested RAW stream.
- The installed `librpicam_app.so.1.13.0` was built against older
  `libcamera::Span` function signatures. The installed
  `libcamera.so.0.7.2` exported **`std::span` signatures** (and a changed
  `FrameBuffer` constructor parameter width).

The direct ABI incompatibility is proven by the linker/symbol inspection.
The ABI mismatch is a credible explanation for the option-parser segfault,
but the backtrace alone does not establish its exact mechanism.

After synchronizing the `rpicam-apps` packages, `ldd -r` found no
unresolved symbols, `rpicam-hello --list-cameras` worked, and
`rpicam-raw` successfully negotiated and captured unpacked OV5647 Bayer.

## Verified hardware and software

| Item | Observed value |
| --- | --- |
| Board | Raspberry Pi 5 Model B |
| SoC / pipeline | BCM2712 / `rpi/pisp` |
| PiSP | `libpisp 1.7.0-1` |
| OS/package suite | Raspberry Pi OS Trixie, arm64 |
| Package source | `http://archive.raspberrypi.com/debian trixie/main` |
| `libcamera0.7` | `0.7.2+rpt20261009-1` |
| `libcamera-dev` | `0.7.2+rpt20261009-1` |
| `libcamera-ipa` | `0.7.2+rpt20261009-1` |
| `rpicam-apps` before repair | `1.13.0-1` |
| `rpicam-apps-core` before repair | `1.13.0-1` |
| `librpicam_app` before repair | `librpicam_app.so.1.13.0` |
| Available `rpicam-apps[-core]` upgrade | `1.14.0-1` |
| Camera | OV5647 Camera Module v1 |
| libcamera camera ID | `/base/axi/pcie@1000120000/rp1/i2c@88000/ov5647@36` |

The *actual installed application package versions after repair were not
captured in the supplied logs*. `1.14.0-1` was the package-manager
**candidate**; confirm the installed versions with `dpkg-query` rather than
assuming them from the candidate.

### Symptoms before repair

```console
$ rpicam-hello --list-cameras
rpicam-hello: symbol lookup error:
/lib/aarch64-linux-gnu/librpicam_app.so.1:
undefined symbol:
_ZN9libcamera6Camera21generateConfigurationENS_4SpanIKNS_10StreamRoleELm18446744073709551615EEE
```

`ldd -r /lib/aarch64-linux-gnu/librpicam_app.so.1` reported two
unresolved symbols:

```text
libcamera::Camera::generateConfiguration(
    libcamera::Span<const libcamera::StreamRole>)

libcamera::FrameBuffer::FrameBuffer(
    libcamera::Span<const libcamera::FrameBuffer::Plane>,
    unsigned int)
```

These signatures are expressed in demangled C++ form here for readability;
the diagnostic output contained their mangled ELF names.

Meanwhile, `nm -D -C /lib/aarch64-linux-gnu/libcamera.so.0.7` showed
that the **loaded libcamera library** exported:

```text
libcamera::Camera::generateConfiguration(
    std::span<const libcamera::StreamRole>)

libcamera::FrameBuffer::FrameBuffer(
    std::span<const libcamera::FrameBuffer::Plane>,
    unsigned long)
```

This is an **ABI break**, not merely a difference in header spelling.
C++ name mangling includes the parameter types, so a binary requiring
`libcamera::Span` cannot link against a library exporting only
`std::span` under that name.

The executable paths were the distribution binaries,
`/usr/bin/rpicam-hello` and `/usr/bin/rpicam-raw`. There were no
`LD_LIBRARY_PATH` or `LD_PRELOAD` overrides in the reported environment.
`dpkg -V libcamera0.7 rpicam-apps-core` produced no reported file
verification differences. The installed files were package-owned:

```text
libcamera0.7:arm64: /usr/lib/aarch64-linux-gnu/libcamera.so.0.7.2
librpicam-app1:arm64: /usr/lib/aarch64-linux-gnu/librpicam_app.so.1.13.0
```

The available `rpicam-apps-core` and `rpicam-apps` packages were
`1.14.0-1`, newer than the installed `1.13.0-1`.

### The segfault was not evidence of a RAW-format failure

Before repair, `rpicam-raw --mode 640:480:10:U` started libcamera, loaded
the OV5647 tuning file, registered the camera with PiSP, then crashed.

GDB's main-thread backtrace was:

```text
libc.so.6
OptsInternal::Parse()
Options::Parse(int, char**)
VideoOptions::Parse(int, char**)
rpicam-raw
```

The camera-manager/IPA threads were still running. No requested RAW
stream configuration had been completed. Consequently, this crash said
nothing about whether unpacked RAW was supported by the sensor or PiSP.

## Diagnostic procedure

Use these commands **before** attempting driver changes, kernel updates,
custom libcamera builds, or camera-cable troubleshooting:

```bash
# Identify the executable paths.
type -a rpicam-hello rpicam-raw
readlink -f "$(command -v rpicam-hello)"
readlink -f "$(command -v rpicam-raw)"

# Show the libraries loaded and any unresolved relocations.
ldd -r /lib/aarch64-linux-gnu/librpicam_app.so.1 2>&1 \
  | grep -E 'libcamera|undefined|not found'

# Compare installed versions with available Raspberry Pi OS packages.
apt-cache policy \
  libcamera0.7 libcamera-dev libcamera-ipa \
  rpicam-apps-core rpicam-apps librpicam-app1

# Verify package-managed binaries/libraries.
dpkg -V libcamera0.7 rpicam-apps-core librpicam-app1

# Determine which packages own the loaded shared objects.
dpkg -S \
  "$(readlink -f /lib/aarch64-linux-gnu/libcamera.so.0.7)" \
  "$(readlink -f /lib/aarch64-linux-gnu/librpicam_app.so.1)"

# Inspect exported function signatures.
nm -D -C /lib/aarch64-linux-gnu/libcamera.so.0.7 \
  | grep -E 'Camera::generateConfiguration|FrameBuffer::FrameBuffer'

# Rule out overrides from the environment.
env | grep -E '^LD_(LIBRARY_PATH|PRELOAD)=' || true
```

`dpkg -V` producing no output is expected when it detects no relevant
package-file discrepancies. It does **not** independently prove that two
otherwise intact Debian packages were built against compatible ABIs.

If linker inspection succeeds but an application still crashes,
collect a backtrace:

```bash
gdb -q -batch \
  -ex run \
  -ex 'thread apply all bt 12' \
  --args rpicam-raw \
    --nopreview --mode 640:480:10:U -t 1000 \
    -o /tmp/ov5647-unpacked.raw
```

## Repair procedure used for this installation

The package manager reported the newer `rpicam-apps` version
`1.14.0-1` while `libcamera0.7` was already at its current candidate
`0.7.2+rpt20261009-1`. Upgrading the application-side package set was
the lower-risk first intervention.

**1. Refresh package metadata and simulate the upgrade:**

```bash
sudo apt update

sudo apt-get -s install \
  rpicam-apps \
  rpicam-apps-core \
  librpicam-app1 \
  rpicam-apps-encoder \
  rpicam-apps-preview \
  rpicam-apps-opencv-postprocess
```

Review the simulation: it should choose a consistent camera-app stack,
with no unexpected package removals or libcamera downgrades. Package
availability and dependency names can change in later Raspberry Pi OS
releases; use `apt-cache policy` for the system being repaired.

**2. If the proposed changes are appropriate, install them:**

```bash
sudo apt-get install \
  rpicam-apps \
  rpicam-apps-core \
  librpicam-app1 \
  rpicam-apps-encoder \
  rpicam-apps-preview \
  rpicam-apps-opencv-postprocess
```

There is no evidence in this incident that rebuilding the sensor driver,
changing boot overlays, recompiling PiSP, or reinstalling the same
libcamera version would address the symbol mismatch. Avoid changing the
working `libcamera` runtime in isolation.

## Verified result after repair

The user reran:

```bash
ldd -r /lib/aarch64-linux-gnu/librpicam_app.so.1 \
  2>&1 | grep 'undefined symbol' || echo "No unresolved symbols"

rpicam-hello --list-cameras
```

The output was `No unresolved symbols`, followed by successful OV5647
enumeration and these libcamera-advertised modes:

| Mode | Reported maximum FPS | Reported sensor crop |
| --- | ---: | --- |
| 640×480 | 62.50 | `(16,0)/2560x1920` |
| 1296×972 | 46.34 | `(0,0)/2592x1944` |
| 1920×1080 | 32.81 | `(348,434)/1928x1080` |
| 2592×1944 | 15.63 | `(0,0)/2592x1944` |

A subsequent unpacked-RAW test succeeded:

```bash
rpicam-raw \
  --nopreview \
  --mode 640:480:10:U \
  -t 1000 \
  -o /tmp/ov5647-unpacked.raw
```

Relevant output:

```text
Mode selection for 640:480:10:U
    SGBRG10_CSI2P,640x480/0 - Score: 0
Stream configuration adjusted
configuring streams: (0) 640x480-YUV420/SMPTE170M
                     (1) 640x480-SGBRG16/RAW
Selected sensor format: 640x480-SGBRG10_1X10/RAW
Selected CFE format: 640x480-GB16/RAW
Raw stream: 640x480 stride 1280 format SGBRG16
```

The output file was reported as approximately 11 MB. The file size alone
does **not** demonstrate the advertised 62.5 FPS—frame counts and sensor
timestamps must be checked separately. It does establish that **unpacked
Bayer acquisition is possible on this tested Pi 5/PiSP installation**.

Warnings about unsupported `Nc30` and `Nc12` V4L2 pixel formats occurred
during this successful configuration; they were not fatal to this test.

## Separate build prerequisite: `libcamera-dev`

The new Raspberry Pi OS installation also needed the development package to
compile this repository against the actual installed libcamera headers:

```bash
sudo apt install build-essential cmake pkg-config libcamera-dev
```

Then build with a required camera backend:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DHSCAM_REQUIRE_LIBCAMERA=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The distinction is important:

- `libcamera0.7` is the **runtime** shared library.
- `libcamera-dev` provides the **headers and build integration** needed to
  compile `hscam` with libcamera support.
- `rpicam-apps` is a separate **application suite** linked against libcamera.

A working `rpicam-hello` does not imply that `libcamera-dev` is installed,
and a working `hscam` executable does not imply the system's `rpicam-apps`
binary was linked against a compatible ABI.

For optional DNG export install `libtiff-dev`; for video export install
`ffmpeg`.

## Implications for this project

1. **Pin the complete environment for qualification.** Record Raspberry Pi
   OS suite, kernel, `libcamera`, `libpisp`, `rpicam-apps`, relevant
   package versions, and the `hscam` source revision.
2. **Require `libcamera-dev` for real Pi builds.** Do not silently accept
   the host-only fallback when validating a camera.
3. **Keep linking/packaging failures separate from sensor capability tests.**
   An app crashing before configuration is not proof a Bayer packing or
   sensor-crop request was rejected by hardware.
4. **Check the negotiated stream, not just the request.** This Pi 5 produced
   `SGBRG16` unpacked RAW when explicitly requested, whereas the initial
   `hscam` raw-role defaults produced PiSP `GBRG_PISP_COMP1`.
   Format negotiation and recorded storage layout matter.
5. **Do not publish conclusions about fresh installs generally** from this
   incident alone. The observed original-package mismatch is established;
   the precise reason that package combination was present on the fresh
   installation was not determined.

## References

- [Raspberry Pi camera software documentation](https://www.raspberrypi.com/documentation/computers/camera_software.html) — `rpicam-apps`, Debian packages, and dependencies.
- [Raspberry Pi `rpicam-apps` source](https://github.com/raspberrypi/rpicam-apps) — reference camera applications.
- [libcamera: transition from `Span` to `std::span` (2026)](https://patchwork.libcamera.org/patch/28152/) — context for the ABI-changing type transition.
- [Pi 5/libcamera compatibility fixes in this repository](https://github.com/civcode/rpi-highspeed-camera/commit/87b7534065706dc42cfa86df8c1c066cef39ccce) — the independent `hscam` build fixes, distinct from the system application ABI mismatch.

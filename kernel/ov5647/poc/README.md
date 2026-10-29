# OV5647 reduced-height POC — 2026-10-29

Small, opt-in research experiment, separate from the original pinned module.
All three additional fixed sensor-output modes are now compiled, booted,
enumerated and benchmarked on edge (Pi 5, 6.18.50+rpt-rpi-2712).
**Physical ROI, row sampling and image fidelity are NOT yet validated.**

## Reconstruction, not arbitrary register scanning

Sources are pinned by commit and SHA256 in `sources.json`:
- OmniVision preliminary datasheet, timing registers (§6.4):
  https://cdn.sparkfun.com/datasheets/Dev/RaspberryPi/ov5647_full.pdf
- Hermann-SW/fork-raspiraw commit `18fac55136f98960ccd4dcfff95112134e5e45db`,
  `ov5647_modes.h`, `raspiraw.c`, tools/640x128, tools/640x64, tools/640x32.
- Audited packaged Linux stock source, SHA256
  `da354174e9a9aba57fb59bd72650c199a7ab5fc03b3e1cdc65557a35c571a75c`.

Legacy scripts select mode 7, height H, top 0, vertical increment **hex 0x17**.
`raspiraw.c` writes height to 0x380a/b and increment to 0x3815. Top 0 is
actually a no-op (`if (cfg.top > 0)`); source-window end remains 0x079f.
Legacy VTS is floor(1e9 / (21165 ns * requested FPS)).

| Output | Legacy target | Legacy VTS | POC VTS | Why different |
|---|---:|---:|---:|---|
| 640x128 | 349 | 135 | 152 | Linux VBLANK_MIN=24 retained |
| 640x64 | 659 | 71 | 88 | Linux VBLANK_MIN=24 retained |
| 640x32 | 749 | 63 | 64 | Conservative 32-line blanking |

POC keeps HTS=1852, PLL 0x3036=0x69, pixel rate 87.5 MHz, full-rate CSI
link, 0x3814=0x71, 0x3815=0x17, 0x370c=0x0f, 0x4837=0x17.
Linux orientation/common-init/control handling remains unchanged; this is
not wholesale copying of the legacy initialization sequence. No PLL sweep,
userspace I2C writes, or reduced minimum blanking is performed.

The new modes retain stock full-window crop *metadata* because inferring
an effective physical ROI from H*4 would be unproven. Therefore these are
reduced-height sensor-output experiments, NOT proven exact physical crops.
The existing 640x240 profile retains its original 0x3815=0x35/window settings.
All four stock register tables and mode order are preserved verbatim.

## Results

RAW10 in left-aligned SGBRG16, 1280-byte stride, two application DMA buffers.
Payload was saved during all probes and final soaks, not discarded.

| Output | Requested final FPS | Steady measured FPS | Saved frames | Gaps/overruns |
|---|---:|---:|---:|---|
| 640x128 | 300 | 298.62 | 3000 | 0/0 |
| 640x64 | 500 | 502.14 | 5000 | 0/0 |
| 640x32 | 650 | 655.95 | 6500 | 0/0 |

Steady statistics exclude the first 0.5 s of requested frames; original
metadata/logs retain startup. Whole-run averages: 298.545, 501.732, 655.398.
Startup max intervals were 9–10 ms; steady maxima were 3.430, 2.037, 1.585 ms.
At 700 requested FPS, 640x32 had a 41.151-ms steady interval and was rejected.
No CSI/capture errors found in the bounded kernel-log review. Stock VGA
recovery captures succeeded after every profile. These are tested rates,
not exhaustive maximum searches or production qualification.

Images at high rates are near black (RAW p95=16–17 / 1023). Additional 30-FPS,
25-ms exposure / 8x gain images have p95=36/39/38 for 128/64/32;
240-row control p95=48, stock VGA control p95=107. Thus all current controls
are dim. Three adjacent sampled payload hashes differ in each soak, but
noise alone can do that: this does NOT prove independent scene sampling.
A well-lit chart and moving/timed target are still needed to validate ROI,
Bayer phase, aspect ratio and nonrepeating rows. No image-validity pass claimed.

## Files and reproduction

Implementation: `generate.py`, `benchmark.py`, `manage.sh`, two test files,
`fetch_sources.py`, `sources.json`, all in this directory. Manual exposure/gain
support in the generic qualifier is a separate change from this POC runner.

From repository root:

```sh
python3 kernel/ov5647/poc/fetch_sources.py
python3 kernel/ov5647/poc/test_profiles.py
python3 kernel/ov5647/poc/test_benchmark.py
# Requires the audited stock source at the original generator's default path.
# Refuses to overwrite an existing generated POC directory.
python3 kernel/ov5647/poc/generate.py
make -C /lib/modules/$(uname -r)/build M="$PWD/kernel/work/ov5647-poc-module" modules
# Deployment is guarded by exact kernel, previous/new hashes and vermagic.
sudo bash kernel/ov5647/poc/manage.sh stage
sudo reboot
# Reconnect, verify live srcversion + all mode IDs before running:
python3 kernel/ov5647/poc/benchmark.py --output qualification/work/NEW-UNUSED-DIR
```

Existing evidence, preserved (~1.9 GiB):
`qualification/work/ov5647-poc-128-64-32-20261029/`.
Includes per-frame metadata, HSCAP payload/index, raw PNGs, statistics,
JSON results, recovery logs, provenance and kernel log. Bright control PNGs:
`128-bright-25ms-g8.png`, `64-bright-25ms-g8.png`, `32-bright-25ms-g8.png`.

## Current deployment / rollback

POC loaded and selected on next boot. Module SHA256:
`bc88ac19568d23a54785206b5d0aa808e4198b3be24e7b4e797ce7437b35af93`.
Live srcversion `92DF7E5477531205179644B` agrees with built module.
Previous 640x240-only module preserved byte-for-byte at
`/var/lib/hscam-poc/ov5647-before-poc.ko`. Packaged stock module untouched.
Original single-mode staging/probe helpers deliberately reject this new hash.

To return to the previous 640x240 module:

```sh
sudo bash kernel/ov5647/poc/manage.sh restore
sudo reboot
```

After that, the original `manage-module-override.sh unstage` + reboot can
restore packaged stock. Never unload/reload the active sensor driver.

## Capture and lighting insights

Relighting the same scene changed the RAW distribution substantially. At
25 ms / 8x gain, the new 128/64/32-row captures had medians 204/173/154
and 16–25% near-white samples (RAW10 white=1023). They were over-amplified
for parts of that scene. At 1x gain, the subsequent medians were 27/24/23;
near-white samples fell to 4.87% / 0.05% / 0%. These are sequential captures,
not a calibrated gain-linearity experiment: lighting/scene may change.
This supports illumination/exposure as an important cause of the prior dark
frames, but does not independently validate the sensor sampling geometry.

Preserved local comparisons (not committed):
- `qualification/work/ov5647-poc-relit-20261029T184529Z/`: 25-ms / 8x gain
  at 30 FPS, plus short-exposure / 4x gain at 300/500/650 FPS.
- `qualification/work/ov5647-poc-gain1-20261029T184736Z/`: matching requested
  exposure settings at 1x gain. The 500/650-FPS images are still very dim;
  add light rather than extending exposure beyond the frame period.

Current mode IDs (verify with `build/hscam-inspect --json` on another build):

| Sensor output | Mode ID | Tested target FPS | Duration us | Exposure example us |
|---|---|---:|---:|---:|
| 640x128 | `9767e16d3bd1ba1d` | 300 | 3333 | 3000 |
| 640x64 | `8dc77e56ce364936` | 500 | 2000 | 1693 |
| 640x32 | `278ef754bfecebef` | 650 | 1538 | 1227 |

Frame rate is approximately 1e6 / frame duration. Keep exposure shorter
than the duration with margin; shorter exposures reduce motion blur but
need more illumination. Start gain at 1 or 2, then try 4 if needed. Gain
amplifies noise and may clip highlights. The driver quantizes requested
exposure/timing: always inspect the actual per-frame values.

```bash
cd ~/workspace/rpi-highspeed-camera
OUT="qualification/work/capture-$(date +%Y%m%d-%H%M%S-%N)"
build/hscam-capture \
  --mode 9767e16d3bd1ba1d \
  --raw --size 640x128 --representation uncompressed \
  --frame-duration-us 3333 --exposure-us 3000 --gain 1 \
  --frames 600 --output "$OUT.hscap"
build/hscam-export stats "$OUT.hscap" --frame 300
build/hscam-export image "$OUT.hscap" --frame 300 --output "$OUT.png"
```

For bright, low-speed geometry inspection use duration 33333 us, exposure
25000 us, gain 1. For the smaller outputs change **both** mode ID and size,
and use the matching duration/exposure pair above. `--frames` is a count,
not seconds; choose a valid export index smaller than the saved frame count.

HSCAP output directories cannot be overwritten (`--overwrite` does not exist).
Assign a fresh `OUT` each run; do not delete old evidence to reuse a filename.
The POC benchmark likewise refuses an existing output directory.

Data path: CSI/CFE DMA writes RAM buffers; hscam maps these DMA-BUFs and
writes leased frames to HSCAP `frames.bin` through buffered filesystem I/O.
It does not accumulate the entire series before saving. Linux page-cache RAM
can absorb short write bursts; a successful short recording is not proof of
sustained SSD throughput or power-loss durability. PNG reconstruction is
performed after capture, not inside the camera completion callback.

The generic manifest accepts `manual_controls.exposure_us` and
`manual_controls.analogue_gain` for both timing workers and visual evidence.
That is not automatic exposure tuning. Its current 4-ms manifest setting
cannot fit every shorter-period mode; use the POC runner's per-rate exposure
policy for this multi-mode campaign. Its warm-up exclusion is specific to
POC statistics, not a fix to the generic qualification worker.

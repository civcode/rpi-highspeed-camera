#!/usr/bin/env python3
"""Prepare (never install) an OV5647 experimental kernel module source tree.

Strict source integrity guard: only the independently hashed, extracted
Raspberry Pi OS 6.18.50-1+rpt1 ov5647.c baseline is accepted.

The high-speed register/timing profile below is an EXPERIMENTAL CANDIDATE,
adapted from raspiraw's OV5647 mode 7 and the current Linux VGA mode. It is
NOT validated on Raspberry Pi 5, and must never be described as measured FPS.
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import sys

EXPECTED_STOCK_SHA256 = "da354174e9a9aba57fb59bd72650c199a7ab5fc03b3e1cdc65557a35c571a75c"
EXPECTED_KERNEL = "6.18.50+rpt-rpi-2712"

# The old raspiraw mode 7 ran a faster clock and could shorten its output
# height. This initial candidate also reduces the source readout end line.
# The 32-line source overscan mirrors the Linux driver's stock VGA window:
# 1952 addressed lines for its 1920-active-line crop.
REG_UPDATES = {
    0x3036: 0x69,  # PLL multiplier from historical high-clock mode 7
    0x3814: 0x71,  # mode 7 horizontal increment (sums to 8)
    0x3806: 0x03,  # end Y high: 0x03df = 991
    0x3807: 0xDF,  # end Y low: 992 source rows / ~960 active
    0x380A: 0x00,  # output height 0x00f0 = 240
    0x380B: 0xF0,
}
REQUIRED_REGS = set(REG_UPDATES) | {
    0x3800, 0x3801, 0x3802, 0x3803, 0x3804, 0x3805,
    0x3808, 0x3809, 0x3815, 0x3820, 0x3821,
}

# Explicitly programmed before 0x0100 stream-on. The driver also applies
# live HTS/VTS through V4L2 controls during ov5647_stream_on().
EXTRA_REGS = {
    0x370C: 0x0F,  # historical mode 7
    0x380C: 0x07,  # HTS 0x073c = 1852 clocks
    0x380D: 0x3C,
    0x380E: 0x01,  # VTS 0x0108 = 264 lines
    0x380F: 0x08,
    0x4837: 0x17,  # MIPI PCLK period from historical mode 7 (experimental)
}

REG_LIST = re.compile(
    r"(?ms)^static struct regval_list ov5647_640x480_10bpp\[\] = \{\n"
    r"(?P<body>.*?)^\};"
)
REGISTER = re.compile(r"(?m)^(\s*\{0x([0-9a-fA-F]{4}),\s*0x)([0-9a-fA-F]{2})(\},\s*)$")
MODE_ARRAY = re.compile(
    r"(?m)^static const struct ov5647_mode ov5647_modes\[\] = \{\s*$"
)


def patch_driver(source: str) -> str:
    """Patch an unmodified compatible driver source in-memory."""
    if "ov5647_640x240_hscam_10bpp" in source or "hscam_experimental" in source:
        raise ValueError("Experimental profile already present; refusing a second patch")
    match = REG_LIST.search(source)
    if not match:
        raise ValueError("Cannot locate the expected OV5647 VGA register list")

    regs: dict[int, int] = {}
    for row in REGISTER.finditer(match.group("body")):
        address = int(row.group(2), 16)
        if address in regs:
            raise ValueError(f"Duplicate VGA register address 0x{address:04x}")
        regs[address] = int(row.group(3), 16)

    missing = REQUIRED_REGS - regs.keys()
    if missing:
        raise ValueError(f"Missing expected VGA registers: {sorted(missing)}")
    if set(EXTRA_REGS) & regs.keys():
        raise ValueError("Extra timing registers already present: source layout changed")
    if regs[0x0100] != 0x01:
        raise ValueError("VGA register sequence has no expected final stream-on")

    body = match.group("body")
    for address, value in REG_UPDATES.items():
        pattern = re.compile(
            rf"(?mi)^(\s*\{{0x{address:04x},\s*0x)[0-9a-f]{{2}}(\}},\s*)$"
        )
        if len(pattern.findall(body)) != 1:
            raise ValueError(f"VGA register 0x{address:04x} not unique")
        body = pattern.sub(lambda m: m.group(1) + f"{value:02x}" + m.group(2), body)

    # Insert additional register writes before stream-on, not after it.
    sentinel = re.compile(r"(?mi)^\s*\{0x0100,\s*0x01\},\s*$")
    if len(sentinel.findall(body)) != 1:
        raise ValueError("Cannot uniquely locate final stream-on register")
    extra_lines = "".join(
        f"\t{{0x{address:04x}, 0x{value:02x}}},\n"
        for address, value in EXTRA_REGS.items()
    )
    body = sentinel.sub(lambda m: extra_lines + m.group(0), body, count=1)

    candidate_reg_list = (
        "\n/* hscam experimental OV5647 640x240 candidate (UNVALIDATED).\n"
        " * Based on stock VGA registers and historical raspiraw mode 7.\n"
        " * Modifies actual sensor output Y size / end line and sensor timing;\n"
        " * requires Pi 5 CSI-2 readout and image verification before use.\n"
        " */\n"
        "static struct regval_list ov5647_640x240_hscam_10bpp[] = {\n"
        + body + "};\n"
    )
    source = source[:match.end()] + candidate_reg_list + source[match.end():]

    mode_start = MODE_ARRAY.search(source)
    if not mode_start:
        raise ValueError("Cannot locate static OV5647 mode table")
    mode_end = source.find("\n};", mode_start.end())
    if mode_end < 0:
        raise ValueError("Cannot locate end of OV5647 mode table")
    mode_table = source[mode_start.end():mode_end]
    if mode_table.count(".reg_list") != 4:
        raise ValueError("Expected exactly four stock OV5647 modes")

    # Pixel clock: 87.5 MHz; line time 1852/87.5MHz; minimum VTS 264.
    # Nominal rate is 87500000 / (1852 * 264) ~= 178.96 FPS, NOT measured.
    new_mode = """
\t/* hscam experimental 640x240, register profile not hardware validated. */
\t{
\t\t.format = {
\t\t\t.code\t\t= MEDIA_BUS_FMT_SBGGR10_1X10,
\t\t\t.colorspace\t= V4L2_COLORSPACE_RAW,
\t\t\t.field\t\t= V4L2_FIELD_NONE,
\t\t\t.width\t\t= 640,
\t\t\t.height\t\t= 240
\t\t},
\t\t.crop = {
\t\t\t.left\t\t= 16 + OV5647_PIXEL_ARRAY_LEFT,
\t\t\t.top\t\t= OV5647_PIXEL_ARRAY_TOP,
\t\t\t.width\t\t= 2560,
\t\t\t.height\t\t= 960,
\t\t},
\t\t.pixel_rate\t= 87500000,
\t\t.link_freq_index = FREQ_INDEX_FULL,
\t\t.hts\t\t= 1852,
\t\t.vts\t\t= 0x108,
\t\t.reg_list\t= ov5647_640x240_hscam_10bpp,
\t\t.num_regs\t= ARRAY_SIZE(ov5647_640x240_hscam_10bpp)
\t},"""
    source = source[:mode_end] + "\n" + new_mode + source[mode_end:]

    marker = 'MODULE_LICENSE("GPL v2");'
    if source.count(marker) != 1:
        raise ValueError("Cannot uniquely locate module license metadata")
    source = source.replace(
        marker, marker +
        '\nMODULE_INFO(hscam_experimental, "ov5647-640x240-rev1-unvalidated");',
        1,
    )
    return source


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=pathlib.Path,
        default=pathlib.Path("kernel/work/ov5647-6.18.50/"
                             "linux-source-6.18/drivers/media/i2c/ov5647.c"),
    )
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path("kernel/work/ov5647-6.18.50/"
                             "experimental-module"),
    )
    args = parser.parse_args()
    src = args.source.read_bytes()
    found_hash = hashlib.sha256(src).hexdigest()
    if found_hash != EXPECTED_STOCK_SHA256:
        parser.error(
            "Refusing patch: source SHA256 does not match audited Raspberry "
            f"Pi 6.18.50 stock driver ({found_hash}); expected "
            f"{EXPECTED_STOCK_SHA256}"
        )

    out_source = patch_driver(src.decode("utf-8"))
    args.output.mkdir(parents=True, exist_ok=True)
    driver_file = args.output / "ov5647.c"
    makefile = args.output / "Makefile"
    if driver_file.exists() or makefile.exists():
        parser.error("Output already exists; use a fresh output directory to "
                     "avoid overwriting experimental edits")
    driver_file.write_text(out_source)
    makefile.write_text("obj-m += ov5647.o\n")
    (args.output / "stock-source.sha256").write_text(
        f"{found_hash}  {args.source}\n"
    )
    (args.output / "EXPERIMENTAL-NOT-VALIDATED.txt").write_text(
        "The new 640x240 sensor mode is unvalidated on Raspberry Pi 5.\n"
        "No kernel module has been installed or loaded.\n"
        "No achieved FPS or valid CSI readout is claimed.\n"
        "The original unmodified kernel source is preserved.\n"
        "Compile against EXACT running-kernel headers before any live test.\n"
    )
    print(f"Candidate module source: {driver_file}")
    print("Mode: 640x240; pixel_rate=87500000 HTS=1852 VTS=264")
    print("Expected nominal FPS only: "
          f"{87500000 / (1852 * 264):.3f} (unvalidated)")
    print("No module built, installed, loaded or bound; no live sensor touched.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

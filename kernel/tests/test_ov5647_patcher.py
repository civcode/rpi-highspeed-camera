#!/usr/bin/env python3
"""Offline unit tests for the strictly pinned OV5647 source transformer."""
from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "ov5647" / "prepare-experimental-module.py"
spec = importlib.util.spec_from_file_location("hscam_ov5647_patcher", SCRIPT)
patcher = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(patcher)


def stock_fixture() -> str:
    # Matches stock OV5647 VGA row formatting and contains the mandatory
    # register keys without shipping a copy of Linux's licensed driver file.
    addresses = sorted(patcher.REQUIRED_REGS | {0x0100})
    rows = []
    for reg in addresses:
        value = {
            0x3036: 0x46, 0x3806: 0x07, 0x3807: 0x9F,
            0x380A: 0x01, 0x380B: 0xE0, 0x3814: 0x35,
            0x0100: 0x01,
        }.get(reg, 0)
        if reg == 0x0100:
            continue
        rows.append(f"\t{{0x{reg:04x}, 0x{value:02x}}},")
    rows.append("\t{0x0100, 0x01},")
    vg = "\n".join(rows)
    modes = "\n".join(
        f"\t{{ .reg_list = ov5647_mode_{i}, }}," for i in range(4)
    )
    return (
        "/* source fixture */\n"
        "static struct regval_list ov5647_640x480_10bpp[] = {\n"
        f"{vg}\n"
        "};\n\n"
        "static const struct ov5647_mode ov5647_modes[] = {\n"
        f"{modes}\n"
        "};\n"
        '#define OV5647_DEFAULT_MODE (&ov5647_modes[3])\n'
        'MODULE_LICENSE("GPL v2");\n'
    )


class PatcherTests(unittest.TestCase):
    def test_generates_one_separate_640x240_mode(self):
        stock = stock_fixture()
        result = patcher.patch_driver(stock)
        self.assertEqual(result.count("ov5647_640x240_hscam_10bpp[] ="), 1)
        self.assertIn(".height\t\t= 240", result)
        self.assertIn(".height\t\t= 960", result)
        self.assertIn(".pixel_rate\t= 87500000", result)
        self.assertIn(".vts\t\t= 0x108", result)
        self.assertIn(".link_freq_index = FREQ_INDEX_FULL", result)
        self.assertIn("MODULE_INFO(hscam_experimental", result)
        self.assertEqual(result.count(".reg_list"), 5)
        self.assertIn("#define OV5647_DEFAULT_MODE (&ov5647_modes[3])", result)
        # The stock entry must remain untouched.
        self.assertIn("ov5647_640x480_10bpp[] = {\n", result)
        stock_vga = result.split("ov5647_640x480_10bpp[] = {\n", 1)[1].split("};", 1)[0]
        self.assertIn("{0x380a, 0x01}", stock_vga)
        candidate = result.split("ov5647_640x240_hscam_10bpp[] = {\n", 1)[1].split("};", 1)[0]
        for address, value in patcher.REG_UPDATES.items():
            self.assertIn(f"{{0x{address:04x}, 0x{value:02x}}}", candidate)
        for address, value in patcher.EXTRA_REGS.items():
            self.assertIn(f"{{0x{address:04x}, 0x{value:02x}}}", candidate)
        self.assertLess(
            candidate.index("{0x380f, 0x08}"),
            candidate.index("{0x0100, 0x01}"),
        )

    def test_rejects_reapplication(self):
        result = patcher.patch_driver(stock_fixture())
        with self.assertRaisesRegex(ValueError, "already present"):
            patcher.patch_driver(result)

    def test_rejects_missing_register(self):
        fixture = stock_fixture().replace("\t{0x3036, 0x46},\n", "")
        with self.assertRaisesRegex(ValueError, "Missing expected"):
            patcher.patch_driver(fixture)

    def test_rejects_new_mode_table_layout(self):
        fixture = stock_fixture().replace(".reg_list = ov5647_mode_3", ".regs = ov5647_mode_3")
        with self.assertRaisesRegex(ValueError, "four stock"):
            patcher.patch_driver(fixture)

    def test_kernel_shell_helpers_parse(self):
        # A previous compile-only helper successfully built the module but
        # failed later on a multiline Bash [[ ... == ... ]] expression.
        # These scripts can modify only local staging state and must be
        # syntax-checked on hosts without the Raspberry Pi hardware.
        for name in ("build-experimental-module.sh", "manage-module-override.sh"):
            helper = SCRIPT.parent / name
            result = subprocess.run(
                ["bash", "-n", str(helper)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(
                result.returncode, 0,
                f"{helper} has a Bash syntax error: {result.stderr}",
            )

    def test_refuses_wrong_checksum_without_writing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            source = root / "ov5647.c"
            source.write_text(stock_fixture())
            target = root / "output"
            run = subprocess.run(
                [sys.executable, str(SCRIPT),
                 "--source", str(source), "--output", str(target)],
                capture_output=True, text=True, check=False,
            )
            self.assertNotEqual(run.returncode, 0)
            self.assertIn("SHA256", run.stderr)
            self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()

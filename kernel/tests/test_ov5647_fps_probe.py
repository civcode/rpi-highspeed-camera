#!/usr/bin/env python3
"""Host-only contract tests; never touches Pi hardware or starts a capture."""
from __future__ import annotations

import importlib.util
import pathlib
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "ov5647" / "fps-probe.py"
spec = importlib.util.spec_from_file_location("hscam_fps_probe", SCRIPT)
probe = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(probe)


class FPSProbeTests(unittest.TestCase):
    def test_parse_valid_capture_result(self):
        output = (
            "configured 640x240 SGBRG16, representation=uncompressed, bits=10/16\n"
            "frames=180 gaps=0 overruns=0 fps=59.91\n"
        )
        self.assertEqual(probe.parse_capture_summary(output), (180, 0, 0, 59.91))

    def test_parse_missing_sensor_fps_fails(self):
        with self.assertRaisesRegex(ValueError, "statistics"):
            probe.parse_capture_summary("frames=128 gaps=0 overruns=0\n")

    def test_parse_gap_counts(self):
        self.assertEqual(
            probe.parse_capture_summary("frames=270 gaps=2 overruns=1 fps=89.2\n"),
            (270, 2, 1, 89.2),
        )

    def test_find_exact_pi_sensor_mode(self):
        info = {"cameras": [
            {"model": "ov5647", "sensor_model": "ov5647", "modes": [
                {"id": "stock", "width": 640, "height": 480, "bit_depth": 10},
                {"id": "experimental", "width": 640, "height": 240, "bit_depth": 10},
            ]}
        ]}
        self.assertEqual(probe.get_sensor_mode(info), "experimental")

    def test_refuse_stock_only_modes(self):
        info = {"cameras": [
            {"sensor_model": "ov5647", "modes": [
                {"id": "stock", "width": 640, "height": 480, "bit_depth": 10},
            ]}
        ]}
        with self.assertRaisesRegex(RuntimeError, "not uniquely advertised"):
            probe.get_sensor_mode(info)

    def test_refuse_duplicate_mode(self):
        info = {"cameras": [
            {"sensor_model": "ov5647", "modes": [
                {"id": "one", "width": 640, "height": 240, "bit_depth": 10},
                {"id": "two", "width": 640, "height": 240, "bit_depth": 10},
            ]}
        ]}
        with self.assertRaisesRegex(RuntimeError, "not uniquely advertised"):
            probe.get_sensor_mode(info)


if __name__ == "__main__":
    unittest.main()

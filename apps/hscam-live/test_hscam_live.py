"""Host-side tests for batch settings, independent of camera hardware."""
import unittest
from pathlib import Path

from hscam_live import BatchSettings, SharedSettings, capture_command, export_command


class BatchSettingsTests(unittest.TestCase):
    def setUp(self):
        self.fields = dict(frames="10", frame="5", frame_duration_us="3333", exposure_us="3000", gain="1")

    def test_parse_and_snapshot(self):
        original = BatchSettings.parse(self.fields)
        state = SharedSettings(original)
        in_progress, rev = state.snapshot()
        self.fields.update(frames="8", frame="7", frame_duration_us="4000", exposure_us="3500", gain="2.5")
        next_batch = BatchSettings.parse(self.fields)
        self.assertEqual(state.update(next_batch), 1)
        self.assertEqual((in_progress, rev), (original, 0))
        self.assertEqual(state.snapshot(), (next_batch, 1))

    def test_capture_export_are_consistent_with_snapshot(self):
        settings = BatchSettings.parse(self.fields)
        cmd = list(map(str, capture_command("capture", Path("bundle"), "mode", "640x128", settings)))
        self.assertEqual(cmd[cmd.index("--frames") + 1], "10")
        self.assertEqual(cmd[cmd.index("--gain") + 1], "1.0")
        self.assertEqual(cmd[cmd.index("--frame-duration-us") + 1], "3333")
        self.assertEqual(cmd[cmd.index("--exposure-us") + 1], "3000")
        exp = list(map(str, export_command("export", Path("bundle"), Path("frame.png"), settings)))
        self.assertEqual(exp[exp.index("--frame") + 1], "5")

    def test_invalid_values_do_not_modify_active_settings(self):
        state = SharedSettings(BatchSettings.parse(self.fields))
        for field, value in (("frames", "0"), ("frame", "10"), ("frame", "-1"),
                             ("exposure_us", "0"), ("frame_duration_us", "invalid"),
                             ("gain", "nan"), ("gain", "inf"), ("gain", "-1")):
            fields = {**self.fields, field: value}
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                state.update(BatchSettings.parse(fields))
        self.assertEqual(state.snapshot()[1], 0)


if __name__ == "__main__":
    unittest.main()

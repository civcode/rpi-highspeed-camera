"""Host-side tests for batch settings, independent of camera hardware."""
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from hscam_live import BatchSettings, PngSaver, SharedSettings, capture_command, export_command


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


class PngSaverTests(unittest.TestCase):
    PNG = b"\x89PNG\r\n\x1a\nexample-png-bytes"

    def test_numbers_increase_and_contents_are_unchanged(self):
        with TemporaryDirectory() as directory:
            saver = PngSaver(directory)
            first = saver.save(self.PNG)
            second = saver.save(self.PNG + b"second")
            self.assertEqual(first.name, "hscam-live-000001.png")
            self.assertEqual(second.name, "hscam-live-000002.png")
            self.assertEqual(first.read_bytes(), self.PNG)
            self.assertEqual(second.read_bytes(), self.PNG + b"second")

    def test_restart_resumes_after_highest_existing_number(self):
        with TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "hscam-live-000009.png").write_bytes(self.PNG)
            (output / "hscam-live-readme.png").write_bytes(b"not a snapshot")
            result = PngSaver(output).save(self.PNG)
            self.assertEqual(result.name, "hscam-live-000010.png")
            self.assertEqual((output / "hscam-live-000009.png").read_bytes(), self.PNG)

    def test_racing_new_file_never_overwrites(self):
        with TemporaryDirectory() as directory:
            saver = PngSaver(directory)
            another_process_file = Path(directory) / "hscam-live-000001.png"
            another_process_file.write_bytes(b"original")
            result = saver.save(self.PNG)
            self.assertEqual(result.name, "hscam-live-000002.png")
            self.assertEqual(another_process_file.read_bytes(), b"original")

    def test_invalid_png_does_not_create_a_file(self):
        with TemporaryDirectory() as directory:
            saver = PngSaver(directory)
            with self.assertRaises(ValueError):
                saver.save(b"not a PNG")
            self.assertEqual(list(Path(directory).iterdir()), [])


if __name__ == "__main__":
    unittest.main()

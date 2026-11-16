"""Synthetic, payload-free intake gates. No emulator or retail input required."""
import io
import json
import unittest
import zipfile

import bk2_intake as intake


def movie(rows, *, extra=None, sync=None, key=None, core="Octoshock"):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        archive.writestr("Header.txt", f"Platform PSX\nCore {core}\nemuVersion Version 2.2.2\n")
        archive.writestr("SyncSettings.json", json.dumps(intake.SYNC if sync is None else sync))
        archive.writestr("Input Log.txt", "\n".join([
            "[Input]", intake.LOG_KEY if key is None else key, *rows, "[/Input]"]))
        for name, data in (extra or {}).items():
            archive.writestr(name, data)
    return output.getvalue()


def row(buttons=".............."):
    return "|    1,...|" + buttons + "|"


class IntakeTests(unittest.TestCase):
    def test_neutral_and_multiple_buttons(self):
        route, receipt = intake.inspect(movie([row(), row("U........X...."), row()]))
        self.assertEqual([s["buttons"] for s in route["segments"]], [65535, 49135, 65535])
        self.assertTrue(receipt["roundtrip_equal"])

    def test_each_button_against_psx_wire_word(self):
        # Independent expected words in the source's 14-button order.
        expected = [0xFFEF, 0xFFBF, 0xFF7F, 0xFFDF, 0xFFFE, 0xFFF7, 0x7FFF,
                    0xEFFF, 0xDFFF, 0xBFFF, 0xFBFF, 0xF7FF, 0xFEFF, 0xFDFF]
        rows = [row("." * i + "X" + "." * (13-i)) for i in range(14)]
        route, _ = intake.inspect(movie(rows))
        self.assertEqual([s["buttons"] for s in route["segments"]], expected)

    def test_run_lengths_retain_neutral_frames(self):
        route, receipt = intake.inspect(movie([row(), row(), row("U............."), row()]))
        self.assertEqual([(s["source_frame"], s["frames"]) for s in route["segments"]], [(0, 2), (2, 1), (3, 1)])
        self.assertEqual(receipt["frame_count"], 4)
        self.assertEqual(route["qualification"], "structural_only_playback_not_run")

    def test_site_wrapper(self):
        direct = movie([row()])
        wrapper = io.BytesIO()
        with zipfile.ZipFile(wrapper, "w") as archive:
            archive.writestr("example.bk2", direct)
        self.assertEqual(intake.inspect(wrapper.getvalue())[1]["movie_sha256"], intake.sha(direct))

    def test_reject_embedded_state_even_without_header_flag(self):
        with self.assertRaisesRegex(ValueError, "embedded state"):
            intake.inspect(movie([row()], extra={"Core.bin": b"synthetic"}))

    def test_reject_save_ram(self):
        with self.assertRaisesRegex(ValueError, "embedded state"):
            intake.inspect(movie([row()], extra={"SaveRam.bin": b"synthetic"}))

    def test_reject_analog_or_second_pad(self):
        for devices in ([2, 0, 0, 0, 0, 0, 0, 0], [1, 0, 0, 0, 1, 0, 0, 0]):
            sync = json.loads(json.dumps(intake.SYNC))
            sync["o"]["FIOConfig"]["Devices8"] = devices
            with self.assertRaisesRegex(ValueError, "configuration"):
                intake.inspect(movie([row()], sync=sync))

    def test_reject_reset_tray_and_disc_change(self):
        for changed in ("|    1,..R|", "|    1,O..|", "|    2,...|"):
            with self.assertRaisesRegex(ValueError, "disc/reset/tray"):
                intake.inspect(movie([changed + "..............|"]))

    def test_reject_unknown_core_and_layout(self):
        for kwargs in ({"core": "Nymashock"}, {"key": intake.LOG_KEY.replace("P1 Up", "P1 Unknown")}):
            with self.assertRaises(ValueError):
                intake.inspect(movie([row()], **kwargs))

    def test_reject_extra_columns_or_lost_rows(self):
        for malformed in (row() + "|", row()[:-2] + "|", "unrecognized event"):
            with self.assertRaises(ValueError):
                intake.inspect(movie([malformed]))


if __name__ == "__main__":
    unittest.main()

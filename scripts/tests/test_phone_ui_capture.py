"""Real snapshot transport must reject evidence gaps and corruption."""
import base64
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location(
    "phone_ui_capture", Path(__file__).resolve().parents[2] / "tools/phone_ui_capture.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PhoneUiCaptureTests(unittest.TestCase):
    def snapshot(self, data=b"\x00\xf8\xe0\x07"):
        checksum = MODULE.fnv1a(data)
        return MODULE.Snapshot(f"UI_SNAPSHOT_BEGIN width=2 height=1 stride=4 bytes=4 fnv={checksum:08x}")

    def test_exact_frame_with_checksum(self):
        shot = self.snapshot()
        shot.chunk("UI_PIXELS 0 APjgBw==")
        self.assertEqual(shot.finish(f"UI_SNAPSHOT_END fnv={shot.checksum:08x}"), b"\x00\xf8\xe0\x07")

    def test_gap_and_duplicate_rejected(self):
        for offset in (1, 192):
            with self.assertRaises(ValueError):
                self.snapshot().chunk(f"UI_PIXELS {offset} AAA=")
        shot = self.snapshot()
        shot.chunk("UI_PIXELS 0 AAA=")
        with self.assertRaises(ValueError):
            shot.chunk("UI_PIXELS 0 AAA=")

    def test_truncated_corrupt_and_wrong_footer_rejected(self):
        for data in (b"\x00\xf8", b"\x00\xf8\xe1\x07"):
            shot = self.snapshot()
            shot.chunk("UI_PIXELS 0 " + base64.b64encode(data).decode())
            with self.assertRaises(ValueError):
                shot.finish(f"UI_SNAPSHOT_END fnv={shot.checksum:08x}")
        shot = self.snapshot()
        shot.chunk("UI_PIXELS 0 APjgBw==")
        with self.assertRaises(ValueError):
            shot.finish("UI_SNAPSHOT_END fnv=00000000")

    def test_unbounded_or_inconsistent_dimensions_rejected(self):
        for dims in ("width=480 height=320 stride=960 bytes=3", "width=0 height=1 stride=4 bytes=4",
                     "width=5000 height=1 stride=10000 bytes=10000"):
            with self.assertRaises(ValueError):
                MODULE.Snapshot(f"UI_SNAPSHOT_BEGIN {dims} fnv=00000000")

    def test_chunk_overrun_rejected(self):
        with self.assertRaises(ValueError):
            self.snapshot().chunk("UI_PIXELS 0 AAAAAAAA")

    def test_short_chunks_and_single_character_transport_loss(self):
        data = bytes(range(256)) + bytes(range(64))
        checksum = MODULE.fnv1a(data)
        header = f"UI_SNAPSHOT_BEGIN width=80 height=2 stride=160 bytes=320 fnv={checksum:08x}"
        shot = MODULE.Snapshot(header)
        for offset in range(0, len(data), 96):
            encoded = base64.b64encode(data[offset:offset + 96]).decode()
            shot.chunk(f"UI_PIXELS {offset} {encoded}")
        self.assertEqual(shot.finish(f"UI_SNAPSHOT_END fnv={checksum:08x}"), data)
        # The hardware failure lost one character from an otherwise valid line.
        # Never pad/repair that evidence into a passing screenshot.
        encoded = base64.b64encode(data[:96]).decode()
        with self.assertRaises(ValueError):
            MODULE.Snapshot(header).chunk("UI_PIXELS 0 " + encoded[:-1])


if __name__ == "__main__":
    unittest.main()

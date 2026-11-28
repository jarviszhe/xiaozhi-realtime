"""Offline settings must cover every rendered character before networking."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import re

ROOT = Path(__file__).resolve().parents[2]
FONTS = ROOT / "managed_components/78__xiaozhi-fonts"
SOURCE = ROOT / "main/boards/waveshare/esp32-s3-touch-lcd-3.5b/phone_settings_view.cc"
SPEC = importlib.util.spec_from_file_location("generate_phone_ui_font", ROOT / "scripts/generate_phone_ui_font.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


@unittest.skipUnless(FONTS.is_dir(), "requires pinned font component")
class PhoneUiFontTests(unittest.TestCase):
    def test_all_real_ui_strings_have_offline_glyphs(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "font.cc"
            MODULE.generate(FONTS, SOURCE, output)
            source = output.read_text(encoding="utf8")
            self.assertIn("phone_settings_font", source)
            self.assertLessEqual(int(re.search(r"\.line_height=(\d+)", source)[1]), 26)
            self.assertIn(".bpp=4", source)

    def test_missing_glyph_fails_build_instead_of_dropping_text(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "view.cc"
            source.write_text('const char* text = "设置\U0010ffff";', encoding="utf8")
            with self.assertRaisesRegex(ValueError, "missing codepoints"):
                MODULE.generate(FONTS, source, root / "font.cc")
            self.assertFalse((root / "font.cc").exists())


if __name__ == "__main__":
    unittest.main()

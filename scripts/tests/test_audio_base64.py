"""Check the audio fast path against Python's Base64 decoder and buffer bounds."""

import base64
import ctypes
import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AudioBase64Tests(unittest.TestCase):
    def test_audio_samples_padding_invalid_input_and_output_bounds(self):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        if not compiler:
            self.skipTest("requires a host compiler")
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            source = folder / "decode.cc"
            source.write_text('''
                #include "audio_base64.h"
                #ifdef _WIN32
                #define EXPORT __declspec(dllexport)
                #else
                #define EXPORT
                #endif
                extern "C" EXPORT ptrdiff_t decode(const char* input, size_t length,
                                                   uint8_t* output, size_t capacity) {
                    size_t written = 0;
                    return realtime_audio::DecodeBase64(input, length, output, capacity, written)
                               ? static_cast<ptrdiff_t>(written) : -1;
                }
            ''', encoding="utf-8")
            library = folder / ("decode.dll" if os.name == "nt" else "decode.so")
            subprocess.run([compiler, "-std=c++17", "-O2", "-shared", "-fPIC",
                            "-I", str(ROOT / "main/realtime"), str(source), "-o", str(library)],
                           check=True, capture_output=True)
            loaded = ctypes.CDLL(str(library))
            decode = loaded.decode
            decode.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
            decode.restype = ctypes.c_ssize_t
            try:
                rng = random.Random(8311)
                for size in list(range(1025)) + [15360, 24000, 65536, 96000]:
                    original = rng.randbytes(size)
                    encoded = base64.b64encode(original)
                    output = ctypes.create_string_buffer(b"\xa5" * (size + 16))
                    self.assertEqual(decode(encoded, len(encoded), output, size), size)
                    self.assertEqual(output.raw[:size], original)
                    self.assertEqual(output.raw[size:size + 16], b"\xa5" * 16)
                    if size:
                        output.raw = b"\xa5" * len(output)
                        self.assertEqual(decode(encoded, len(encoded), output, size - 1), -1)
                        self.assertEqual(output.raw[:size + 16], b"\xa5" * (size + 16))
                for encoded in (b"A", b"AAA", b"====", b"A===", b"AA=A", b"AA==AAAA",
                                b"!!!!", b"AA\xffA", b"AA\nA=", b" AAA=", b"AAA=\r\n"):
                    output = ctypes.create_string_buffer(b"\xa5" * 48)
                    self.assertEqual(decode(encoded, len(encoded), output, 32), -1)
                    self.assertEqual(output.raw[32:48], b"\xa5" * 16)
                self.assertEqual(decode(None, 0, None, 0), 0)
                self.assertEqual(decode(None, 4, None, 0), -1)
            finally:
                if os.name == "nt":
                    import _ctypes
                    _ctypes.FreeLibrary(loaded._handle)


if __name__ == "__main__":
    unittest.main()

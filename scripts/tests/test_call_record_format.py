"""Compile production WAV, timeline, and durable-file helpers against a host filesystem."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CXX = os.environ.get("PHONE_AI_CXX") or shutil.which("c++") or shutil.which("g++")


@unittest.skipUnless(CXX, "requires a native C++ compiler (PHONE_AI_CXX)")
class CallRecordFormatTests(unittest.TestCase):
    def test_wav_bounds_reject_corruption_and_recoverable_replace(self):
        source = r'''
#include "call_record_format.h"
#include <cassert>
#include <string>
#include <fstream>
#include <iterator>

void write(const char* p, const char* s) { std::ofstream f(p); f << s; }
std::string read(const char* p) {
    std::ifstream f(p); return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
int main() {
    using namespace call_record;
    assert(ValidId("00000001-1234abcd"));
    for (const char* bad : {"../secret", "00000001-1234abc/", "00000001-1234abcz", "00000001-1234abcde"})
        assert(!ValidId(bad));
    uint8_t header[44];
    assert(WavHeader(header, 16000 * 300));
    assert(ValidWav(header, 44 + 16000 * 300 * 2));
    assert(!ValidWav(header, 43 + 16000 * 300 * 2));
    header[22] = 2; assert(!ValidWav(header, 44 + 16000 * 300 * 2));
    assert(WavHeader(header, kMaxSamples));
    assert(!WavHeader(header, kMaxSamples + 1));
    assert(FramePosition(1600, 180) == 1600); // coex jitter does not erase frames
    assert(FramePosition(1600, 400) == 6400); // sustained absence gets explicit silence
    MediaClock clock;
    assert(clock.Position(1, 7, 0xfffffffe, 100) == 1600);
    assert(clock.Position(1, 7, 0xffffffff, 400) == 1920); // delayed consecutive packet
    assert(clock.Position(1, 7, 0, 400) == 2240); // sequence wrap
    assert(clock.Position(1, 7, 2, 400) == 2880); // one lost packet remains a hole
    assert(clock.Position(1, 8, 0, 500) == 8000); // new media generation
    assert(clock.Position(2, 8, 1, 20) == 320); // independent next recording
    assert(Mix(30000, 30000) == 32767);
    assert(Mix(-30000, -30000) == -32768);
    const char* chinese = "\xe4\xb8\xad\xe6\x96\x87";
    assert(Utf8Prefix(chinese, 4) == 3 && Utf8Prefix(chinese, 6) == 6);
    write("oversize", "123456789\n");
    FILE* long_line = fopen("oversize", "rb");
    char small[5]; bool invalid = false;
    assert(!ReadLine(long_line, small, sizeof(small), invalid) && invalid);
    fclose(long_line);
    write("meta", "first"); write("new", "second");
    assert(CommitFile("new", "meta"));
    assert(read("meta") == "second" && read("meta.bak") == "first");
    // Failed final rename restores the last committed metadata.
    assert(!CommitFile("does-not-exist", "meta"));
    assert(read("meta") == "second");
    write("new", "third"); assert(CommitFile("new", "meta"));
    assert(read("meta") == "third" && read("meta.bak") == "second");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            src, exe = path / "record.cc", path / "record.exe"
            src.write_text(source, encoding="utf8")
            compiler = [CXX]
            if Path(CXX).name.lower() in ("zig", "zig.exe"):
                compiler.append("c++")
            result = subprocess.run(compiler + ["-std=c++17", "-UNDEBUG", "-Wall", "-Wextra",
                "-Werror", "-I", str(ROOT / "main"), str(src), "-o", str(exe)],
                capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run([str(exe)], cwd=path, check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()

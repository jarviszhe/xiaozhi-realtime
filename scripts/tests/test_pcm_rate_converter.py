"""Exercise interleaved conversion, reported capacity and failure boundaries."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CXX = os.environ.get("PHONE_AI_CXX") or shutil.which("c++") or shutil.which("g++")


@unittest.skipUnless(CXX, "requires a native C++ compiler (PHONE_AI_CXX)")
class PcmRateConverterTests(unittest.TestCase):
    def test_interleaved_channels_and_failed_or_out_of_bounds_conversion(self):
        code = r'''
#include "pcm_rate_converter.h"
#include <cassert>
int mode = 0, calls = 0;
unsigned channels = 1;
esp_ae_err_t esp_ae_rate_cvt_get_max_out_sample_num(void*, uint32_t, uint32_t* size) {
    *size = mode == 2 ? UINT32_MAX : mode == 6 ? 288 : 321;
    return mode == 3 ? -1 : 0;
}
esp_ae_err_t esp_ae_rate_cvt_process(void*, const int16_t*, uint32_t, int16_t* out,
                                   uint32_t* size) {
    ++calls;
    assert(*size == (mode == 6 ? 288 : 321)); // Supply the advertised capacity.
    if (mode == 1) return -1;
    if (mode == 4) { ++*size; return 0; }
    if (mode == 6) { // Replay the real mixed-frame failure with a diagnostic guard.
        for (unsigned i = 0; i < 289 * channels; ++i) out[i] = int16_t(i + 1);
        *size = 289;
        return 0;
    }
    for (unsigned i = 0; i < 321 * channels; ++i) out[i] = int16_t(i + 1);
    if (mode == 5) out[*size * channels] = 123; // corrupt diagnostic guard
    *size = 321;
    return 0;
}
int main() {
    void* handle = reinterpret_cast<void*>(1);
    for (channels = 1; channels <= 2; ++channels) {
        std::vector<int16_t> input(240 * channels), output;
        mode = 0;
        assert(ConvertPcmRate(handle, input, channels, output));
        assert(output.size() == 321 * channels && output.back() == int16_t(321 * channels));
        assert(ConvertPcmRate(handle, input, channels, input)); // in-place caller
        assert(input.size() == 321 * channels);
        input.resize(240 * channels);
        for (mode = 1; mode <= 6; ++mode) {
            output.assign(7, 7);
            int previous = calls;
            assert(!ConvertPcmRate(handle, input, channels, output));
            assert(output.empty());
            if (mode == 2 || mode == 3) assert(calls == previous);
        }
        int previous = calls;
        assert(!ConvertPcmRate(nullptr, input, channels, output));
        assert(!ConvertPcmRate(handle, input, 0, output));
        assert(calls == previous);
    }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "esp_log.h").write_text("#define ESP_LOGE(...) ((void)0)\n", encoding="utf8")
            (path / "esp_ae_rate_cvt.h").write_text(
                "#pragma once\n#include <cstdint>\n"
                "using esp_ae_rate_cvt_handle_t = void*;\n"
                "using esp_ae_sample_t = int16_t*;\nusing esp_ae_err_t = int;\n"
                "constexpr int ESP_AE_ERR_OK = 0;\n"
                "int esp_ae_rate_cvt_get_max_out_sample_num(void*, uint32_t, uint32_t*);\n"
                "int esp_ae_rate_cvt_process(void*, const int16_t*, uint32_t, int16_t*, uint32_t*);\n",
                encoding="utf8")
            source, exe = path / "test.cc", path / "test.exe"
            source.write_text(code, encoding="utf8")
            command = [CXX]
            if Path(CXX).name.lower() in ("zig", "zig.exe"):
                command.append("c++")
            build = subprocess.run(command + ["-std=c++17", "-O2", "-UNDEBUG", "-Wall", "-Wextra",
                "-Werror", "-DCONFIG_HEAP_POISONING_LIGHT=1", "-I", str(path),
                "-I", str(ROOT / "main/audio"), str(source), "-o", str(exe)],
                capture_output=True, text=True, timeout=180)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=10)

"""Exercise the exact bounded PCM buffer and streaming FIR used by phone AI."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CXX = os.environ.get("PHONE_AI_CXX") or shutil.which("c++") or shutil.which("g++")


@unittest.skipUnless(CXX, "requires a native C++ compiler (PHONE_AI_CXX)")
class PhoneAiAudioTests(unittest.TestCase):
    def test_streaming_filter_alias_rejection_packet_boundaries_and_queue_reset(self):
        code = r'''
#include "phone_ai_audio.h"
#include <cassert>
#include <cstdio>
#include <vector>
double tone(unsigned source, unsigned target, double hz) {
    PhoneRateConverter cvt;
    assert(cvt.Configure(source, target));
    int16_t in[480], out[480]; double power=0, discontinuity=0; size_t n=0;
    double previous=0;
    for (unsigned frame=0; frame<80; ++frame) {
        for (unsigned i=0; i<source/50; ++i)
            in[i]=int16_t(10000*std::sin(2*3.141592653589793*hz*(frame*(source/50)+i)/source));
        assert(cvt.Process(in,source/50,out,480));
        if (frame>2) for (unsigned i=0;i<target/50;++i) {
            power+=double(out[i])*out[i]; ++n;
            if (i==0) discontinuity=std::max(discontinuity,std::abs(double(out[i])-previous));
            previous=out[i];
        }
    }
    if (hz==1000) assert(discontinuity < 8500); // no state reset at packet seams
    cvt.Reset(); std::fill(std::begin(in),std::end(in),0);
    assert(cvt.Process(in,source/50,out,480));
    for (unsigned i=0;i<target/50;++i) assert(out[i]==0); // no old-call tail
    return std::sqrt(power/n);
}
int main() {
    int16_t storage[7], output[9]; PhonePcmRing queue; queue.Attach(storage,7);
    const int16_t a[]={1,2,3,4,5}, b[]={6,7,8,9,10};
    assert(queue.Push(a,5)); assert(!queue.Push(b,5)); assert(queue.size()==5);
    assert(queue.Pop(output,3)==3 && output[0]==1 && output[2]==3);
    assert(queue.Push(b,5)); assert(queue.Pop(output,9)==7);
    const int16_t expected[]={4,5,6,7,8,9,10,0,0};
    assert(std::equal(std::begin(output),std::end(output),std::begin(expected)));
    assert(queue.Push(a,5)); queue.Clear(); assert(queue.Pop(output,9)==0);
    for (int16_t sample:output) assert(sample==0);
    const double pass8=tone(24000,8000,1000), alias8=tone(24000,8000,7000);
    const double pass16=tone(24000,16000,1000), alias16=tone(24000,16000,11000);
    assert(pass8>6500 && pass16>6500);
    assert(alias8<pass8*.02 && alias16<pass16*.02);
    assert(tone(8000,16000,1000)>6500);
    printf("PASS rms pass8=%.1f alias8=%.1f pass16=%.1f alias16=%.1f\n",pass8,alias8,pass16,alias16);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            source, exe = path / "test.cc", path / "test.exe"
            source.write_text(code, encoding="utf8")
            command = [CXX]
            if Path(CXX).name.lower() in ("zig", "zig.exe"):
                command += ["c++"]
            build = subprocess.run(command + ["-std=c++17", "-O2", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "main"), str(source), "-o", str(exe)],
                capture_output=True, text=True, timeout=180)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn("PASS rms", result.stdout)


if __name__ == "__main__":
    unittest.main()

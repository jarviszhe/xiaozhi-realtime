"""Run the actual firmware C helpers using a native host compiler."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CC = os.environ.get("PHONE_DOWNLINK_CC") or shutil.which("cc") or shutil.which("gcc")


@unittest.skipUnless(CC, "requires a native C compiler (PHONE_DOWNLINK_CC)")
class PhoneDownlinkTests(unittest.TestCase):
    def test_independent_pcm_and_adpcm_vectors_and_malformed_payloads(self):
        vectors = json.loads((ROOT / "scripts/tests/fixtures/phone-adpcm-v1.json").read_text(encoding="utf8"))
        source = r'''
#include "phone_downlink.h"
#include <stdio.h>
#define CHECK(v) do { if (!(v)) { fprintf(stderr,"check failed line %d\n",__LINE__); return 1; } } while (0)
'''
        for i, v in enumerate(vectors):
            for key in ("payload_hex", "decoded_pcm_hex"):
                values = ",".join(str(b) for b in bytes.fromhex(v[key]))
                source += f"static const uint8_t {key}_{i}[] = {{{values}}};\n"
        source += "int main(void) { uint8_t output[642], bad[164];\n"
        for i, v in enumerate(vectors):
            samples = v["fields"]["rate"] // 50
            payload = f"payload_hex_{i}"
            expected = f"decoded_pcm_hex_{i}"
            source += f'''
    memset(output, 0x5a, sizeof(output));
    CHECK(phone_downlink_decode(2,{payload},sizeof({payload}),{samples},output+1,{samples*2}));
    CHECK(!memcmp(output+1,{expected},sizeof({expected})));
    CHECK(output[0]==0x5a && output[{samples*2+1}]==0x5a);
    CHECK(phone_downlink_decode(1,{expected},sizeof({expected}),{samples},output+1,{samples*2}));
    CHECK(!memcmp(output+1,{expected},sizeof({expected})));
    CHECK(!phone_downlink_decode(2,{payload},sizeof({payload})-1,{samples},output,sizeof(output)));
    CHECK(!phone_downlink_decode(2,{payload},sizeof({payload}),{samples},output,{samples*2-1}));
    CHECK(!phone_downlink_decode(3,{payload},sizeof({payload}),{samples},output,sizeof(output)));
    memcpy(bad,{payload},sizeof({payload})); bad[2]=89;
    memset(output,0x5a,sizeof(output));
    CHECK(!phone_downlink_decode(2,bad,sizeof({payload}),{samples},output,sizeof(output)));
    for (unsigned j=0;j<sizeof(output);++j) CHECK(output[j]==0x5a);
    memcpy(bad,{payload},sizeof({payload})); bad[3]=1;
    CHECK(!phone_downlink_payload_valid(2,bad,sizeof({payload}),{samples}));
    memcpy(bad,{payload},sizeof({payload})); bad[sizeof({payload})-1]|=0xf0;
    CHECK(!phone_downlink_payload_valid(2,bad,sizeof({payload}),{samples}));
'''
        source += "CHECK(!phone_downlink_payload_valid(1,output,320,159)); return 0; }\n"
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder/"test.c").write_text(source, encoding="utf8")
            compiler = [CC] + (["cc"] if Path(CC).name.lower() in ("zig", "zig.exe") else [])
            executable = folder / ("test.exe" if os.name == "nt" else "test")
            build = subprocess.run(compiler + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                                   "-I", str(ROOT/"main"), str(folder/"test.c"), "-o", str(executable)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout+build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_wrap_reordering_large_gaps_and_signed_fade(self):
        source = r'''
#include "phone_downlink.h"
int gap(uint32_t expected, uint32_t sequence) {
    bool late, rebuffer;
    uint32_t missing = phone_downlink_missing(expected, sequence, &late, &rebuffer);
    return late ? -1 : rebuffer ? -2 : (int)missing;
}
int fade(int sample, uint32_t index) {
    int16_t previous = (int16_t)sample, output = 0;
    return phone_downlink_conceal(&previous, &output, 1, index) ? output : 999999;
}
int invalid(void) {
    int16_t previous = 1, output = 0;
    return phone_downlink_conceal(0, &output, 1, 1) ||
           phone_downlink_conceal(&previous, 0, 1, 1) ||
           phone_downlink_conceal(&previous, &output, 0, 1) ||
           phone_downlink_conceal(&previous, &output, 321, 1);
}
int boot_check(void) { return phone_downlink_selftest(); }
'''
        samples = [-32768, -32767, -12345, -1, 0, 1, 12345, 32767]
        gaps = [(5, 5), (5, 6), (5, 10), (5, 11), (5, 4),
                (0xFFFFFFFF, 0), (0, 0xFFFFFFFF), (0xFFFFFFFE, 2)]
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / "test.c").write_text(source, encoding="utf8")
            compiler = [CC]
            if Path(CC).name.lower() in ("zig", "zig.exe"):
                compiler += ["cc"]
            executable = folder / ("test.exe" if os.name == "nt" else "test")
            # A native executable prints results for independently checked vectors.
            native = source + r'''
#include <stdio.h>
int main(void) {
    const uint32_t a[]={5,5,5,5,5,UINT32_MAX,0,UINT32_MAX-1};
    const uint32_t b[]={5,6,10,11,4,0,UINT32_MAX,2};
    const int samples[]={-32768,-32767,-12345,-1,0,1,12345,32767};
    printf("{\"gaps\":[");
    for (int i=0;i<8;++i) printf("%s%d",i?",":"",gap(a[i],b[i]));
    printf("],\"fade\":[");
    for (int i=0;i<8;++i) {
        printf("%s[",i?",":"");
        for (int j=1;j<=5;++j) printf("%s%d",j>1?",":"",fade(samples[i],j));
        printf("]");
    }
    printf("],\"bounds\":[%d,%d,%d],\"boot\":%d}",fade(123,0),fade(123,6),invalid(),boot_check());
    return 0;
}
'''
            (folder / "test.c").write_text(native, encoding="utf8")
            build = subprocess.run(compiler + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                                   "-I", str(ROOT / "main"), str(folder / "test.c"),
                                   "-o", str(executable)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
        got = json.loads(result.stdout)
        self.assertEqual(got["gaps"], [0, 1, 5, -2, -1, 1, -1, 4])
        self.assertEqual(got["fade"], [[int(sample * (6-index) / 6) for index in range(1, 6)]
                                       for sample in samples])
        self.assertEqual(got["bounds"], [999999, 999999, 0])
        self.assertEqual(got["boot"], 1)


if __name__ == "__main__":
    unittest.main()

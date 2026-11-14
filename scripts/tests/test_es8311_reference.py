"""Check mono playback when ES8311 captures ADC/DAC in two I2S slots."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Es8311ReferenceTests(unittest.TestCase):
    def test_stereo_capture_preserves_mono_playback_samples_and_duration(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("requires a host C++17 compiler")
        source = (ROOT / "main/audio/codecs/es8311_audio_codec.cc").read_text(encoding="utf-8")
        start = source.index("int Es8311AudioCodec::Write(")
        brace = source.index("{", start)
        end, depth = brace + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        driver = r'''
            #include <array>
            #include <cassert>
            #include <cstdint>
            #include <vector>
            #define ESP_ERROR_CHECK_WITHOUT_ABORT(x) assert((x)==0)
            std::vector<int16_t> sent;int writes=0;
            int esp_codec_dev_write(int,const void* data,int bytes){
                auto pcm=static_cast<const int16_t*>(data);
                sent.assign(pcm,pcm+bytes/2);++writes;return 0;}
            class Es8311AudioCodec {
            public:
                bool output_enabled_=true,input_reference_=false;
                int dev_=1;std::vector<int16_t> stereo_output_;
                int Write(const int16_t*,int);
            };
        ''' + source[start:end] + r'''
            int main(){
                Es8311AudioCodec codec;std::array<int16_t,480> pcm;
                for(int i=0;i<480;++i)pcm[i]=i%2?-32768:32767;
                assert(codec.Write(pcm.data(),480)==480);
                assert(sent==std::vector<int16_t>(pcm.begin(),pcm.end()));
                codec.input_reference_=true;codec.stereo_output_.reserve(960);
                auto* storage=codec.stereo_output_.data();
                assert(codec.Write(pcm.data(),480)==480);
                assert(sent.size()==960 && codec.stereo_output_.data()==storage);
                for(int i=0;i<480;++i){assert(sent[2*i]==pcm[i]);assert(sent[2*i+1]==pcm[i]);}
                // A short last chunk still occupies exactly its original number of I2S frames.
                assert(codec.Write(pcm.data(),113)==113 && sent.size()==226);
                for(int i=0;i<113;++i){assert(sent[2*i]==pcm[i]&&sent[2*i+1]==pcm[i]);}
                codec.output_enabled_=false;
                assert(codec.Write(pcm.data(),480)==480 && writes==3);
            }
        '''
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / "codec.cc").write_text(driver, encoding="utf-8")
            binary = path / ("codec.exe" if os.name == "nt" else "codec")
            result = subprocess.run(compiler + ["-std=c++17", str(path / "codec.cc"), "-o", str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

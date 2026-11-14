"""Exercise the real Realtime protocol with deterministic cloud event ordering."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AliyunRealtimeTests(unittest.TestCase):
    def test_diagnostic_pcm_batching_preserves_samples_and_discontinuities(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("requires a host C++17 compiler")
        source = (ROOT / "main/realtime/debug_trace.cc").read_text(encoding="utf-8")
        start = source.index("void DebugTrace::AecPcm(")
        end = source.index("void DebugTrace::Queue(", start)
        driver = r'''
            #include <algorithm>
            #include <array>
            #include <atomic>
            #include <cassert>
            #include <cstdint>
            #include <cstdlib>
            #include <cstring>
            #include <mutex>
            #include <memory>
            #include <vector>
            struct Captured {uint16_t kind;std::vector<int16_t> pcm;int64_t time;uint64_t index;};
            class DebugTrace {
            public:
                bool enabled_=true;int level_=2;std::mutex aec_mutex_;std::atomic<bool> capture_aec_{true};
                struct AecBatch {std::array<int16_t,1024> data{};size_t samples=0;
                    int64_t start_us=0;uint64_t index=0;};
                std::unique_ptr<std::array<AecBatch,3>> aec_batches_=std::make_unique<std::array<AecBatch,3>>();
                std::vector<Captured> frames;
                void AecPcm(const char*,const int16_t*,size_t,int64_t,uint64_t);
                void FlushAecBatch(size_t);
                void Queue(uint16_t kind,const char*,const char*,const uint8_t* data,size_t length,
                           int,int,int,size_t,const char*,int64_t time,uint64_t index){
                    auto pcm=reinterpret_cast<const int16_t*>(data);
                    frames.push_back({kind,{pcm,pcm+length/2},time,index});
                }
            };
        ''' + source[start:end] + r'''
            int main(){
                DebugTrace d;std::vector<int16_t> input(1600);
                for(size_t i=0;i<input.size();++i)input[i]=i;
                for(size_t i=0;i<input.size();i+=160){
                    d.AecPcm("aec_mic",input.data()+i,160,1000000+i*1000000/16000,i);
                    d.AecPcm("aec_ref",input.data()+i,160,1000000+i*1000000/16000,i);
                }
                d.FlushAecBatch(0);d.FlushAecBatch(1);
                for(int track=0;track<2;++track){
                    std::vector<int16_t> got;
                    for(const auto& f:d.frames)if(f.kind==11+track){
                        assert(f.index==got.size());
                        assert(f.time==1000000+f.index*1000000/16000);
                        got.insert(got.end(),f.pcm.begin(),f.pcm.end());
                    }
                    assert(got==input);
                }
                d.frames.clear();
                d.AecPcm("aec_out",input.data(),160,1000000,0);
                d.AecPcm("aec_out",input.data(),160,2000000,0); // new capture
                assert(d.frames.size()==1 && d.frames[0].time==1000000);
                d.FlushAecBatch(2);assert(d.frames.size()==2 && d.frames[1].time==2000000);
                d.AecPcm("aec_out",input.data(),160,0,0); // invalid reset timestamp
                d.FlushAecBatch(2);assert(d.frames.size()==2);
            }
        '''
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / "batch.cc").write_text(driver, encoding="utf-8")
            binary = path / ("batch.exe" if os.name == "nt" else "batch")
            result = subprocess.run(compiler + ["-std=c++17", str(path / "batch.cc"), "-o", str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_reference_ring_restarts_index_after_long_silence(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("requires a host C++17 compiler")
        source = (ROOT / "main/audio/engines/afe_audio_engine.cc").read_text(encoding="utf-8")
        start = source.index("void AfeAudioEngine::FeedPlaybackReference(")
        brace = source.index("{", start)
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        driver = r'''
            #include <algorithm>
            #include <array>
            #include <atomic>
            #include <cassert>
            #include <cmath>
            #include <cstdio>
            #include <mutex>
            #include <vector>
            #include "playback_timeline.h"
            #define CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC 1
            constexpr int ESP_AE_ERR_OK=0;
            using esp_ae_sample_t=void*;
            int esp_ae_rate_cvt_get_max_out_sample_num(int,size_t n,uint32_t* out){*out=n*2/3;return 0;}
            int esp_ae_rate_cvt_process(int,void*,size_t,void* out,uint32_t* n){
                std::fill_n(static_cast<int16_t*>(out),*n,123);return 0;}
            struct DebugTrace {static DebugTrace& Instance(){static DebugTrace d;return d;}
                void Event(const char*,const char*){}};
            class AfeAudioEngine {
            public:
                static constexpr size_t kReferenceCapacity=16000;
                bool software_reference_ready_=true,reference_nonzero_logged_=false;
                std::mutex reference_mutex_;std::atomic<int> reference_delay_ms_{0};
                int reference_resampler_=0;
                std::array<int16_t,kReferenceCapacity> reference_ring_{};
                std::vector<int16_t> reference_resampled_;
                int64_t playback_reference_start_us_=0,reference_start_us_=0;
                uint64_t playback_reference_samples_=0,reference_written_=0;
                void FeedPlaybackReference(const int16_t*,size_t,int,int64_t);
            };
        ''' + source[start:end] + r'''
            int main(){
                AfeAudioEngine e;std::array<int16_t,480> pcm{};
                e.FeedPlaybackReference(pcm.data(),pcm.size(),24000,1000000);
                assert(e.reference_written_==320 && e.reference_start_us_==1000000);
                e.FeedPlaybackReference(pcm.data(),pcm.size(),24000,8000000);
                assert(e.reference_start_us_==8000000);
                assert(e.reference_written_==320); // do not retain the old coordinate after reset
                assert(e.reference_ring_[0]==123 && e.reference_ring_[319]==123);
                e.FeedPlaybackReference(pcm.data(),pcm.size(),24000,8020000);
                assert(e.reference_written_==640);
            }
        '''
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / "reference.cc").write_text(driver, encoding="utf-8")
            binary = path / ("reference.exe" if os.name == "nt" else "reference")
            result = subprocess.run(compiler + ["-std=c++17", "-I", str(ROOT / "main/audio"),
                                    str(path / "reference.cc"), "-o", str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_playback_reference_preserves_silence_between_replies(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("requires a host C++17 compiler")
        driver = r'''
            #include <cassert>
            #include "playback_timeline.h"
            int main(){
                int64_t origin=0;uint64_t samples=0;
                auto next=[&](int64_t observed){return AdvancePlaybackTimeline(observed,origin,samples,480,24000);};
                assert(next(1000000)==1000000);
                assert(next(1001000)==1020000); // DMA prefill is not a sample gap
                assert(next(1023000)==1040000);
                assert(next(3000000)==3000000); // second reply after silence
                assert(next(3006000)==3020000);
                assert(next(5000000)==5000000); // interruption then next reply
                assert(samples==2880);
            }
        '''
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / "timeline.cc").write_text(driver, encoding="utf-8")
            binary = path / ("timeline.exe" if os.name == "nt" else "timeline")
            result = subprocess.run(compiler + ["-std=c++17", "-I", str(ROOT / "main/audio"),
                                    str(path / "timeline.cc"), "-o", str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_mode_cycle_display_and_manual_click(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("requires a host C++17 compiler")
        source = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        methods = []
        for signature in ["bool Application::SupportsRealtimeMode() const",
                          "void Application::EndConversation()",
                          "void Application::HandleCycleListeningModeEvent()",
                          "void Application::UpdateConversationStatus()",
                          "void Application::HandleToggleChatEvent()",
                          "ListeningMode Application::GetDefaultListeningMode() const"]:
            start = source.index(signature)
            brace = source.index("{", start)
            depth, end = 1, brace + 1
            while depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            methods.append(source[start:end])
        driver = r'''
            #include <atomic>
            #include <cassert>
            #include <functional>
            #include <memory>
            #include <string>
            #define ESP_LOGE(...) ((void)0)
            #define CONFIG_ALIYUN_REALTIME 1
            enum ListeningMode{kListeningModeAutoStop,kListeningModeManualStop,kListeningModeRealtime};
            enum AecMode{kAecOff,kAecOnDeviceSide,kAecOnServerSide};
            enum State{kDeviceStateIdle,kDeviceStateConnecting,kDeviceStateListening,kDeviceStateSpeaking,
                kDeviceStateNotifying,kDeviceStateActivating,kDeviceStateWifiConfiguring,kDeviceStateAudioTesting,
                kDeviceStateStarting};
            constexpr int kAbortReasonNone=0;
            namespace Lang::Strings {constexpr auto CONNECTING="Connecting",LISTENING="Listening",SPEAKING="Speaking";}
            struct Audio {
                bool reference=true;int stops=0,resets=0;bool reset_reference=true;
                bool SupportsRealtime()const{return reference;}
                void EnableVoiceProcessing(bool b){assert(!b);++stops;}
                void ResetDecoder(bool b){++resets;reset_reference=b;}
                void EnableAudioTesting(bool){}
            };
            struct Protocol {bool opened=true;int closes=0;
                bool IsAudioChannelOpened(){return opened;}
                void CloseAudioChannel(){opened=false;++closes;}};
            struct Display {std::string status;void SetStatus(const char* s){status=s;}};
            class Board {Display display;
            public:static Board& GetInstance(){static Board b;return b;}Display* GetDisplay(){return &display;}};
            class Application {
            public:
                State state=kDeviceStateIdle;Audio audio_service_;
                std::unique_ptr<Protocol> protocol_=std::make_unique<Protocol>();
                ListeningMode preferred_listening_mode_=kListeningModeAutoStop,listening_mode_=kListeningModeAutoStop;
                std::atomic<ListeningMode> displayed_listening_mode_{kListeningModeAutoStop};
                bool listening_mode_selected_=false,aliyun_mode_=true,aliyun_response_done_=false;
                bool pending_listening_start_=true,play_popup_on_listening_=true;
                std::string last_error_message_;AecMode aec_mode_=kAecOff;
                int commits=0;
                State GetDeviceState()const{return state;}
                void SetDeviceState(State s){state=s;}
                void StopNotification(){state=kDeviceStateIdle;}
                void AbortSpeaking(int){}
                void HandleStopListeningEvent(){++commits;}
                void SetListeningMode(ListeningMode m){listening_mode_=m;state=kDeviceStateListening;}
                void ContinueOpenAudioChannel(ListeningMode m){SetListeningMode(m);}
                void Schedule(std::function<void()> f){f();}
                bool SupportsRealtimeMode()const;
                void EndConversation();
                void HandleCycleListeningModeEvent();void UpdateConversationStatus();void HandleToggleChatEvent();
                ListeningMode GetDefaultListeningMode()const;
            };
        ''' + "\n".join(methods) + r'''
            int main(){
                Application a;a.HandleCycleListeningModeEvent();
                assert(a.GetDefaultListeningMode()==kListeningModeRealtime);
                assert(a.state==kDeviceStateIdle && a.audio_service_.stops==1 && !a.audio_service_.reset_reference);
                assert(!a.pending_listening_start_ && !a.play_popup_on_listening_);
                auto display=Board::GetInstance().GetDisplay();assert(display->status=="Realtime");
                a.state=kDeviceStateSpeaking;a.protocol_->opened=true;a.listening_mode_=kListeningModeRealtime;
                a.HandleCycleListeningModeEvent();assert(a.state==kDeviceStateIdle);
                assert(a.GetDefaultListeningMode()==kListeningModeManualStop && display->status=="Manual");
                a.state=kDeviceStateListening;a.listening_mode_=kListeningModeManualStop;
                a.HandleToggleChatEvent();assert(a.commits==1); // submit, rather than close session
                a.state=kDeviceStateIdle;a.HandleCycleListeningModeEvent();assert(display->status=="Auto");
                a.audio_service_.reference=false;a.HandleCycleListeningModeEvent();assert(display->status=="Manual");
                a.state=kDeviceStateStarting;a.HandleCycleListeningModeEvent();assert(display->status=="Manual");
                a.state=kDeviceStateConnecting;a.UpdateConversationStatus();assert(display->status=="Manual | Connecting");
                a.state=kDeviceStateIdle;a.last_error_message_="error";display->status="Error";
                a.UpdateConversationStatus();assert(display->status=="Error");
                a.aliyun_mode_=false;a.listening_mode_selected_=false;a.aec_mode_=kAecOnServerSide;
                assert(a.SupportsRealtimeMode());
                // A finished Manual reply is idle but still owns an open channel.
                a.state=kDeviceStateIdle;a.protocol_->opened=true;
                auto closes=a.protocol_->closes;
                a.EndConversation();
                assert(!a.protocol_->opened && a.protocol_->closes==closes+1);
                assert(a.state==kDeviceStateIdle && !a.audio_service_.reset_reference);
                a.state=kDeviceStateStarting;a.protocol_->opened=true;
                closes=a.protocol_->closes;a.EndConversation();
                assert(a.protocol_->opened && a.protocol_->closes==closes);
            }
        '''
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / "modes.cc").write_text(driver, encoding="utf-8")
            binary = path / ("modes.exe" if os.name == "nt" else "modes")
            result = subprocess.run(compiler + ["-std=c++17", str(path / "modes.cc"), "-o", str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_aec_playback_tail_and_manual_bypass(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("requires a host C++17 compiler")
        source = (ROOT / "main/audio/engines/afe_audio_engine.cc").read_text(encoding="utf-8")
        start = source.index("void AfeAudioEngine::ApplyAfeControls()")
        brace = source.index("{", start)
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        method = source[start:end]
        driver = r'''
            #include <atomic>
            #include <cassert>
            #include <cstdint>
            #define CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC 0
            using EventBits_t=uint32_t;
            int64_t now=0;int64_t esp_timer_get_time(){return now;}
            uint32_t xEventGroupGetBits(uint32_t* p){return *p;}
            struct Codec {bool input_reference(){return true;}};
            int aec_on=0,aec_off=0,wake_on=0,wake_off=0;
            struct Interface {
                void enable_aec(void*){++aec_on;} void disable_aec(void*){++aec_off;}
                void enable_wakenet(void*){++wake_on;} void disable_wakenet(void*){++wake_off;}
            };
            class AfeAudioEngine {
            public:
                enum class WakeDetector{kWakeNet};
                WakeDetector wake_detector_=WakeDetector::kWakeNet;
                static constexpr int kWakeWordEnabled=1,kVoiceProcessingEnabled=2;
                uint32_t events=0;uint32_t* event_group_=&events;
                Codec codec;Codec* codec_=&codec;
                Interface iface;Interface* afe_iface_=&iface;void* afe_data_=nullptr;
                std::atomic<bool> playback_aec_only_{true},device_aec_enabled_{false};
                std::atomic<int64_t> playback_aec_until_us_{0};
                std::atomic<bool> applied_aec_enabled_{false};bool applied_wake_enabled_=false;
                void ApplyAfeControls();
            };
        ''' + method + r'''
            int main(){
                AfeAudioEngine e;e.events=1;e.ApplyAfeControls();
                assert(wake_on==1 && aec_on==0); // Manual/Auto wake cannot force AEC on
                e.device_aec_enabled_=true;e.events=2;e.playback_aec_until_us_=300000;
                now=1;e.ApplyAfeControls();assert(aec_on==1 && wake_off==1);
                now=200000;e.ApplyAfeControls();assert(aec_on==1 && aec_off==0);
                now=300000;e.ApplyAfeControls();assert(aec_off==1);
                e.ApplyAfeControls();assert(aec_off==1); // no repeated state-resetting toggles
                e.playback_aec_until_us_=600000;e.ApplyAfeControls();assert(aec_on==2);
                e.device_aec_enabled_=false;e.ApplyAfeControls();assert(aec_off==2);
                e.playback_aec_only_=false;e.events=1;e.ApplyAfeControls();assert(aec_on==3);
            }
        '''
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / "aec.cc").write_text(driver, encoding="utf-8")
            binary = path / ("aec.exe" if os.name == "nt" else "aec")
            result = subprocess.run(compiler + ["-std=c++17", str(path / "aec.cc"), "-o", str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_full_duplex_cancellation_late_events_and_reconnect(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("requires a host C++17 compiler")
        cjson = ROOT / "managed_components/espressif__cjson/cJSON"
        if not (cjson / "cJSON.c").is_file():
            self.skipTest("requires the resolved cJSON component")
        stubs = {
            "freertos/FreeRTOS.h": r'''
                #pragma once
                #include <cstdint>
                constexpr bool pdTRUE=true, pdFALSE=false;
                #define pdMS_TO_TICKS(x) (x)
                #define BIT0 1
                #define BIT1 2
                #define BIT2 4
            ''',
            "freertos/event_groups.h": r'''
                #pragma once
                #include "FreeRTOS.h"
                using EventBits_t=uint32_t;
                using EventGroupHandle_t=uint32_t*;
                inline auto xEventGroupCreate(){return new uint32_t(0);}
                inline void vEventGroupDelete(EventGroupHandle_t p){delete p;}
                inline void xEventGroupSetBits(EventGroupHandle_t p,uint32_t b){*p|=b;}
                inline void xEventGroupClearBits(EventGroupHandle_t p,uint32_t b){*p&=~b;}
                inline uint32_t xEventGroupWaitBits(EventGroupHandle_t p,uint32_t b,bool clear,bool,int){
                    auto result=*p&b;if(clear)*p&=~b;return result;
                }
            ''',
            "esp_log.h": "#define ESP_LOGI(...)\n#define ESP_LOGW(...)\n#define ESP_LOGE(...)\n",
            "mbedtls/base64.h": r'''
                #pragma once
                #include <cstring>
                inline int mbedtls_base64_encode(unsigned char* out,size_t,size_t* n,const unsigned char*,size_t){
                    memcpy(out,"AAA=",4);*n=4;return 0;
                }
                inline int mbedtls_base64_decode(unsigned char* out,size_t,size_t* n,const unsigned char*,size_t){
                    out[0]=out[1]=0;*n=2;return 0;
                }
            ''',
            "application.h": r'''
                #pragma once
                #include <deque>
                #include <functional>
                class Application {
                public:
                    static Application& GetInstance(){static Application a;return a;}
                    std::deque<std::function<void()>> tasks;
                    void Schedule(std::function<void()> f){tasks.push_back(std::move(f));}
                    void Run(){while(!tasks.empty()){auto f=std::move(tasks.front());tasks.pop_front();f();}}
                };
            ''',
            "web_socket.h": r'''
                #pragma once
                #include <functional>
                #include <string>
                #include <vector>
                struct ConnectResult {operator bool()const{return true;}
                    ConnectResult error()const{return {};}
                    std::string ToString()const{return "error";}};
                class WebSocket {
                public:
                    std::function<void(const char*,size_t,bool)> data;
                    std::function<void()> disconnected;
                    std::vector<std::string> sent;
                    bool connected=true;
                    void SetHeader(const char*,const char*){}
                    void OnData(decltype(data) f){data=std::move(f);}
                    void OnPing(std::function<void(const char*,size_t)>){ }
                    void OnDisconnected(std::function<void()> f){disconnected=std::move(f);}
                    void Emit(std::string s){data(s.data(),s.size(),false);}
                    ConnectResult Connect(const char*){Emit(R"({"type":"session.created","session":{"id":"s"}})");return {};}
                    bool IsConnected()const{return connected;}
                    bool Send(const std::string& s){sent.push_back(s);
                        if(s.find("session.update")!=std::string::npos)Emit(R"({"type":"session.updated"})");
                        return true;}
                    void SendPong(const char*,size_t){}
                };
            ''',
            "board.h": r'''
                #pragma once
                #include <memory>
                #include "web_socket.h"
                struct Network {std::shared_ptr<WebSocket> socket;
                    auto CreateWebSocket(int){socket=std::make_shared<WebSocket>();return socket;}};
                class Board {Network network;
                public:
                    static Board& GetInstance(){static Board b;return b;}
                    Network* GetNetwork(){return &network;}
                };
            ''',
            "debug_trace.h": r'''
                #pragma once
                #include "config.h"
                struct DebugTrace {
                    static DebugTrace& Instance(){static DebugTrace d;return d;}
                    void Configure(const AliyunRealtimeConfig&){}
                    void Open(const char*){} void Close(){} void SetMode(const char*){}
                    void Event(const char*,const char*=""){}
                    void WebSocketTx(const char*,size_t,bool,bool=false){} void WebSocketRx(const char*,size_t,bool=false){}
                    void Pcm(bool,const uint8_t*,size_t,int){}
                };
            ''',
        }
        driver = r'''
            #include <cassert>
            #include "aliyun_protocol.h"
            #include "application.h"
            #include "board.h"
            bool AliyunRealtimeConfig::IsReady()const{return true;}
            std::string AliyunRealtimeConfig::WebSocketUrl()const{return "test";}
            void Protocol::OnIncomingAudio(decltype(on_incoming_audio_) f){on_incoming_audio_=std::move(f);}
            void Protocol::OnAudioChannelClosed(decltype(on_audio_channel_closed_) f){on_audio_channel_closed_=std::move(f);}
            void Protocol::SendWakeWordDetected(const std::string&){}
            void Protocol::SendStartListening(ListeningMode){}
            void Protocol::SendStopListening(){}
            void Protocol::SendAbortSpeaking(AbortReason){}
            void Protocol::SendMcpMessage(const std::string&){}
            void Protocol::SetError(const std::string&){error_occurred_=true;}
            bool Protocol::IsTimeout()const{return false;}
            auto packet(){auto p=std::make_unique<AudioStreamPacket>();p->pcm=true;p->payload={0,0};return p;}
            size_t cancels(const std::shared_ptr<WebSocket>& s){size_t n=0;for(auto& x:s->sent)n+=x.find("response.cancel")!=std::string::npos;return n;}
            int main(){
                AliyunProtocol p({});
                int audio=0,done=0,speech=0,closed=0;uint64_t start=0,interrupted=0;
                p.OnIncomingAudio([&](auto){++audio;});
                p.OnResponseStarted([&](auto token){start=token;});
                p.OnResponseDone([&](auto){++done;});
                p.OnSpeechStarted([&](auto token){++speech;interrupted=token;});
                p.OnAudioChannelClosed([&](){++closed;});
                assert(p.OpenAudioChannel());auto s=Board::GetInstance().GetNetwork()->socket;
                p.SendStartListening(kListeningModeRealtime);assert(p.SendAudio(packet()));
                assert(s->sent[s->sent.size()-2].find("server_vad")!=std::string::npos);
                assert(s->sent[s->sent.size()-2].find("\"threshold\":0.5")!=std::string::npos);
                s->Emit(R"({"type":"response.created","response":{"id":"a"}})");
                assert(p.SendAudio(packet()));assert(p.IsCurrentResponse(start));
                s->Emit(R"({"type":"response.audio.delta","response_id":"a","delta":"AAA="})");assert(audio==1);
                s->Emit(R"({"type":"input_audio_buffer.speech_started"})");
                assert(speech==1 && !p.IsCurrentResponse(start) && p.IsCurrentResponse(interrupted));
                s->Emit(R"({"type":"response.audio.delta","response_id":"a","delta":"AAA="})");
                s->Emit(R"({"type":"response.done","response":{"id":"a"}})");
                assert(audio==1 && done==0 && p.SendAudio(packet()));
                Application::GetInstance().Run();assert(cancels(s)==1);
                s->Emit(R"({"type":"response.created","response":{"id":"b"}})");
                s->Emit(R"({"type":"response.done","response":{"id":"b"}})");assert(done==1);
                s->Emit(R"({"type":"response.done","response":{"id":"b"}})");assert(done==1);
                s->Emit(R"({"type":"input_audio_buffer.speech_started"})");
                Application::GetInstance().Run();assert(speech==2 && cancels(s)==1);
                s->Emit(R"({"type":"response.created","response":{"id":"c"}})");
                s->Emit(R"({"type":"input_audio_buffer.speech_started"})");
                s->Emit(R"({"type":"response.created","response":{"id":"d"}})");
                Application::GetInstance().Run();assert(cancels(s)==1); // queued cancel cannot kill d
                p.SendAbortSpeaking(kAbortReasonNone);assert(cancels(s)==2 && p.SendAudio(packet()));
                p.CloseAudioChannel();assert(closed==1 && !p.IsCurrentResponse(start));
                assert(p.OpenAudioChannel());auto fresh=Board::GetInstance().GetNetwork()->socket;
                s->Emit(R"({"type":"response.created","response":{"id":"old"}})");
                assert(!p.IsCurrentResponse(start));
                p.SendStartListening(kListeningModeAutoStop);assert(p.SendAudio(packet()));
                fresh->Emit(R"({"type":"response.created","response":{"id":"auto"}})");
                assert(!p.SendAudio(packet()));
                p.CloseAudioChannel();assert(p.OpenAudioChannel());
                p.SendStartListening(kListeningModeManualStop);assert(p.SendAudio(packet()));
                p.SendStopListening();assert(!p.SendAudio(packet()));
                auto manual=Board::GetInstance().GetNetwork()->socket;
                manual->Emit(R"({"type":"input_audio_buffer.committed"})");
                p.CloseAudioChannel();Application::GetInstance().Run();
                assert(manual->sent.back().find("response.create")==std::string::npos);
            }
        '''
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            for name, contents in stubs.items():
                target = path / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(textwrap.dedent(contents).replace('char*=""', 'char* = ""'))
            (path / "aliyun_protocol.cc").write_text(
                (ROOT / "main/realtime/aliyun_protocol.cc").read_text(encoding="utf-8"))
            (path / "driver.cc").write_text(textwrap.dedent(driver))
            binary = path / ("protocol-test.exe" if os.name == "nt" else "protocol-test")
            result = subprocess.run(compiler + ["-std=c++17", "-I", str(path),
                "-I", str(ROOT / "main/realtime"), "-I", str(ROOT / "main/protocols"),
                "-I", str(cjson), str(path / "driver.cc"), str(path / "aliyun_protocol.cc"),
                "-x", "c++", str(cjson / "cJSON.c"), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

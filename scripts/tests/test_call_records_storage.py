"""Run the production SD writer against real files and controlled platform failures."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CXX = os.environ.get("PHONE_AI_CXX") or shutil.which("c++") or shutil.which("g++")


@unittest.skipUnless(CXX, "requires a native C++ compiler (PHONE_AI_CXX)")
class CallRecordsStorageTests(unittest.TestCase):
    def test_real_writer_full_card_write_failure_pagination_recovery_and_preemption(self):
        stubs = {
            "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n",
            "esp_log.h": "#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n",
            "esp_heap_caps.h": r'''#pragma once
#include <cstdlib>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
inline void* heap_caps_malloc(size_t n,unsigned){return std::malloc(n);}
inline void* heap_caps_calloc(size_t n,size_t s,unsigned){return std::calloc(n,s);}
inline void heap_caps_free(void* p){std::free(p);}
inline size_t heap_caps_get_free_size(unsigned){return 8000000;}
inline size_t heap_caps_get_minimum_free_size(unsigned){return 100000;}
inline size_t heap_caps_get_largest_free_block(unsigned){return 8000000;}
''',
            "esp_timer.h": "#pragma once\n#include <cstdint>\ninline int64_t fake_us=1000000;\ninline int64_t esp_timer_get_time(){return fake_us;}\n",
            "esp_random.h": "#pragma once\n#include <cstdint>\ninline uint32_t esp_random(){static uint32_t n=1;return n++;}\n",
            "esp_vfs_fat.h": "#pragma once\n#include <cstdint>\ninline uint64_t fake_free=64000000;\ninline int esp_vfs_fat_info(const char*,uint64_t* t,uint64_t* f){*t=64000000;*f=fake_free;return 0;}\n",
            "freertos/FreeRTOS.h": r'''#pragma once
#include <cstdint>
using BaseType_t=int; using UBaseType_t=unsigned;
#define pdFAIL 0
#define pdPASS 1
#define pdMS_TO_TICKS(n) (n)
inline void vTaskDelay(unsigned){}
inline void vTaskDelete(void*){}
inline int xTaskCreate(void(*)(void*),const char*,uint32_t,void*,unsigned,void*){return pdFAIL;}
''',
            "freertos/idf_additions.h": "#pragma once\n#include \"FreeRTOS.h\"\n",
            "platform.h": r'''#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#define fsync _commit
#define mkdir(path, mode) _mkdir(path)
#endif
#define _APPLICATION_H_
#define BOARD_H
#define CONFIG_PHONE_RECORDS 1
#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 0
enum {kDeviceStateIdle};
struct AudioStreamPacket { bool pcm; int sample_rate,frame_duration;std::vector<uint8_t> payload; };
struct FakeAudio {
    void ResetDecoder(){} void EnableWakeWordDetection(bool){}
    bool IsPlaybackIdle(){return true;}
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket>){return true;}
};
struct Application {
    static Application& GetInstance(){static Application a;return a;}
    FakeAudio audio;
    FakeAudio& GetAudioService(){return audio;}
    void Schedule(std::function<void()> f){f();}
    int GetDeviceState(){return kDeviceStateIdle;}
};
struct Board {
    static Board& GetInstance(){static Board b;return b;}
    bool MountCallStorage(const char**,std::string&){return false;}
    void UnmountCallStorage(){}
};
'''
        }
        code = r'''
#include "platform.h"
#define private public
#include "call_records.h"
#undef private
#include "call_records.cc"
#include <cassert>
#include <fstream>
bool AliyunRealtimeConfig::IsReady()const{return false;}
CallRecordCloudResult ProcessCallRecording(const std::string&,const AliyunRealtimeConfig&,
                                          const std::function<bool()>&){return {};}
bool phone_fresh=false;
phone_status_t observed_phone{};
uint32_t observed_duration=0;
extern "C" bool gateway_manager_get_phone_status(phone_status_t* p,int64_t* age,uint32_t* duration){
    *p=observed_phone;*age=0;*duration=observed_duration;return phone_fresh;
}

int main() {
    using namespace call_record;
    CallRecords::Impl store;
    store.base="calls"; assert(mkdir("calls",0775)==0);
    store.mounted=true;
    store.queue=new CallRecords::Impl::Event[kTextSlots];
    auto& service=CallRecords::Instance(); service.impl_=&store;
    phone_status_t phone{};phone.call_id=1;phone.phase=PHONE_ACTIVE;
    int16_t pcm[320];std::fill_n(pcm,320,1000);
    store.Begin(phone,0);
    assert(store.current.save==CallSaveState::Recording);
    const auto id=std::string(store.current.id);
    store.queue_mutex.lock();
    service.Capture(false,pcm,320,16000,100,7);
    store.queue_mutex.unlock();
    service.Capture(true,pcm,320,16000);
    service.Transcript(true,"known caller text");
    store.Drain();
    assert(store.written[0]==320 && store.written[1]==320);
    // Finish must drain the final frames already queued before the producer gate closes.
    // A 300ms arrival stall does not stretch two consecutive media frames.
    fake_us+=300000;
    service.Capture(false,pcm,320,16000,101,7);
    fake_us-=300000;
    service.Capture(true,pcm,320,16000);
    fake_us+=40000;store.Finish(40,false);
    assert(store.current.save==CallSaveState::Saved && store.total==1);
    CallRecordInfo info;
    assert(ReadInfo(store.Path(id.c_str(),"meta.json"),info));
    assert(info.duration_ms==40 && info.audio_available);
    assert(info.call_id==1 && info.captured_samples[0]==640);
    assert(info.gap_samples==0);
    // A truncated primary must fall back to the last committed backup.
    assert(WriteJson(store.Path(id.c_str(),"meta.json"),InfoJson(info)));
    {std::ofstream broken(store.Path(id.c_str(),"meta.json"));broken<<"{\"schema\":";}
    assert(ReadInfo(store.Path(id.c_str(),"meta.json"),info));
    assert(info.duration_ms==40);
    assert(WriteJson(store.Path(id.c_str(),"meta.json"),InfoJson(info)));
    size_t bytes;uint64_t total;uint8_t data[1024];
    assert(service.ReadFile(id.c_str(),"caller.wav",0,data,sizeof(data),bytes,total));
    assert(total==44+640*2 && bytes==1024);
    assert(!service.ReadFile("../secret","caller.wav",0,data,sizeof(data),bytes,total));
    assert(!service.ReadFile(id.c_str(),"../meta.json",0,data,sizeof(data),bytes,total));
    store.LoadDetail(id.c_str(),0);assert(strstr(store.view.transcript,"known caller text"));
    // Ordinary pages must navigate back to their actual preceding page.
    {FILE* text=fopen(store.Path(id.c_str(),"live-transcript.jsonl").c_str(),"wb");
     for(int n=0;n<20;++n)fprintf(text,"{\"final\":true,\"text\":\"entry %d\"}\n",n);
     fclose(text);}
    store.LoadDetail(id.c_str(),0);assert(store.view.transcript_next==8 && store.view.transcript_previous==0);
    store.LoadDetail(id.c_str(),8);assert(store.view.transcript_next==16 && store.view.transcript_previous==0);
    store.LoadDetail(id.c_str(),16);assert(!store.view.transcript_more && store.view.transcript_previous==8);
    store.LoadDetail(id.c_str(),store.view.transcript_previous);assert(store.view.transcript_offset==8 && store.view.transcript_previous==0);
    store.LoadDetail(id.c_str(),store.view.transcript_previous);assert(store.view.transcript_offset==0);
    // JSON escaping may exceed a 4K line; long pages must return the actual cursor.
    {FILE* text=fopen(store.Path(id.c_str(),"live-transcript.jsonl").c_str(),"wb");
     for(int n=0;n<20;++n){auto* j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"final",true);
       std::string controls(1530,'\x01');cJSON_AddStringToObject(j,"text",controls.c_str());
       char* line=cJSON_PrintUnformatted(j);fprintf(text,"%s\n",line);cJSON_free(line);cJSON_Delete(j);}
     fclose(text);}
    store.LoadDetail(id.c_str(),0);assert(store.view.transcript_more && store.view.transcript_next==7);
    store.LoadDetail(id.c_str(),store.view.transcript_next);assert(store.view.transcript_more && store.view.transcript_next==14 && store.view.transcript_previous==0);
    store.LoadDetail(id.c_str(),store.view.transcript_next);assert(!store.view.transcript_more && store.view.transcript_previous==7);
    store.LoadDetail(id.c_str(),store.view.transcript_previous);assert(store.view.transcript_offset==7 && store.view.transcript_previous==0);
    store.LoadDetail(id.c_str(),store.view.transcript_previous);assert(store.view.transcript_offset==0);
    // Full storage fails recording without changing the phone lifecycle.
    fake_free=0;phone.call_id=2;store.Begin(phone,0);
    assert(store.active && store.current.save==CallSaveState::Failed);
    assert(!strcmp(store.current.error,"storage_full"));store.Finish(20,false);
    fake_free=64000000;
    // No mounted card reports a save failure and keeps phone observation alive.
    store.mounted=false;store.Begin(phone,0);
    assert(store.active && !strcmp(store.current.error,"storage_unavailable"));
    store.Finish(20,false);assert(store.current.save==CallSaveState::Failed);
    store.mounted=true;
    // A real fwrite failure is propagated; no fabricated successful save.
    phone.call_id=3;store.Begin(phone,0);
    fclose(store.tracks[0]);store.tracks[0]=fopen(store.Path(store.current.id,"caller.wav").c_str(),"rb");
    service.Capture(false,pcm,320,16000);store.Drain();
    assert(store.active && store.current.save==CallSaveState::Failed);
    store.Finish(20,false);assert(store.current.save==CallSaveState::Failed);
    // Power-loss fixture: closed files but persistent state remains Recording.
    phone.call_id=4;store.Begin(phone,0);const auto interrupted=std::string(store.current.id);
    service.Capture(false,pcm,320,16000);service.Capture(true,pcm,320,16000);store.Drain();
    for(auto& file:store.tracks){fclose(file);file=nullptr;}
    fclose(store.texts);store.texts=nullptr;store.active=false;store.token=0;
    store.Recover();assert(store.mounted);
    assert(ReadInfo(store.Path(interrupted.c_str(),"meta.json"),info));
    assert(info.save==CallSaveState::Incomplete && !strcmp(info.error,"interrupted_save"));
    store.LoadPage(0);assert(store.view.count==3 && store.view.total==3);
    store.OpenPlayback(id.c_str());assert(store.view.playback==CallPlaybackState::Playing);
    service.Pause();service.Seek(30);store.Playback();
    assert(store.view.playback==CallPlaybackState::Paused && store.view.playback_ms==30);
    store.playback_stop=true;store.Playback();assert(!store.playback_files[0]);
    // A different record must not inherit the old recording's seek position.
    store.view.playback_ms=100000;store.view.playback_duration_ms=200000;
    service.RequestDetail(interrupted.c_str());
    assert(store.view.playback_ms==0 && store.view.playback_duration_ms==0);
    store.LoadDetail(interrupted.c_str(),0);
    assert(store.view.playback_ms==0 && store.view.playback_duration_ms==info.duration_ms);
    // Missing media cannot be reported as playable.
    remove(store.Path(id.c_str(),"caller.wav").c_str());store.OpenPlayback(id.c_str());
    assert(store.view.playback==CallPlaybackState::Failed);
    // A consumer stall exceeding the old 256-frame bound retains every sample.
    phone.call_id=6;store.Begin(phone,0);store.DrainAudio();
    fake_us+=3000000;
    for(unsigned n=0;n<600;++n)
        service.Capture(n%2,pcm,320,16000);
    assert(store.dropped==0 && store.queue_peak==600);
    store.DrainAudio();
    assert(store.current.captured_samples[0]==96000 && store.current.captured_samples[1]==96000);
    assert(store.current.max_drain_gap_ms==3000);
    // Capacity is finite and separate text overflow cannot be mislabeled as PCM loss.
    for(unsigned n=0;n<kPcmSlots+7;++n)
        service.Capture(n%2,pcm,320,16000);
    assert(store.dropped==7 && store.queue_peak==kPcmSlots);
    for(unsigned n=0;n<kTextSlots+2;++n)service.Transcript(true,"bounded final");
    assert(store.text_drops==2 && store.dropped==7);
    store.Finish(20000,false);
    assert(store.current.save==CallSaveState::Incomplete);
    assert(store.current.captured_samples[0]+store.current.captured_samples[1]==(600+kPcmSlots)*320);
    assert(ReadInfo(store.Path(store.current.id,"meta.json"),info));
    assert(info.queue_peak==kPcmSlots && info.dropped_frames==7 && info.text_drops==2);
    assert(info.max_drain_gap_ms==3000);
    // A short control outage must retain one recording for the same call ID.
    phone.call_id=7;store.Begin(phone,0);const auto retained=std::string(store.current.id);
    const auto count_before=store.total;
    fake_us+=7000000;store.Observe();
    assert(store.active && store.state_interrupted && store.total==count_before);
    phone_fresh=true;observed_phone=phone;observed_duration=7000;store.Observe();
    assert(store.active && store.current.id==retained && store.total==count_before);
    observed_phone.phase=PHONE_ENDED;store.Observe();
    assert(!store.active && store.total==count_before);
    assert(store.current.save==CallSaveState::Incomplete);
    assert(!strcmp(store.current.error,"call_state_lost"));
    // A permanently missing peer still ends storage within a bounded grace.
    phone_fresh=false;phone.call_id=8;store.Begin(phone,0);
    fake_us+=kRecordStateGraceUs+1000;store.Observe();assert(!store.active);
    // Cleanup is explicit, serial/debug-only and refuses busy or stale phones.
    store.request=CallRecords::Impl::Request::None;
    assert(service.Command("records delete ../secret"));
    assert(store.request==CallRecords::Impl::Request::None);
    assert(service.Command(("records delete "+interrupted).c_str()));
    assert(store.request==CallRecords::Impl::Request::None); // debug disabled
    store.cloud_config.debug_enabled=true;
    assert(service.Command(("records delete "+interrupted).c_str()));
    assert(store.request==CallRecords::Impl::Request::Delete);
    store.request=CallRecords::Impl::Request::None;
    const auto before_delete=store.total;
    store.DeleteRecord(interrupted.c_str()); // stale phone
    assert(store.total==before_delete && ReadInfo(store.Path(interrupted.c_str(),"meta.json"),info));
    phone_fresh=true;observed_phone.phase=PHONE_ACTIVE;
    store.DeleteRecord(interrupted.c_str());
    assert(store.total==before_delete);
    observed_phone.phase=PHONE_ENDED;store.post_running=true;
    store.DeleteRecord(interrupted.c_str());assert(store.total==before_delete);
    store.post_running=false;store.OpenPlayback(interrupted.c_str());
    assert(store.playback_files[0]);
    store.DeleteRecord(interrupted.c_str());assert(store.total==before_delete);
    store.ClosePlayback();
    // A nested directory blocks cleanup before the record is renamed or changed.
    const auto nested=store.Path(interrupted.c_str(),"unrelated");assert(mkdir(nested.c_str(),0775)==0);
    store.DeleteRecord(interrupted.c_str());assert(store.total==before_delete);
    assert(ReadInfo(store.Path(interrupted.c_str(),"meta.json"),info));
    assert(rmdir(nested.c_str())==0);
    {std::ofstream keep("calls/keep.txt");keep<<"unrelated root file";}
    store.LoadDetail(interrupted.c_str(),0);
    store.DeleteRecord(interrupted.c_str());
    assert(store.total==before_delete-1 && !store.view.detail.id[0]);
    assert(!ReadInfo(store.Path(interrupted.c_str(),"meta.json"),info));
    assert(ReadInfo(store.Path(id.c_str(),"meta.json"),info));
    {std::ifstream keep("calls/keep.txt");assert(keep.good());}
    // Simulate loss of power after the exact record directory was staged.
    const auto staged="calls/deleted-"+id;
    assert(rename(("calls/"+id).c_str(),staged.c_str())==0);
    store.Recover();const auto after_staging=store.total;
    assert(after_staging==before_delete-2); // never revive half-deleted media
    store.DeleteRecord(id.c_str());assert(store.total==after_staging);
    struct stat deleted_stat{};assert(stat(staged.c_str(),&deleted_stat)!=0);
    store.DeleteRecord(nullptr);assert(store.total==after_staging);
    // Diagnose each track without treating initial wait, delayed consecutive
    // media, or final WAV padding as a mid-call dropout.
    phone.call_id=9;store.Begin(phone,0);
    fake_us+=200000;
    service.Capture(false,pcm,320,16000,100,10);
    service.Capture(true,pcm,320,16000);store.Drain();
    assert(store.current.gap_samples==0);
    fake_us+=20000;
    service.Capture(false,pcm,320,16000,102,10); // one missing incoming frame
    service.Capture(true,pcm,320,16000);store.Drain();
    fake_us+=300000;
    service.Capture(false,pcm,320,16000,103,10); // delayed, still consecutive
    service.Capture(true,pcm,320,16000);store.Drain(); // real transmit-clock absence
    fake_us+=20000;
    service.Capture(false,pcm,320,16000,106,10); // two missing incoming frames
    service.Capture(true,pcm,320,16000);store.Drain();
    store.Finish(560,false);
    assert(ReadInfo(store.Path(store.current.id,"meta.json"),info));
    assert(info.gap_details_available && info.gap_samples==5440);
    assert(info.track_gap_samples[0]==960 && info.track_gap_samples[1]==4480);
    assert(info.track_gap_events[0]==2 && info.track_gap_events[1]==1);
    assert(info.track_max_gap_samples[0]==640 && info.track_max_gap_samples[1]==4480);
    assert(info.captured_samples[0]==1280 && info.captured_samples[1]==1280);
    assert(info.save==CallSaveState::Incomplete && !strcmp(info.error,"audio_gaps"));
    // Earlier metadata retains its aggregate loss, with per-track detail unknown.
    auto* legacy=InfoJson(info);
    for(const char* key:{"gap_details_available","caller_gap_samples","assistant_gap_samples",
                         "caller_gap_events","assistant_gap_events","caller_max_gap_samples",
                         "assistant_max_gap_samples"})cJSON_DeleteItemFromObject(legacy,key);
    assert(WriteJson(store.Path(store.current.id,"meta.json"),legacy));
    assert(ReadInfo(store.Path(store.current.id,"meta.json"),info));
    assert(!info.gap_details_available && info.gap_samples==5440);
    auto* restored=InfoJson(info);
    assert(!cJSON_GetObjectItem(restored,"caller_gap_samples"));cJSON_Delete(restored);
    delete[] store.queue;service.impl_=nullptr;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            for name, content in stubs.items():
                target = path / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content, encoding="utf8")
            src, exe = path / "storage.cc", path / "storage.exe"
            src.write_text(code, encoding="utf8")
            compiler = [CXX]
            if Path(CXX).name.lower() in ("zig", "zig.exe"):
                compiler.append("c++")
            cjson = ROOT / "managed_components/espressif__cjson/cJSON"
            c_compiler = [CXX, "cc"] if Path(CXX).name.lower() in ("zig", "zig.exe") else [os.environ.get("CC") or shutil.which("cc") or "gcc"]
            cjson_object = path / "cjson.o"
            result = subprocess.run(c_compiler + ["-c", str(cjson / "cJSON.c"), "-o", str(cjson_object)],
                                    capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run(compiler + ["-std=c++23", "-UNDEBUG", "-include", str(path / "platform.h"),
                "-I", str(path), "-I", str(ROOT / "main"), "-I", str(ROOT / "main/boards/common"),
                "-I", str(cjson), str(src), str(cjson_object),
                "-o", str(exe)], capture_output=True, text=True, timeout=180)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=path, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

#include "call_records.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if CONFIG_PHONE_RECORDS
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <ctime>
#include <mutex>
#include <string>
#include "application.h"
#include "background_work.h"
#include "board.h"
#include "cJSON.h"
#include "call_record_cloud.h"
#include "call_record_format.h"
#include "call_record_memory.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "gateway_manager.h"

namespace {
constexpr char kTag[] = "call_records";
// At two 20 ms tracks, PSRAM retains about ten seconds of SD/task stalls.
// Text has its own small bound; increasing PCM capacity must not multiply it.
constexpr size_t kPcmSlots = 1024, kTextSlots = 64, kIndexSize = 18;
constexpr uint64_t kReserveBytes = 2 * 1024 * 1024;
// Keep the storage identity through a short control reconnect. This does not
// extend the phone/media lease or declare the interrupted speech successful.
constexpr int64_t kRecordStateGraceUs = 30000000;
const char* SaveName(CallSaveState s) {
    static const char* names[] = {"unavailable", "ready",      "recording", "saving",
                                  "saved",       "incomplete", "failed"};
    return names[unsigned(s)];
}
template <size_t N>
void Copy(char (&out)[N], const char* text) {
    snprintf(out, N, "%s", text ? text : "");
}
bool Sync(FILE* file) { return file && fflush(file) == 0 && fsync(fileno(file)) == 0; }
bool ReadJson(const std::string& path, cJSON** json) {
    *json = nullptr;
    for (const auto& candidate : {path, path + ".bak"}) {
        FILE* f = fopen(candidate.c_str(), "rb");
        if (!f)
            continue;
        struct stat st{};
        if (!fstat(fileno(f), &st) && st.st_size > 0 && st.st_size <= 16384) {
            auto buffer = call_record::Buffer(st.st_size + 1, "metadata_read");
            if (buffer && fread(buffer.get(), 1, st.st_size, f) == size_t(st.st_size)) {
                buffer.get()[st.st_size] = 0;
                *json = cJSON_Parse(buffer.get());
            }
        }
        fclose(f);
        if (*json)
            return true;
    }
    return false;
}
const char* String(const cJSON* j, const char* key) {
    const auto* p = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(p) ? p->valuestring : "";
}
double Number(const cJSON* j, const char* key) {
    const auto* p = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsNumber(p) ? p->valuedouble : 0;
}
bool WriteJson(const std::string& path, cJSON* j) {
    if (!j)
        return false;
    char* text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!text)
        return false;
    // Keep the last committed metadata until the replacement has been flushed.
    const auto temporary = path + ".tmp";
    FILE* f = fopen(temporary.c_str(), "wb");
    bool ok = f && fwrite(text, 1, strlen(text), f) == strlen(text) && Sync(f);
    if (f && fclose(f))
        ok = false;
    cJSON_free(text);
    if (ok)
        ok = call_record::CommitFile(temporary.c_str(), path.c_str());
    return ok;
}
cJSON* InfoJson(const CallRecordInfo& r) {
    auto* j = cJSON_CreateObject();
    if (!j)
        return nullptr;
    cJSON_AddNumberToObject(j, "schema", 1);
    cJSON_AddStringToObject(j, "id", r.id);
    cJSON_AddNumberToObject(j, "call_id", r.call_id);
    for (unsigned t = 0; t < 2; ++t) {
        cJSON_AddNumberToObject(j, t ? "assistant_first_media_ms" : "caller_first_media_ms",
                                r.first_media_ms[t]);
        cJSON_AddNumberToObject(j, t ? "assistant_captured_samples" : "caller_captured_samples",
                                r.captured_samples[t]);
        if (r.gap_details_available) {
            cJSON_AddNumberToObject(j, t ? "assistant_gap_samples" : "caller_gap_samples",
                                    r.track_gap_samples[t]);
            cJSON_AddNumberToObject(j, t ? "assistant_gap_events" : "caller_gap_events",
                                    r.track_gap_events[t]);
            cJSON_AddNumberToObject(j, t ? "assistant_max_gap_samples" : "caller_max_gap_samples",
                                    r.track_max_gap_samples[t]);
        }
    }
    cJSON_AddBoolToObject(j, "gap_details_available", r.gap_details_available);
    cJSON_AddStringToObject(j, "caller", r.caller);
    cJSON_AddStringToObject(j, "save", SaveName(r.save));
    cJSON_AddStringToObject(j, "error", r.error);
    cJSON_AddNumberToObject(j, "started_at", r.started_at);
    cJSON_AddNumberToObject(j, "ended_at", r.ended_at);
    cJSON_AddNumberToObject(j, "duration_ms", r.duration_ms);
    cJSON_AddStringToObject(j, "answer_mode", r.answer_mode);
    cJSON_AddBoolToObject(j, "taken_over", r.taken_over);
    cJSON_AddNumberToObject(j, "takeover_ms", r.takeover_ms);
    cJSON_AddBoolToObject(j, "audio_available", r.audio_available);
    cJSON_AddNumberToObject(j, "dropped_frames", r.dropped_frames);
    cJSON_AddNumberToObject(j, "gap_samples", r.gap_samples);
    cJSON_AddNumberToObject(j, "text_drops", r.text_drops);
    cJSON_AddNumberToObject(j, "queue_peak", r.queue_peak);
    cJSON_AddNumberToObject(j, "max_write_ms", r.max_write_ms);
    cJSON_AddNumberToObject(j, "max_sync_ms", r.max_sync_ms);
    cJSON_AddNumberToObject(j, "max_drain_gap_ms", r.max_drain_gap_ms);
    cJSON_AddStringToObject(j, "summary", r.summary);
    cJSON_AddStringToObject(j, "summary_state", r.summary_state);
    cJSON_AddStringToObject(j, "transcription_state", r.transcription_state);
    return j;
}
bool ReadInfo(const std::string& path, CallRecordInfo& r) {
    cJSON* j = nullptr;
    if (!ReadJson(path, &j))
        return false;
    r = {};
    Copy(r.id, String(j, "id"));
    r.call_id = Number(j, "call_id");
    for (unsigned t = 0; t < 2; ++t) {
        r.first_media_ms[t] = Number(j, t ? "assistant_first_media_ms" : "caller_first_media_ms");
        r.captured_samples[t] =
            Number(j, t ? "assistant_captured_samples" : "caller_captured_samples");
        r.track_gap_samples[t] = Number(j, t ? "assistant_gap_samples" : "caller_gap_samples");
        r.track_gap_events[t] = Number(j, t ? "assistant_gap_events" : "caller_gap_events");
        r.track_max_gap_samples[t] =
            Number(j, t ? "assistant_max_gap_samples" : "caller_max_gap_samples");
    }
    r.gap_details_available = cJSON_IsTrue(cJSON_GetObjectItem(j, "gap_details_available"));
    Copy(r.caller, String(j, "caller"));
    Copy(r.error, String(j, "error"));
    Copy(r.summary, String(j, "summary"));
    Copy(r.summary_state, String(j, "summary_state"));
    Copy(r.transcription_state, String(j, "transcription_state"));
    Copy(r.answer_mode, String(j, "answer_mode"));
    r.started_at = Number(j, "started_at");
    r.ended_at = Number(j, "ended_at");
    r.duration_ms = Number(j, "duration_ms");
    r.takeover_ms = Number(j, "takeover_ms");
    r.dropped_frames = Number(j, "dropped_frames");
    r.gap_samples = Number(j, "gap_samples");
    r.text_drops = Number(j, "text_drops");
    r.queue_peak = Number(j, "queue_peak");
    r.max_write_ms = Number(j, "max_write_ms");
    r.max_sync_ms = Number(j, "max_sync_ms");
    r.max_drain_gap_ms = Number(j, "max_drain_gap_ms");
    r.taken_over = cJSON_IsTrue(cJSON_GetObjectItem(j, "taken_over"));
    r.audio_available = cJSON_IsTrue(cJSON_GetObjectItem(j, "audio_available"));
    for (unsigned i = 0; i <= unsigned(CallSaveState::Failed); ++i)
        if (!strcmp(String(j, "save"), SaveName(CallSaveState(i))))
            r.save = CallSaveState(i);
    const bool valid = Number(j, "schema") == 1 && call_record::ValidId(r.id);
    cJSON_Delete(j);
    return valid;
}
}  // namespace

struct CallRecords::Impl {
    // Both media taps execute on PhoneBridge::Run, so this is a single-producer,
    // single-consumer ring. Text events have a separate queue and cannot contend
    // with the audio producer. Release/acquire publishes complete PCM frames.
    struct PcmEvent {
        uint64_t position;
        uint32_t token, ms;
        uint16_t samples;
        uint8_t type;
        int16_t pcm[320];
    };
    PcmEvent pcm_queue[kPcmSlots]{};
    std::atomic<uint32_t> pcm_head{0}, pcm_tail{0};
    std::atomic<uint32_t> queue_peak{0}, text_drops{0};
    int64_t last_drain_us = 0;
    call_record::MediaClock media_clock[2];
    struct Event {
        uint64_t position = UINT64_MAX;
        uint32_t token, ms;
        uint16_t samples;
        uint8_t type;  // 0 caller PCM, 1 transmitted PCM, 2/3 final text, 4/5 partial text
        union {
            int16_t pcm[320];
            char text[1536];
        };
    };
    enum class Request { None, Page, Detail, Play, Remount, Delete };
    std::mutex mutex, queue_mutex;
    CallRecordView view;
    Event* queue = nullptr;
    size_t head = 0, count = 0;
    std::atomic<uint32_t> token{0}, dropped{0};
    std::atomic<int64_t> began_us{0};
    std::atomic<bool> playback_stop{false}, playback_pause{false};
    std::atomic<int64_t> playback_seek{-1};
    Request request = Request::None;
    char requested_id[18]{};
    uint32_t requested_offset = 0;
    bool mounted = false, active = false, state_interrupted = false;
    uint32_t call_id = 0, serial = 0, total = 0;
    uint32_t next_token = 0;
    int64_t fresh_us = 0, sync_us = 0;
    std::string base;
    CallRecordInfo current;
    FILE* tracks[2]{};
    FILE* texts = nullptr;
    char file_buffers[2][4096]{};
    uint64_t written[2]{};
    char last_partial[2][1536]{};
    FILE* playback_files[2]{};
    uint64_t playback_samples = 0, playback_position = 0;
    int64_t playback_next_us = 0;
    std::atomic<uint32_t> playback_generation{0};
    Event event{};
    AliyunRealtimeConfig cloud_config;
    std::atomic<bool> post_running{false}, post_cancel{false};
    bool post_ready = false;
    CallRecordCloudResult post_result;
    char post_id[18]{};
    uint32_t post_scan_offset = 0;

    std::string Path(const char* id, const char* file) const {
        return base + "/" + id + "/" + file;
    }
    void Publish() {
        std::lock_guard<std::mutex> lock(mutex);
        view.mounted = mounted;
        view.total = total;
        view.save = current.save;
        ++view.revision;
    }
    void Failure(const char* reason) {
        if (!current.error[0])
            Copy(current.error, reason);
        current.save = CallSaveState::Failed;
        ESP_LOGE(kTag, "write_failed id=%s reason=%s errno=%d", current.id, reason, errno);
        Publish();
    }
    void Space() {
        uint64_t capacity = 0, available = 0;
        if (mounted && esp_vfs_fat_info("/sdcard", &capacity, &available) != ESP_OK) {
            std::lock_guard<std::mutex> lock(mutex);
            Copy(view.storage_error, "storage_read_failed");
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        view.total_bytes = capacity;
        view.free_bytes = available;
    }
    void DeleteRecord(const char* id) {
        if (!call_record::ValidId(id)) {
            puts("RECORDS_DELETE success=0 error=invalid_id");
            return;
        }
        const char* error = nullptr;
        phone_status_t phone{};
        int64_t age;
        uint32_t duration;
        if (!mounted || active || post_running || playback_files[0] || playback_files[1] ||
            !gateway_manager_get_phone_status(&phone, &age, &duration) ||
            (phone.phase != PHONE_IDLE && phone.phase != PHONE_ENDED) ||
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
            error = "busy_or_unavailable";
        const auto source = base + "/" + id;
        const auto trash = base + "/deleted-" + id;
        struct stat st{};
        bool staged = !error && stat(trash.c_str(), &st) == 0;
        if (!error && staged && !S_ISDIR(st.st_mode))
            error = "invalid_staged_directory";
        if (!error && staged && stat(source.c_str(), &st) == 0)
            error = "staged_conflict";
        if (!error && !staged) {
            CallRecordInfo info;
            if (!ReadInfo(Path(id, "meta.json"), info) || strcmp(info.id, id) ||
                info.save == CallSaveState::Recording || info.save == CallSaveState::Saving)
                error = "not_finalized";
        }
        // Never recurse or follow nested directories. The exact, validated ID
        // names one record; unrelated files and recording directories stay put.
        DIR* dir = error ? nullptr : opendir((staged ? trash : source).c_str());
        if (!error && !dir)
            error = "directory_read_failed";
        if (dir) {
            while (auto* entry = readdir(dir)) {
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                    continue;
                const auto file = (staged ? trash : source) + "/" + entry->d_name;
                if (stat(file.c_str(), &st) || !S_ISREG(st.st_mode)) {
                    error = "unexpected_directory_entry";
                    break;
                }
            }
            closedir(dir);
        }
        // An interrupted deletion stays outside the valid-ID namespace. Mount
        // cannot revive half-deleted audio as a playable call; retry the same ID.
        if (!error && !staged && rename(source.c_str(), trash.c_str()))
            error = "stage_failed";
        if (!error) {
            Recover();
            post_scan_offset = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!strcmp(view.detail.id, id)) {
                    view.detail = {};
                    view.transcript[0] = 0;
                    view.transcript_offset = view.transcript_next = view.transcript_previous = 0;
                    view.transcript_more = false;
                    view.playback_ms = view.playback_duration_ms = 0;
                }
            }
            LoadPage(0);
            dir = opendir(trash.c_str());
            if (!dir)
                error = "staged_read_failed";
            if (dir) {
                while (auto* entry = readdir(dir)) {
                    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                        continue;
                    if (unlink((trash + "/" + entry->d_name).c_str())) {
                        error = "unlink_failed";
                        break;
                    }
                }
                closedir(dir);
            }
            if (!error && rmdir(trash.c_str()))
                error = "directory_remove_failed";
            if (!mounted && !error)
                error = "index_recovery_failed";
            Space();
            Publish();
        }
        printf("RECORDS_DELETE id=%s success=%d error=%s\n", id, !error, error ? error : "none");
    }
    void Mount() {
        if (active || post_running)
            return;
        ClosePlayback();
        if (mounted)
            Board::GetInstance().UnmountCallStorage();
        const char* path = nullptr;
        std::string error;
        mounted = Board::GetInstance().MountCallStorage(&path, error);
        if (mounted) {
            base = std::string(path) + "/calls";
            if (mkdir(base.c_str(), 0775) && errno != EEXIST) {
                error = "calls_directory_failed";
                mounted = false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            Copy(view.storage_error, error.c_str());
        }
        if (mounted) {
            Recover();
            Space();
            current.save = mounted ? CallSaveState::Ready : CallSaveState::Unavailable;
            LoadPage(0);
        } else {
            current.save = CallSaveState::Unavailable;
        }
        Publish();
        ESP_LOGI(kTag, "mount=%d error=%s total=%lu", mounted, error.c_str(), (unsigned long)total);
    }
    bool Header(unsigned track) {
        uint8_t h[44];
        if (!tracks[track] || !call_record::WavHeader(h, written[track]))
            return false;
        return fseek(tracks[track], 0, SEEK_SET) == 0 && fwrite(h, 1, 44, tracks[track]) == 44 &&
               fseek(tracks[track], 0, SEEK_END) == 0 && Sync(tracks[track]);
    }
    void Recover() {
        // Rebuild the small fixed-width index, preserving every recording directory.
        // A power loss can leave metadata, headers or the old index incomplete.
        DIR* dir = opendir(base.c_str());
        FILE* index = fopen((base + "/index.new").c_str(), "wb");
        if (!dir || !index) {
            if (dir)
                closedir(dir);
            if (index)
                fclose(index);
            mounted = false;
            std::lock_guard<std::mutex> lock(mutex);
            Copy(view.storage_error, "index_recovery_failed");
            return;
        }
        // Repeated bounded selection avoids an unbounded list in PSRAM. This runs
        // at mount only, before recording is accepted.
        char previous[18]{};
        total = serial = 0;
        bool ok = true;
        while (ok) {
            char next[18]{};
            rewinddir(dir);
            while (auto* entry = readdir(dir))
                if (call_record::ValidId(entry->d_name) && strcmp(entry->d_name, previous) > 0 &&
                    (!next[0] || strcmp(entry->d_name, next) < 0))
                    Copy(next, entry->d_name);
            if (!next[0])
                break;
            CallRecordInfo r;
            if (!ReadInfo(Path(next, "meta.json"), r)) {
                Copy(r.id, next);
                Copy(r.caller, "unknown");
                Copy(r.summary_state, "failed");
            }
            if (r.save == CallSaveState::Recording || r.save == CallSaveState::Saving ||
                r.save == CallSaveState::Unavailable) {
                r.save = CallSaveState::Incomplete;
                Copy(r.error, "interrupted_save");
                Copy(r.transcription_state, "partial");
                uint64_t largest = 0;
                bool audio = true;
                for (unsigned t = 0; t < 2; ++t) {
                    const auto file = Path(next, t ? "assistant.wav" : "caller.wav");
                    FILE* f = fopen(file.c_str(), "r+b");
                    struct stat st{};
                    if (!f || fstat(fileno(f), &st) || st.st_size < 44) {
                        if (f)
                            fclose(f);
                        audio = false;
                        continue;
                    }
                    const uint64_t n = (st.st_size - 44) / 2;
                    uint8_t h[44];
                    audio &= call_record::WavHeader(h, n) && fseek(f, 0, SEEK_SET) == 0 &&
                             fwrite(h, 1, 44, f) == 44 && Sync(f);
                    fclose(f);
                    largest = std::max(largest, n);
                }
                r.duration_ms = largest / 16;
                r.audio_available = audio && largest > 0;
                ok = WriteJson(Path(next, "meta.json"), InfoJson(r));
            }
            char entry[kIndexSize]{};
            memcpy(entry, next, 17);
            ok &= fwrite(entry, 1, kIndexSize, index) == kIndexSize;
            Copy(previous, next);
            serial = std::max(serial, uint32_t(strtoul(next, nullptr, 16)));
            ++total;
        }
        ok &= Sync(index);
        ok &= fclose(index) == 0;
        closedir(dir);
        if (!ok || !call_record::CommitFile((base + "/index.new").c_str(),
                                            (base + "/index.bin").c_str())) {
            mounted = false;
            std::lock_guard<std::mutex> lock(mutex);
            Copy(view.storage_error, "index_recovery_failed");
        }
    }
    bool Indexed(uint32_t offset, CallRecordInfo& result) {
        if (offset >= total)
            return false;
        FILE* f = fopen((base + "/index.bin").c_str(), "rb");
        char id[kIndexSize]{};
        bool ok = f && fseek(f, (total - offset - 1) * kIndexSize, SEEK_SET) == 0 &&
                  fread(id, 1, sizeof(id), f) == sizeof(id) && call_record::ValidId(id);
        if (f)
            fclose(f);
        if (!ok)
            return false;
        if (!ReadInfo(Path(id, "meta.json"), result)) {
            result = {};
            Copy(result.id, id);
            Copy(result.error, "metadata_read_failed");
            result.save = CallSaveState::Failed;
        }
        return true;
    }
    void LoadPage(uint32_t offset) {
        CallRecordInfo rows[3], recent[2];
        unsigned n = 0;
        bool ok = mounted;
        for (unsigned i = 0; i < 3 && offset + i < total; ++i) {
            ok &= Indexed(offset + i, rows[i]);
            ++n;
        }
        for (unsigned i = 0; i < 2 && i < total; ++i)
            ok &= Indexed(i, recent[i]);
        std::lock_guard<std::mutex> lock(mutex);
        std::copy_n(rows, 3, view.rows);
        std::copy_n(recent, 2, view.recent);
        view.count = n;
        view.offset = offset;
        view.total = total;
        view.loading = false;
        if (!ok && mounted)
            Copy(view.storage_error, "index_read_failed");
        ++view.revision;
    }
    void LoadDetail(const char* id, uint32_t offset) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            changed = strcmp(view.detail.id, id) != 0;
        }
        if (changed)
            ClosePlayback();
        CallRecordInfo r;
        bool ok = call_record::ValidId(id) && ReadInfo(Path(id, "meta.json"), r);
        if (!ok) {
            r = {};
            Copy(r.error, "metadata_read_failed");
        }
        // Build the page outside the display lock; persistent text is JSONL.
        char* text =
            static_cast<char*>(heap_caps_calloc(1, 12288, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!text)
            call_record::AllocationFailure("transcript_page", 12288,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        FILE* f =
            ok ? fopen(Path(id, !strcmp(r.transcription_state, "complete") ||
                                        !strcmp(r.transcription_state, "complete_with_audio_gaps")
                                    ? "transcript.jsonl"
                                    : "live-transcript.jsonl")
                           .c_str(),
                       "rb")
               : nullptr;
        size_t used = 0;
        uint32_t line = 0, shown = 0;
        size_t preceding_used = 0;
        uint32_t preceding_start = 0, preceding_shown = 0;
        bool more = false;
        auto buffer = call_record::Buffer(call_record::kJsonLineBytes, "transcript_line");
        bool invalid = false;
        if (text && f && buffer) {
            while (call_record::ReadLine(f, buffer.get(), call_record::kJsonLineBytes, invalid)) {
                cJSON* j = cJSON_Parse(buffer.get());
                if (!j) {
                    Copy(r.error, "transcript_read_failed");
                    break;
                }
                if (!cJSON_IsTrue(cJSON_GetObjectItem(j, "final"))) {
                    cJSON_Delete(j);
                    continue;
                }
                const uint32_t index = line++;
                const unsigned ms = Number(j, "ms");
                const char* role = String(j, "speaker");
                const char* speaker = !strcmp(role, "caller") ? "来电人"
                                      : !strcmp(role, "me")   ? "我"
                                                              : "AI";
                const char* sentence = String(j, "text");
                if (index < offset) {
                    // Recreate earlier page boundaries while doing the existing
                    // streaming scan. Long sentences can fill a page before its
                    // eight-row limit; subtracting one row or eight rows cannot
                    // recover the previous page reliably.
                    if (preceding_shown >= 8 || preceding_used + strlen(sentence) + 100 >= 12288) {
                        preceding_start = index;
                        preceding_used = 0;
                        preceding_shown = 0;
                    }
                    preceding_used += snprintf(nullptr, 0, "%02u:%02u %s\n%s\n\n", ms / 60000,
                                               ms / 1000 % 60, speaker, sentence);
                    ++preceding_shown;
                } else {
                    if (shown >= 8 || used + strlen(String(j, "text")) + 100 >= 12288) {
                        more = true;
                        cJSON_Delete(j);
                        break;
                    }
                    used += snprintf(text + used, 12288 - used, "%02u:%02u %s\n%s\n\n", ms / 60000,
                                     ms / 1000 % 60, speaker, String(j, "text"));
                    ++shown;
                }
                cJSON_Delete(j);
            }
            if (ferror(f) || invalid)
                Copy(r.error, "transcript_read_failed");
        } else {
            Copy(r.error, !text || !buffer ? "transcript_memory" : "transcript_missing");
        }
        if (f)
            fclose(f);
        {
            std::lock_guard<std::mutex> lock(mutex);
            view.detail = r;
            if (changed) {
                view.playback_ms = 0;
                view.playback_duration_ms = r.duration_ms;
            }
            Copy(view.transcript, text ? text : "");
            view.transcript_offset = offset;
            view.transcript_next = offset + shown;
            view.transcript_previous = preceding_start;
            view.transcript_more = more;
            view.loading = false;
            ++view.revision;
        }
        heap_caps_free(text);
    }
    void Begin(const phone_status_t& p, uint32_t duration) {
        ClosePlayback();
        current = {};
        current.gap_details_available = true;
        written[0] = written[1] = 0;
        std::memset(last_partial, 0, sizeof(last_partial));
        dropped = 0;
        text_drops = 0;
        queue_peak = 0;
        last_drain_us = 0;
        call_id = p.call_id;
        current.call_id = p.call_id;
        active = true;
        state_interrupted = false;
        began_us = esp_timer_get_time() - int64_t(duration) * 1000;
        fresh_us = sync_us = esp_timer_get_time();
        Copy(current.caller, p.caller_length ? p.caller : "unknown");
        Copy(current.answer_mode, "ai");
        Copy(current.summary_state, "pending");
        Copy(current.transcription_state, "partial_live");
        const time_t now = time(nullptr);
        current.started_at = now >= 1704067200 ? now - duration / 1000 : 0;
        current.save = CallSaveState::Recording;
        snprintf(current.id, sizeof(current.id), "%08lx-%08lx", (unsigned long)++serial,
                 (unsigned long)esp_random());
        if (!mounted) {
            Failure("storage_unavailable");
            return;
        }
        Space();
        if (view.free_bytes < kReserveBytes) {
            Failure("storage_full");
            return;
        }
        if (mkdir((base + "/" + current.id).c_str(), 0775)) {
            Failure("record_directory_failed");
            return;
        }
        for (unsigned t = 0; t < 2; ++t) {
            tracks[t] = fopen(Path(current.id, t ? "assistant.wav" : "caller.wav").c_str(), "w+b");
            if (tracks[t]) {
                setvbuf(tracks[t], file_buffers[t], _IOFBF, sizeof(file_buffers[t]));
                uint8_t h[44];
                call_record::WavHeader(h, 0);
                if (fwrite(h, 1, 44, tracks[t]) != 44)
                    Failure("audio_create_failed");
            } else {
                Failure("audio_create_failed");
            }
        }
        texts = fopen(Path(current.id, "live-transcript.jsonl").c_str(), "ab");
        if (!texts || !WriteJson(Path(current.id, "meta.json"), InfoJson(current)))
            Failure("metadata_create_failed");
        FILE* f = fopen((base + "/index.bin").c_str(), "ab");
        const bool indexed = f && fwrite(current.id, 1, kIndexSize, f) == kIndexSize && Sync(f);
        if (f)
            fclose(f);
        if (indexed)
            ++total;
        else
            Failure("index_write_failed");
        if (!++next_token)
            ++next_token;
        token.store(next_token);
        Publish();
        ESP_LOGI(kTag, "begin id=%s call_id=%lu state=%s", current.id, (unsigned long)call_id,
                 SaveName(current.save));
    }
    bool Pad(unsigned t, uint64_t until) {
        if (!tracks[t] || until > call_record::kMaxSamples)
            return false;
        int16_t silence[320]{};
        while (written[t] < until) {
            const size_t n = std::min<uint64_t>(320, until - written[t]);
            if (fwrite(silence, 2, n, tracks[t]) != n)
                return false;
            written[t] += n;
        }
        return true;
    }
    void Consume(const Event& e, uint32_t finishing_token = 0) {
        if (!active || e.token != (finishing_token ? finishing_token : token.load()))
            return;
        if (e.type < 2) {
            if (current.save == CallSaveState::Failed)
                return;
            const uint64_t start = e.position == UINT64_MAX
                                       ? call_record::FramePosition(written[e.type], e.ms)
                                       : std::max(written[e.type], e.position);
            if (!current.captured_samples[e.type])
                current.first_media_ms[e.type] = e.ms;
            else if (start > written[e.type]) {
                const uint32_t missing = start - written[e.type];
                current.gap_samples += missing;
                current.track_gap_samples[e.type] += missing;
                ++current.track_gap_events[e.type];
                current.track_max_gap_samples[e.type] =
                    std::max(current.track_max_gap_samples[e.type], missing);
            }
            const auto write_begin = esp_timer_get_time();
            const bool ok = Pad(e.type, start) && start + e.samples <= call_record::kMaxSamples &&
                            fwrite(e.pcm, 2, e.samples, tracks[e.type]) == e.samples;
            current.max_write_ms = std::max<uint32_t>(current.max_write_ms,
                                                      (esp_timer_get_time() - write_begin) / 1000);
            if (!ok) {
                Failure("audio_write_failed");
                return;
            }
            written[e.type] += e.samples;
            current.captured_samples[e.type] += e.samples;
        } else {
            if (!texts)
                return;
            auto* j = cJSON_CreateObject();
            if (!j) {
                Failure("transcript_memory");
                return;
            }
            const bool user = e.type == 2 || e.type == 4;
            cJSON_AddNumberToObject(j, "ms", e.ms);
            cJSON_AddStringToObject(j, "speaker", user ? "caller" : "ai");
            cJSON_AddBoolToObject(j, "final", e.type < 4);
            cJSON_AddStringToObject(j, "text", e.text);
            char* line = cJSON_PrintUnformatted(j);
            cJSON_Delete(j);
            if (!line || fprintf(texts, "%s\n", line) < 0)
                Failure("transcript_write_failed");
            cJSON_free(line);
        }
    }
    void Drain() {
        DrainAudio();
        for (size_t i = 0; i < kTextSlots; ++i) {
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (!count)
                    break;
                event = queue[head];
                head = (head + 1) % kTextSlots;
                --count;
            }
            Consume(event);
        }
    }
    void DrainAudio(uint32_t finishing_token = 0) {
        const auto now = esp_timer_get_time();
        if (active && last_drain_us)
            current.max_drain_gap_ms =
                std::max<uint32_t>(current.max_drain_gap_ms, (now - last_drain_us) / 1000);
        last_drain_us = now;
        uint32_t tail = pcm_tail.load(std::memory_order_relaxed);
        for (size_t n = 0; n < kPcmSlots && tail != pcm_head.load(std::memory_order_acquire); ++n) {
            const auto& frame = pcm_queue[tail % kPcmSlots];
            event.position = frame.position;
            event.token = frame.token;
            event.ms = frame.ms;
            event.type = frame.type;
            event.samples = frame.samples;
            std::copy_n(frame.pcm, frame.samples, event.pcm);
            pcm_tail.store(++tail, std::memory_order_release);
            Consume(event, finishing_token);
        }
    }
    void Flush() {
        const auto sync_begin = esp_timer_get_time();
        current.duration_ms = std::max<int64_t>(0, (esp_timer_get_time() - began_us) / 1000);
        current.dropped_frames = dropped.load();
        current.text_drops = text_drops.load();
        current.queue_peak = queue_peak.load();
        if (current.save != CallSaveState::Failed && (!Header(0) || !Header(1) || !Sync(texts)))
            Failure("storage_sync_failed");
        if (!WriteJson(Path(current.id, "meta.json"), InfoJson(current)))
            Failure("metadata_write_failed");
        Space();
        if (view.free_bytes < kReserveBytes && current.save != CallSaveState::Failed)
            Failure("storage_full");
        current.max_sync_ms =
            std::max<uint32_t>(current.max_sync_ms, (esp_timer_get_time() - sync_begin) / 1000);
        ESP_LOGI(kTag,
                 "storage id=%s queued=%lu peak=%lu/%u drops=%lu text_drops=%lu "
                 "write_max_ms=%lu sync_max_ms=%lu drain_gap_max_ms=%lu",
                 current.id, (unsigned long)(pcm_head.load() - pcm_tail.load()),
                 (unsigned long)queue_peak.load(), unsigned(kPcmSlots),
                 (unsigned long)dropped.load(), (unsigned long)text_drops.load(),
                 (unsigned long)current.max_write_ms, (unsigned long)current.max_sync_ms,
                 (unsigned long)current.max_drain_gap_ms);
        Publish();
    }
    void Finish(uint32_t duration, bool interrupted) {
        interrupted |= state_interrupted;
        // Close the producer gate before draining; queued audio still belongs to
        // this call until Drain completes. No next-call data can enter these files.
        const auto old = token.exchange(0);
        DrainFinished(old);
        current.duration_ms = duration;
        current.ended_at = current.started_at ? current.started_at + duration / 1000 : 0;
        const bool failed = current.save == CallSaveState::Failed;
        current.save = failed ? CallSaveState::Failed : CallSaveState::Saving;
        Publish();
        bool ok = !failed;
        if (ok) {
            const uint64_t target =
                std::max<uint64_t>(uint64_t(duration) * 16, std::max(written[0], written[1]));
            ok = Pad(0, target) && Pad(1, target) && Header(0) && Header(1) && Sync(texts);
        }
        for (auto& f : tracks)
            if (f) {
                ok &= fclose(f) == 0;
                f = nullptr;
            }
        if (texts) {
            ok &= fclose(texts) == 0;
            texts = nullptr;
        }
        current.dropped_frames = dropped.load();
        current.text_drops = text_drops.load();
        current.queue_peak = queue_peak.load();
        current.audio_available = ok && written[0] && written[1];
        if (!ok)
            current.save = CallSaveState::Failed;
        else if (interrupted || current.dropped_frames || current.text_drops ||
                 current.gap_samples > 3200 || !current.captured_samples[0] ||
                 !current.captured_samples[1]) {
            current.save = CallSaveState::Incomplete;
            if (!current.error[0])
                Copy(current.error, interrupted ? "call_state_lost"
                                    : !current.captured_samples[0] || !current.captured_samples[1]
                                        ? "missing_media"
                                    : current.text_drops ? "transcript_drops"
                                                         : "audio_gaps");
        } else
            current.save = CallSaveState::Saved;
        if (!ok && !current.error[0])
            Copy(current.error, "save_failed");
        if (!WriteJson(Path(current.id, "meta.json"), InfoJson(current)))
            Failure("metadata_commit_failed");
        active = false;
        post_scan_offset = 0;
        Publish();
        LoadPage(0);
        ESP_LOGI(kTag, "end id=%s save=%s duration_ms=%lu drops=%lu gaps=%lu", current.id,
                 SaveName(current.save), (unsigned long)duration,
                 (unsigned long)current.dropped_frames, (unsigned long)current.gap_samples);
    }
    void DrainFinished(uint32_t old) {
        DrainAudio(old);
        // The worker is the sole consumer; retain the old identity only locally.
        while (true) {
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (!count)
                    break;
                event = queue[head];
                head = (head + 1) % kTextSlots;
                --count;
            }
            Consume(event, old);
        }
    }
    void Observe() {
        phone_status_t p{};
        int64_t age = 0;
        uint32_t duration = 0;
        const bool fresh = gateway_manager_get_phone_status(&p, &age, &duration);
        const auto now = esp_timer_get_time();
        if (fresh) {
            fresh_us = now;
            if (p.phase == PHONE_INCOMING || p.phase == PHONE_OUTGOING || p.phase == PHONE_ACTIVE) {
                post_cancel = true;
                ClosePlayback();
            }
            if (active && (p.call_id != call_id || p.phase == PHONE_ENDED || p.phase == PHONE_IDLE))
                Finish(p.call_id == call_id ? duration : current.duration_ms, false);
            if (!active && p.phase == PHONE_ACTIVE)
                Begin(p, duration);
        } else if (active) {
            if (!state_interrupted) {
                state_interrupted = true;
                ESP_LOGI(kTag, "control_interrupted id=%s call_id=%lu grace_ms=%lu", current.id,
                         (unsigned long)call_id, (unsigned long)(kRecordStateGraceUs / 1000));
            }
            if (now - fresh_us > kRecordStateGraceUs)
                Finish((now - began_us) / 1000, true);
        }
        if (active && now - sync_us >= 2000000) {
            sync_us = now;
            Flush();
        }
    }
    void Postprocess() {
        CallRecordCloudResult result;
        char id[18]{};
        bool ready = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (post_ready) {
                post_ready = false;
                ready = true;
                result = std::move(post_result);
                Copy(id, post_id);
            }
        }
        if (ready) {
            CallRecordInfo record;
            if (ReadInfo(Path(id, "meta.json"), record)) {
                Copy(record.transcription_state,
                     result.transcript_complete
                         ? (record.save == CallSaveState::Saved ? "complete"
                                                                : "complete_with_audio_gaps")
                     : result.canceled ? "pending"
                                       : "partial_asr_failed");
                Copy(record.summary_state, result.summary_complete ? "complete"
                                           : result.canceled       ? "pending"
                                                                   : "failed");
                if (result.summary_complete)
                    Copy(record.summary, result.summary.c_str());
                if (!result.error.empty() && !result.canceled && !record.error[0])
                    Copy(record.error, result.error.c_str());
                if (!WriteJson(Path(id, "meta.json"), InfoJson(record)))
                    ESP_LOGE(kTag, "postprocess metadata_commit_failed id=%s", id);
                uint32_t offset, transcript_offset;
                bool showing;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    offset = view.offset;
                    transcript_offset = view.transcript_offset;
                    showing = !strcmp(view.detail.id, id);
                }
                LoadPage(offset);
                if (showing)
                    LoadDetail(id, transcript_offset);
                ESP_LOGI(kTag, "postprocess id=%s transcript=%s summary=%s error=%s", id,
                         record.transcription_state, record.summary_state, result.error.c_str());
            }
            if (result.canceled)
                post_scan_offset = 0;
            post_running = false;
        }
        if (post_running || active || !mounted)
            return;
        phone_status_t p{};
        int64_t age;
        uint32_t duration;
        if (!gateway_manager_get_phone_status(&p, &age, &duration) ||
            (p.phase != PHONE_IDLE && p.phase != PHONE_ENDED) ||
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
            return;
        if (post_scan_offset >= total)
            return;
        CallRecordInfo record;
        if (!Indexed(post_scan_offset++, record) || !record.audio_available ||
            strcmp(record.summary_state, "pending"))
            return;
        AliyunRealtimeConfig config;
        {
            std::lock_guard<std::mutex> lock(mutex);
            config = cloud_config;
            Copy(post_id, record.id);
        }
        if (!config.IsReady()) {
            --post_scan_offset;
            return;
        }
        post_cancel = false;
        post_running = true;
        const auto directory = base + "/" + record.id;
        if (!StartBackgroundWork("call_cloud", 28672, 1, [this, directory, config] {
                auto result =
                    ProcessCallRecording(directory, config, [this] { return post_cancel.load(); });
                std::lock_guard<std::mutex> lock(mutex);
                post_result = std::move(result);
                post_ready = true;
            })) {
            post_running = false;
            Copy(record.summary_state, "failed");
            Copy(record.error, "postprocess_worker_memory");
            WriteJson(Path(record.id, "meta.json"), InfoJson(record));
        }
    }
    void ClosePlayback() {
        const auto generation = ++playback_generation;
        bool had = false;
        for (auto& file : playback_files)
            if (file) {
                fclose(file);
                file = nullptr;
                had = true;
            }
        playback_stop = false;
        playback_pause = false;
        if (had)
            Application::GetInstance().Schedule([this, generation] {
                phone_status_t phone{};
                int64_t age;
                uint32_t duration;
                const bool calling = gateway_manager_get_phone_status(&phone, &age, &duration) &&
                                     (phone.phase == PHONE_INCOMING ||
                                      phone.phase == PHONE_OUTGOING || phone.phase == PHONE_ACTIVE);
                if (generation != playback_generation || calling ||
                    Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
                    return;
                auto& audio = Application::GetInstance().GetAudioService();
                audio.ResetDecoder();
                audio.EnableWakeWordDetection(true);
            });
        std::lock_guard<std::mutex> lock(mutex);
        if (view.playback != CallPlaybackState::Failed)
            view.playback = CallPlaybackState::Stopped;
    }
    void OpenPlayback(const char* id) {
        ClosePlayback();
        phone_status_t phone{};
        int64_t age;
        uint32_t duration;
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
            (gateway_manager_get_phone_status(&phone, &age, &duration) &&
             (phone.phase == PHONE_INCOMING || phone.phase == PHONE_OUTGOING ||
              phone.phase == PHONE_ACTIVE)))
            return;
        CallRecordInfo info;
        bool ok = !active && call_record::ValidId(id) && ReadInfo(Path(id, "meta.json"), info) &&
                  info.audio_available;
        uint64_t samples = 0;
        for (unsigned t = 0; t < 2 && ok; ++t) {
            playback_files[t] = fopen(Path(id, t ? "assistant.wav" : "caller.wav").c_str(), "rb");
            uint8_t header[44];
            struct stat st{};
            FILE* f = playback_files[t];
            ok = f && fstat(fileno(f), &st) == 0 && fread(header, 1, 44, f) == 44 &&
                 call_record::ValidWav(header, st.st_size);
            if (ok)
                samples = std::max<uint64_t>(samples, call_record::Get32(header + 40) / 2);
        }
        if (!ok) {
            ClosePlayback();
            std::lock_guard<std::mutex> lock(mutex);
            view.playback = CallPlaybackState::Failed;
            Copy(view.detail.error, "audio_missing_or_read_failed");
            ++view.revision;
            return;
        }
        playback_samples = samples;
        playback_position = 0;
        playback_next_us = esp_timer_get_time() + 100000;
        playback_seek = -1;
        Application::GetInstance().Schedule([] {
            auto& audio = Application::GetInstance().GetAudioService();
            audio.EnableWakeWordDetection(false);
            audio.ResetDecoder();
        });
        std::lock_guard<std::mutex> lock(mutex);
        view.playback = CallPlaybackState::Playing;
        view.playback_ms = 0;
        view.playback_duration_ms = samples / 16;
        ++view.revision;
    }
    void Playback() {
        if (playback_stop.exchange(false))
            ClosePlayback();
        if (!playback_files[0] || !playback_files[1])
            return;
        const int64_t seek = playback_seek.exchange(-1);
        if (seek >= 0) {
            playback_position = std::min<uint64_t>(seek * 16, playback_samples);
            for (auto* f : playback_files)
                if (fseek(f, 44 + playback_position * 2, SEEK_SET)) {
                    ClosePlayback();
                    std::lock_guard<std::mutex> lock(mutex);
                    view.playback = CallPlaybackState::Failed;
                    Copy(view.detail.error, "audio_seek_failed");
                    return;
                }
            Application::GetInstance().Schedule(
                [] { Application::GetInstance().GetAudioService().ResetDecoder(); });
            playback_next_us = esp_timer_get_time();
            std::lock_guard<std::mutex> lock(mutex);
            view.playback_ms = playback_position / 16;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            view.playback = playback_pause ? CallPlaybackState::Paused : CallPlaybackState::Playing;
        }
        const auto now = esp_timer_get_time();
        if (playback_pause || now < playback_next_us)
            return;
        if (playback_position >= playback_samples) {
            if (Application::GetInstance().GetAudioService().IsPlaybackIdle())
                ClosePlayback();
            return;
        }
        int16_t a[320]{}, b[320]{};
        const size_t n = std::min<uint64_t>(320, playback_samples - playback_position);
        bool ok = true;
        for (unsigned t = 0; t < 2; ++t) {
            const size_t got = fread(t ? b : a, 2, n, playback_files[t]);
            ok &= got == n;
        }
        if (!ok) {
            ClosePlayback();
            std::lock_guard<std::mutex> lock(mutex);
            view.playback = CallPlaybackState::Failed;
            Copy(view.detail.error, "audio_read_failed");
            return;
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->pcm = true;
        packet->sample_rate = 16000;
        packet->frame_duration = 20;
        packet->payload.resize(n * 2);
        auto* out = reinterpret_cast<int16_t*>(packet->payload.data());
        for (size_t i = 0; i < n; ++i)
            out[i] = call_record::Mix(a[i], b[i]);
        if (!Application::GetInstance().GetAudioService().PushPacketToDecodeQueue(
                std::move(packet))) {
            for (auto* f : playback_files)
                fseek(f, -long(n * 2), SEEK_CUR);
            return;
        }
        playback_position += n;
        playback_next_us = std::max(playback_next_us + 20000, now + 10000);
        std::lock_guard<std::mutex> lock(mutex);
        view.playback_ms = playback_position / 16;
    }
    void Run() {
        Mount();
        while (true) {
            Observe();
            Drain();
            Request r;
            char id[18];
            uint32_t offset;
            {
                std::lock_guard<std::mutex> lock(mutex);
                r = request;
                request = Request::None;
                Copy(id, requested_id);
                offset = requested_offset;
            }
            if (r == Request::Page)
                LoadPage(offset);
            else if (r == Request::Detail)
                LoadDetail(id, offset);
            else if (r == Request::Play)
                OpenPlayback(id);
            else if (r == Request::Remount)
                Mount();
            else if (r == Request::Delete)
                DeleteRecord(id);
            Playback();
            Postprocess();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
};
#endif

CallRecords& CallRecords::Instance() {
    static CallRecords instance;
    return instance;
}
void CallRecords::Start() {
#if CONFIG_PHONE_RECORDS
    if (impl_)
        return;
    void* memory = heap_caps_calloc(1, sizeof(Impl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) {
        call_record::AllocationFailure("store", sizeof(Impl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return;
    }
    impl_ = new (memory) Impl;
    impl_->queue = static_cast<Impl::Event*>(
        heap_caps_calloc(kTextSlots, sizeof(Impl::Event), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!impl_->queue || !StartBackgroundWork("call_sd", 24576, 2, [this] { impl_->Run(); })) {
        Copy(impl_->view.storage_error, "storage_worker_memory");
        ESP_LOGE(kTag, "worker unavailable queue_bytes=%u",
                 unsigned(kTextSlots * sizeof(Impl::Event)));
        if (!impl_->queue)
            call_record::AllocationFailure("record_queue", kTextSlots * sizeof(Impl::Event),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
#endif
}
void CallRecords::ConfigureCloud(const AliyunRealtimeConfig& config) {
#if CONFIG_PHONE_RECORDS
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->cloud_config = config;
    }
#endif
}
void CallRecords::Capture(bool outgoing, const int16_t* samples, size_t count, unsigned rate,
                          uint32_t sequence, uint32_t generation) {
#if CONFIG_PHONE_RECORDS
    auto* p = impl_;
    if (!p || !samples || !count || (rate != 8000 && rate != 16000))
        return;
    const auto token = p->token.load();
    if (!token)
        return;
    const size_t n = count * 16000 / rate;
    if (n > 320)
        return;
    const uint32_t head = p->pcm_head.load(std::memory_order_relaxed);
    const auto depth = head - p->pcm_tail.load(std::memory_order_acquire);
    if (depth >= kPcmSlots) {
        ++p->dropped;
        return;
    }
    auto& e = p->pcm_queue[head % kPcmSlots];
    e.token = token;
    e.ms = std::max<int64_t>(0, (esp_timer_get_time() - p->began_us) / 1000);
    e.type = outgoing ? 1 : 0;
    e.position = generation ? p->media_clock[e.type].Position(token, generation, sequence, e.ms)
                            : UINT64_MAX;
    e.samples = n;
    for (size_t i = 0; i < n; ++i)
        e.pcm[i] = samples[i * rate / 16000];
    if (token == p->token.load()) {
        p->pcm_head.store(head + 1, std::memory_order_release);
        // The PCM producer is unique; only the worker resets this between calls.
        if (depth + 1 > p->queue_peak.load(std::memory_order_relaxed))
            p->queue_peak.store(depth + 1, std::memory_order_relaxed);
    }
#endif
}
void CallRecords::Transcript(bool user, const char* text, bool final) {
#if CONFIG_PHONE_RECORDS
    auto* p = impl_;
    if (!p || !text || !*text || !p->token.load())
        return;
    std::lock_guard<std::mutex> lock(p->queue_mutex);
    if (!p->token.load() || p->count >= kTextSlots || strlen(text) >= sizeof(Impl::Event::text)) {
        ++p->text_drops;
        return;
    }
    auto& e = p->queue[(p->head + p->count++) % kTextSlots];
    e.token = p->token.load();
    e.ms = std::max<int64_t>(0, (esp_timer_get_time() - p->began_us) / 1000);
    e.type = final ? (user ? 2 : 3) : (user ? 4 : 5);
    Copy(e.text, text);
#endif
}
void CallRecords::Snapshot(CallRecordView& view) {
#if CONFIG_PHONE_RECORDS
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        view = impl_->view;
        return;
    }
#endif
    view = {};
}
void CallRecords::RequestPage(uint32_t offset) {
#if CONFIG_PHONE_RECORDS
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->request = Impl::Request::Page;
        impl_->requested_offset = offset;
        impl_->view.loading = true;
    }
#endif
}
void CallRecords::RequestDetail(const char* id, uint32_t offset) {
#if CONFIG_PHONE_RECORDS
    if (impl_ && call_record::ValidId(id)) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->request = Impl::Request::Detail;
        Copy(impl_->requested_id, id);
        impl_->requested_offset = offset;
        if (strcmp(impl_->view.detail.id, id)) {
            impl_->playback_stop = true;
            impl_->view.detail = {};
            impl_->view.transcript[0] = 0;
            impl_->view.playback_ms = 0;
            impl_->view.playback_duration_ms = 0;
        }
        impl_->view.loading = true;
    }
#endif
}
void CallRecords::Play(const char* id) {
#if CONFIG_PHONE_RECORDS
    if (impl_ && call_record::ValidId(id)) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->request = Impl::Request::Play;
        Copy(impl_->requested_id, id);
    }
#endif
}
void CallRecords::Pause() {
#if CONFIG_PHONE_RECORDS
    if (impl_) {
        impl_->playback_pause = !impl_->playback_pause.load();
        if (impl_->playback_pause)
            Application::GetInstance().Schedule(
                [] { Application::GetInstance().GetAudioService().ResetDecoder(); });
    }
#endif
}
void CallRecords::Seek(uint32_t ms) {
#if CONFIG_PHONE_RECORDS
    if (impl_)
        impl_->playback_seek = ms;
#endif
}
void CallRecords::StopPlayback() {
#if CONFIG_PHONE_RECORDS
    if (impl_)
        impl_->playback_stop = true;
#endif
}
void CallRecords::Remount() {
#if CONFIG_PHONE_RECORDS
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->request = Impl::Request::Remount;
    }
#endif
}
bool CallRecords::Command(const char* line) {
#if CONFIG_PHONE_RECORDS
    if (!strncmp(line, "records delete ", 15)) {
        const char* id = line + 15;
        if (!impl_ || !call_record::ValidId(id)) {
            puts("RECORDS_DELETE success=0 error=invalid_id");
            return true;
        }
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->cloud_config.debug_enabled || impl_->request != Impl::Request::None) {
            puts("RECORDS_DELETE success=0 error=debug_disabled_or_busy");
            return true;
        }
        impl_->request = Impl::Request::Delete;
        Copy(impl_->requested_id, id);
        puts("RECORDS_DELETE queued=1");
        return true;
    }
    if (!strcmp(line, "records status") || !strcmp(line, "records list")) {
        if (!impl_)
            return true;
        auto* state = static_cast<CallRecordView*>(
            heap_caps_malloc(sizeof(CallRecordView), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!state) {
            call_record::AllocationFailure("record_status", sizeof(CallRecordView),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            return true;
        }
        Snapshot(*state);
        auto* j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "mounted", state->mounted);
        cJSON_AddNumberToObject(j, "total", state->total);
        cJSON_AddNumberToObject(j, "free_bytes", double(state->free_bytes));
        cJSON_AddNumberToObject(j, "total_bytes", double(state->total_bytes));
        cJSON_AddStringToObject(j, "error", state->storage_error);
        cJSON_AddStringToObject(j, "save", SaveName(state->save));
        cJSON_AddNumberToObject(j, "playback", unsigned(state->playback));
        cJSON_AddNumberToObject(j, "position_ms", state->playback_ms);
        cJSON_AddNumberToObject(j, "playback_duration_ms", state->playback_duration_ms);
        cJSON_AddBoolToObject(j, "loading", state->loading);
        cJSON_AddNumberToObject(j, "offset", state->offset);
        cJSON_AddNumberToObject(j, "transcript_offset", state->transcript_offset);
        cJSON_AddNumberToObject(j, "transcript_next", state->transcript_next);
        cJSON_AddNumberToObject(j, "transcript_previous", state->transcript_previous);
        cJSON_AddBoolToObject(j, "transcript_more", state->transcript_more);
        if (state->detail.id[0])
            cJSON_AddItemToObject(j, "detail", InfoJson(state->detail));
        auto* rows = cJSON_AddArrayToObject(j, "records");
        for (unsigned i = 0; i < state->count; ++i)
            cJSON_AddItemToArray(rows, InfoJson(state->rows[i]));
        char* text = cJSON_PrintUnformatted(j);
        if (text) {
            printf("RECORDS %s\n", text);
            cJSON_free(text);
        }
        cJSON_Delete(j);
        heap_caps_free(state);
        return true;
    }
    if (!strcmp(line, "records remount")) {
        Remount();
        return true;
    }
    if (!strcmp(line, "records stop")) {
        StopPlayback();
        return true;
    }
#endif
    return false;
}
bool CallRecords::ReadFile(const char* id, const char* name, uint64_t offset, void* buffer,
                           size_t capacity, size_t& bytes, uint64_t& total) {
    bytes = 0;
    total = 0;
#if CONFIG_PHONE_RECORDS
    if (!impl_ || !call_record::ValidId(id) || !name || !buffer || capacity > 8192)
        return false;
    const char* allowed[] = {"meta.json", "caller.wav", "assistant.wav", "transcript.jsonl",
                             "live-transcript.jsonl"};
    bool match = false;
    for (const char* n : allowed)
        match |= !strcmp(n, name);
    if (!match)
        return false;
    CallRecordInfo r;
    if (!ReadInfo(impl_->Path(id, "meta.json"), r) || r.save == CallSaveState::Recording ||
        r.save == CallSaveState::Saving)
        return false;
    FILE* f = fopen(impl_->Path(id, name).c_str(), "rb");
    struct stat st{};
    if (!f)
        return false;
    bool ok = fstat(fileno(f), &st) == 0 && st.st_size >= 0 && offset <= uint64_t(st.st_size) &&
              offset <= LONG_MAX && fseek(f, offset, SEEK_SET) == 0;
    if (ok) {
        total = st.st_size;
        bytes = fread(buffer, 1, std::min<uint64_t>(capacity, total - offset), f);
        ok &= !ferror(f);
    }
    fclose(f);
    return ok;
#else
    return false;
#endif
}

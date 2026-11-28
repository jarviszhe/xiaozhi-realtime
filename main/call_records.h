#pragma once

#include <cstddef>
#include <cstdint>
struct AliyunRealtimeConfig;

enum class CallSaveState : uint8_t {
    Unavailable,
    Ready,
    Recording,
    Saving,
    Saved,
    Incomplete,
    Failed
};
enum class CallPlaybackState : uint8_t { Stopped, Playing, Paused, Failed };

struct CallRecordInfo {
    char id[18]{};
    char caller[80]{};
    char summary[768]{};
    char error[80]{};
    char summary_state[24]{};
    char transcription_state[32]{};
    char answer_mode[24]{};
    int64_t started_at = 0, ended_at = 0;
    uint32_t duration_ms = 0, takeover_ms = 0;
    uint32_t dropped_frames = 0, gap_samples = 0;
    uint32_t text_drops = 0, queue_peak = 0;
    uint32_t max_write_ms = 0, max_sync_ms = 0, max_drain_gap_ms = 0;
    uint32_t call_id = 0;
    uint32_t first_media_ms[2]{}, captured_samples[2]{};
    // Excludes the initial media wait and final padding, like gap_samples.
    // Old records only have the combined total; do not interpret absent detail as zero loss.
    uint32_t track_gap_samples[2]{}, track_gap_events[2]{}, track_max_gap_samples[2]{};
    bool gap_details_available = false;
    CallSaveState save = CallSaveState::Unavailable;
    bool taken_over = false, audio_available = false;
};

struct CallRecordView {
    uint32_t revision = 0, total = 0, offset = 0, count = 0, transcript_offset = 0;
    uint32_t transcript_next = 0, transcript_previous = 0;
    uint64_t free_bytes = 0, total_bytes = 0;
    bool mounted = false, loading = false, transcript_more = false;
    char storage_error[80]{};
    CallSaveState save = CallSaveState::Unavailable;
    CallRecordInfo rows[3]{}, recent[2]{}, detail{};
    // One bounded page, never an entire long conversation in RAM.
    char transcript[12288]{};
    CallPlaybackState playback = CallPlaybackState::Stopped;
    uint32_t playback_ms = 0, playback_duration_ms = 0;
};

class CallRecords {
public:
    static CallRecords& Instance();
    void Start();
    void ConfigureCloud(const AliyunRealtimeConfig& config);
    // Nonblocking, bounded copies from the real HFP media path. No filesystem IO.
    void Capture(bool outgoing, const int16_t* samples, size_t count, unsigned rate,
                 uint32_t sequence = 0, uint32_t generation = 0);
    void Transcript(bool user, const char* text, bool final = true);
    void Snapshot(CallRecordView& view);
    void RequestPage(uint32_t offset);
    void RequestDetail(const char* id, uint32_t transcript_offset = 0);
    void Play(const char* id);
    void Pause();
    void Seek(uint32_t ms);
    void StopPlayback();
    void Remount();
    bool Command(const char* line);
    // Read-only diagnostic access to finalized files; IDs and names are allowlisted.
    bool ReadFile(const char* id, const char* name, uint64_t offset, void* buffer, size_t capacity,
                  size_t& bytes, uint64_t& total);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

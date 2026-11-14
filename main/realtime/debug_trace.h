#ifndef ALIYUN_REALTIME_DEBUG_TRACE_H
#define ALIYUN_REALTIME_DEBUG_TRACE_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"

class DebugTrace {
public:
    static DebugTrace& Instance();
    void Configure(const AliyunRealtimeConfig& config);
    void Open(const char* mode, bool capture_aec = false);
    void SetConversationAecCapture(bool enable) { conversation_aec_capture_.store(enable); }
    void SetMode(const char* mode);
    void Close();
    void Event(const char* type, const char* detail = "");
    void WebSocketTx(const char* data, size_t length, bool success, bool audio = false);
    void WebSocketRx(const char* data, size_t length, bool audio = false);
    void Pcm(bool uplink, const uint8_t* data, size_t length, int sample_rate);
    void PcmInput(const int16_t* data, size_t samples, int channels, int sample_rate);
    void AecPcm(const char* track, const int16_t* data, size_t samples, int64_t sample_start_us,
                uint64_t sample_index);

private:
    struct FrameDeleter {
        void operator()(uint8_t* data) const;
    };
    struct Frame {
        std::unique_ptr<uint8_t, FrameDeleter> bytes;
        size_t size = 0;
    };
    DebugTrace();
    static void RunTask(void* arg);
    void Run();
    struct AecBatch {
        std::array<int16_t, 1024> data{};
        size_t samples = 0;
        int64_t start_us = 0;
        uint64_t index = 0;
    };
    void FlushAecBatch(size_t track);
    void Queue(uint16_t kind, const char* type, const char* detail, const uint8_t* payload,
               size_t length, int sample_rate = 0, int part_index = 0, int part_count = 0,
               size_t total_length = 0, const char* message_id = "", int64_t sample_start_us = 0,
               uint64_t sample_index = 0, int channels = 1);
    bool SendFrame(int socket, const uint8_t* frame, size_t size);

    std::mutex mutex_;
    std::mutex aec_mutex_;
    std::unique_ptr<std::array<AecBatch, 3>> aec_batches_;
    std::atomic<bool> capture_aec_{false};
    std::atomic<bool> conversation_aec_capture_{false};
    std::atomic<bool> capture_input_pcm_{false};
    std::deque<Frame> queue_;
    std::string address_;
    std::string boot_id_;
    std::string trace_id_;
    std::string mode_;
    uint64_t seq_ = 0;
    size_t queued_bytes_ = 0;
    int level_ = 0;
    bool enabled_ = false;
    bool started_ = false;
};

#endif

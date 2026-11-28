#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include "audio_service.h"
#include "phone_ai_audio.h"
#include "phone_ai_state.h"

struct PhoneBridgeCallbacks {
    std::function<void(bool)> on_call_active;
};

class PhoneBridge {
public:
    explicit PhoneBridge(AudioService& audio) : audio_(audio) {}
    void Start();
    void SetCallbacks(PhoneBridgeCallbacks callbacks) { callbacks_ = std::move(callbacks); }
    bool SetAiRoute(bool enabled);
    bool AiRoute() const { return ai_route_.load(); }
    bool UsesLocalAudio() const { return local_audio_.load(); }
    void QueueAiDownlink(std::unique_ptr<AudioStreamPacket> packet);
    std::unique_ptr<AudioStreamPacket> PopPhoneUplink();
    bool AiPlaybackIdle();
    bool AiAudioFailed() const { return AiAudioFailure() != PhoneAiFailure::None; }
    PhoneAiFailure AiAudioFailure() const { return ai_failure_.load(); }

private:
    static void Task(void* arg);
    void Run();
#if CONFIG_PHONE_BRIDGE_UART
    void RunUart();
#endif
    void RunUdp();
    void InvalidateAiMedia(const char* reason, PhoneAiFailure failure);
    AudioService& audio_;
    TaskHandle_t task_handle_ = nullptr;
    PhoneBridgeCallbacks callbacks_;
    std::atomic<bool> ai_route_{false};
    std::atomic<bool> local_audio_{false};
    std::mutex ai_mutex_;
    PhonePcmRing ai_downlink_;
    int16_t* ai_storage_ = nullptr;
    std::deque<std::unique_ptr<AudioStreamPacket>> phone_uplink_;
    std::atomic<PhoneAiFailure> ai_failure_{PhoneAiFailure::None};
    std::atomic<uint32_t> ai_epoch_{0};
    int64_t ai_played_us_ = 0;
    uint32_t ai_rx_samples_ = 0, ai_tx_samples_ = 0, ai_uplink_drops_ = 0;
    bool last_call_active_ = false;
};

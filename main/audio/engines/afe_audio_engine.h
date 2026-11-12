#ifndef AFE_AUDIO_ENGINE_H
#define AFE_AUDIO_ENGINE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <esp_afe_sr_models.h>
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
#include <esp_ae_rate_cvt.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "audio_engine.h"
#include "wake_words/wake_word_audio_cache.h"

class CustomWakeWord;

class AfeAudioEngine : public AudioEngine {
public:
    AfeAudioEngine();
    ~AfeAudioEngine() override;

    bool Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    void Feed(std::vector<int16_t>&& data) override;
    void FeedPlaybackReference(const int16_t* data, size_t samples, int sample_rate,
                               int64_t output_time_us) override;
    void ResetPlaybackReference() override;
    void SetPlaybackReferenceDelay(int delay_ms) override;
    AudioEngineDiagnostics GetDiagnostics() const override;

    void EnableWakeWordDetection(bool enable) override;
    void EnableVoiceProcessing(bool enable) override;
    void EnableDeviceAec(bool enable) override;

    bool HasWakeWord() const override;
    bool IsWakeWordDetectionEnabled() const override;
    bool IsVoiceProcessingEnabled() const override;
    bool IsAfeWakeWord() const override { return HasWakeWord(); }
    std::vector<std::string> GetWakeWords() const override;
    size_t GetFeedSize() const override;

    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;

    void EncodeWakeWordData() override;
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override;
    const std::string& GetLastDetectedWakeWord() const override { return last_detected_wake_word_; }

private:
    enum class WakeDetector {
        kNone,
        kWakeNet,
        kMultiNet,
    };

    static constexpr EventBits_t kWakeWordEnabled = 1 << 0;
    static constexpr EventBits_t kVoiceProcessingEnabled = 1 << 1;
    static constexpr EventBits_t kAfeActive = 1 << 2;
    static constexpr size_t kProcessingTaskStackSize = 4096;

    AudioCodec* codec_ = nullptr;
    srmodel_list_t* models_ = nullptr;
    bool owns_models_ = false;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t processing_task_ = nullptr;
    StaticTask_t* processing_task_buffer_ = nullptr;
    StackType_t* processing_task_stack_ = nullptr;
    int frame_samples_ = 0;
    bool is_speaking_ = false;
    std::atomic<bool> device_aec_enabled_{false};
    // Deferred AFE buffer reset, performed by ProcessingTask (see UpdateActiveState)
    std::atomic<bool> reset_pending_{false};
    // Deferred WakeNet/AEC toggles, applied by ProcessingTask (see ApplyAfeControls)
    std::atomic<bool> afe_control_dirty_{false};
    // Deferred output_buffer_ clear, performed by the output-producing task
    std::atomic<bool> output_reset_pending_{false};
    // Incremented whenever an active AFE session is invalidated. ProcessingTask
    // uses it to reject a fetch result produced before a disable/re-enable cycle.
    std::atomic<uint32_t> control_generation_{0};
    WakeDetector wake_detector_ = WakeDetector::kNone;

    std::unique_ptr<CustomWakeWord> custom_wake_word_;
    std::vector<std::string> wake_words_;
    std::string last_detected_wake_word_;
    std::vector<int16_t> input_buffer_;
    std::vector<int16_t> output_buffer_;
    std::mutex input_buffer_mutex_;
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
    static constexpr size_t kReferenceCapacity = 16000;
    std::mutex reference_mutex_;
    esp_ae_rate_cvt_handle_t reference_resampler_ = nullptr;
    int16_t* reference_ring_ = nullptr;
    int64_t reference_start_us_ = 0;
    uint64_t reference_written_ = 0;
    int64_t playback_reference_start_us_ = 0;
    uint64_t playback_reference_samples_ = 0;
    std::vector<int16_t> reference_resampled_;
    std::vector<int16_t> aligned_reference_;
    std::vector<int16_t> interleaved_input_;
    uint64_t input_sample_index_ = 0;
    uint64_t output_sample_index_ = 0;
    bool reference_nonzero_logged_ = false;
    bool aligned_reference_nonzero_logged_ = false;
    uint32_t reference_debug_feed_count_ = 0;
    std::atomic<int64_t> input_origin_us_{0};
    std::atomic<int> reference_delay_ms_{CONFIG_WAVESHARE_3_5B_AEC_DELAY_MS};
    bool software_reference_ready_ = false;
    std::atomic<uint32_t> diagnostic_feed_frames_{0};
    std::atomic<uint32_t> diagnostic_feed_samples_{0};
    std::atomic<uint32_t> diagnostic_feed_wall_us_{0};
    std::atomic<uint32_t> diagnostic_feed_max_us_{0};
    std::atomic<uint32_t> diagnostic_feed_overruns_{0};
    std::atomic<uint32_t> diagnostic_fetch_frames_{0};
    std::atomic<uint32_t> diagnostic_fetch_samples_{0};
    std::atomic<uint32_t> diagnostic_fetch_errors_{0};
#endif

    std::function<void(const std::string&)> wake_word_detected_callback_;
    std::function<void(std::vector<int16_t>&&)> output_callback_;
    std::function<void(bool)> vad_state_change_callback_;

    TaskHandle_t wake_word_encode_task_ = nullptr;
    StaticTask_t* wake_word_encode_task_buffer_ = nullptr;
    StackType_t* wake_word_encode_task_stack_ = nullptr;
    WakeWordAudioCache wake_word_audio_cache_;
    std::deque<std::vector<uint8_t>> wake_word_opus_;
    std::mutex wake_word_mutex_;
    std::condition_variable wake_word_cv_;

    void ProcessingTask();
    void UpdateActiveState();
    void UpdateAecState();
    void ApplyAfeControls();
    void ApplyPendingReset();
    void OutputRawAudio(const std::vector<int16_t>& data);
    void HandleWakeWordResult(const afe_fetch_result_t* result);
    void HandleVoiceResult(const afe_fetch_result_t* result);
};

#endif

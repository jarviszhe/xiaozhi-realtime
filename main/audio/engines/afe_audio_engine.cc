#include "afe_audio_engine.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vadn_models.h>

#include "audio_service.h"
#include "wake_words/custom_wake_word.h"
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
#include "realtime/debug_trace.h"
#endif

#define TAG "AfeAudioEngine"

#ifndef CONFIG_WAVESHARE_3_5B_AEC_NS
#define CONFIG_WAVESHARE_3_5B_AEC_NS 0
#endif

#if CONFIG_USE_AUDIO_PROCESSOR
static constexpr bool kUseAfeForVoiceProcessing = true;
#else
static constexpr bool kUseAfeForVoiceProcessing = false;
#endif

AfeAudioEngine::AfeAudioEngine() { event_group_ = xEventGroupCreate(); }

AfeAudioEngine::~AfeAudioEngine() {
    custom_wake_word_.reset();
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
    if (reference_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(reference_resampler_);
    }
    heap_caps_free(reference_ring_);
#endif
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    if (processing_task_stack_ != nullptr) {
        heap_caps_free(processing_task_stack_);
    }
    if (processing_task_buffer_ != nullptr) {
        heap_caps_free(processing_task_buffer_);
    }
    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }
    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }
    if (owns_models_ && models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}

bool AfeAudioEngine::Initialize(AudioCodec* codec, int frame_duration_ms,
                                srmodel_list_t* models_list) {
    if (afe_data_ != nullptr || (codec_ != nullptr && !kUseAfeForVoiceProcessing &&
                                 wake_detector_ == WakeDetector::kNone)) {
        return true;
    }
    if (codec == nullptr) {
        ESP_LOGE(TAG, "Codec is null");
        return false;
    }

    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
    output_buffer_.reserve(frame_samples_);

#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
    if (codec_->input_channels() == 1 && !codec_->input_reference() &&
        codec_->output_sample_rate() == 24000) {
        reference_ring_ = static_cast<int16_t*>(
            heap_caps_malloc(kReferenceCapacity * sizeof(int16_t), MALLOC_CAP_SPIRAM));
        esp_ae_rate_cvt_cfg_t cfg = {
            .src_rate = 24000,
            .dest_rate = 16000,
            .channel = 1,
            .bits_per_sample = ESP_AUDIO_BIT16,
            .complexity = 2,
            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
        };
        if (reference_ring_ != nullptr &&
            esp_ae_rate_cvt_open(&cfg, &reference_resampler_) == ESP_AE_ERR_OK) {
            reference_resampled_.reserve(1600);
            aligned_reference_.reserve(160);
            interleaved_input_.reserve(320);
            software_reference_ready_ = true;
        } else {
            ESP_LOGW(TAG, "Software AEC reference unavailable; using microphone-only AFE");
        }
    }
#endif

    if (models_list == nullptr) {
        models_ = esp_srmodel_init("model");
        owns_models_ = models_ != nullptr;
    } else {
        models_ = models_list;
    }

    char* wakenet_model_name = nullptr;
    char* multinet_model_name = nullptr;
    if (models_ != nullptr && models_->num > 0) {
        wakenet_model_name = esp_srmodel_filter(models_, ESP_WN_PREFIX, nullptr);
        multinet_model_name = esp_srmodel_filter(models_, ESP_MN_PREFIX, nullptr);
        for (int i = 0; i < models_->num; ++i) {
            ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
        }
    }

    if (multinet_model_name != nullptr) {
        wake_detector_ = WakeDetector::kMultiNet;
        custom_wake_word_ = std::make_unique<CustomWakeWord>();
        custom_wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            last_detected_wake_word_ = wake_word;
            xEventGroupClearBits(event_group_, kWakeWordEnabled);
            UpdateActiveState();
            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(wake_word);
            }
        });
        if (!custom_wake_word_->Initialize(codec_, models_)) {
            ESP_LOGE(TAG, "Failed to initialize MultiNet wake-word detector");
            custom_wake_word_.reset();
            wake_detector_ = WakeDetector::kNone;
            return false;
        }
    } else if (wakenet_model_name != nullptr) {
        wake_detector_ = WakeDetector::kWakeNet;
#if CONFIG_SEND_WAKE_WORD_DATA
        if (!wake_word_audio_cache_.Initialize(16000 * 2)) {
            ESP_LOGW(TAG, "Wake-word audio upload disabled: PSRAM cache allocation failed");
        }
#endif
    }

    const bool needs_afe = kUseAfeForVoiceProcessing || wake_detector_ != WakeDetector::kNone;
    if (!needs_afe) {
        ESP_LOGI(TAG, "Initialized as raw engine because AFE features are disabled");
        return true;
    }

    int ref_num = codec_->input_reference() ? 1 : 0;
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
    if (software_reference_ready_) {
        ++ref_num;
    }
#endif
    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - (codec_->input_reference() ? 1 : 0); ++i) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; ++i) {
        input_format.push_back('R');
    }

    char* vad_model_name =
        models_ == nullptr ? nullptr : esp_srmodel_filter(models_, ESP_VADN_PREFIX, nullptr);
    afe_config_t* afe_config =
        afe_config_init(input_format.c_str(), models_, AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (afe_config == nullptr) {
        ESP_LOGE(TAG, "Failed to create AFE configuration");
        return false;
    }

    afe_config->aec_init = ref_num > 0;
    afe_config->aec_mode = AEC_MODE_FD_LOW_COST;
    afe_config->aec_nlp_level = AEC_NLP_LEVEL_VERYAGGR;
    // The ESP-SR 1-mic AFE uses se_init for its built-in WebRTC noise
    // suppression stage. Keep ns_init false because no NSNet model is loaded.
    afe_config->se_init = CONFIG_WAVESHARE_3_5B_AEC_NS;
    afe_config->ns_init = false;
    afe_config->vad_init = kUseAfeForVoiceProcessing;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }
    afe_config->wakenet_init = wake_detector_ == WakeDetector::kWakeNet;
    afe_config->wakenet_model_name = nullptr;
    afe_config->wakenet_model_name_2 = nullptr;
    if (wake_detector_ == WakeDetector::kWakeNet && models_ != nullptr) {
        std::vector<char*> wakenet_models;
        for (int i = 0; i < models_->num; ++i) {
            char* name = models_->model_name[i];
            if (name != nullptr && strncmp(name, ESP_WN_PREFIX, strlen(ESP_WN_PREFIX)) == 0) {
                wakenet_models.push_back(name);
            }
        }
        if (!wakenet_models.empty()) {
            afe_config->wakenet_model_name = wakenet_models[0];
        }
        if (wakenet_models.size() > 1) {
            afe_config->wakenet_model_name_2 = wakenet_models[1];
        }
        // Only the two models selected above are active in the AFE.
        wake_words_.clear();
        for (auto name : {afe_config->wakenet_model_name, afe_config->wakenet_model_name_2}) {
            if (name == nullptr) {
                continue;
            }
            auto words = esp_srmodel_get_wake_words(models_, name);
            if (words == nullptr) {
                continue;
            }
            std::string_view remaining(words);
            while (!remaining.empty()) {
                const auto end = remaining.find(';');
                const auto word = remaining.substr(0, end);
                if (!word.empty()) {
                    wake_words_.emplace_back(word);
                }
                if (end == std::string_view::npos) {
                    break;
                }
                remaining.remove_prefix(end + 1);
            }
        }
    }
    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    ESP_LOGI(TAG, "Before AFE create: free=%u min=%u largest=%u",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    afe_iface_ = esp_afe_handle_from_config(afe_config);
    if (afe_iface_ != nullptr) {
        afe_data_ = afe_iface_->create_from_config(afe_config);
    }
    afe_config_free(afe_config);

    if (afe_iface_ == nullptr || afe_data_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create FD AFE instance");
        afe_iface_ = nullptr;
        afe_data_ = nullptr;
        return false;
    }

    if (afe_iface_->get_feed_channel_num(afe_data_) != codec_->input_channels()
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
                                                           + (software_reference_ready_ ? 1 : 0)
#endif
    ) {
        ESP_LOGE(TAG, "AFE feed channel count does not match input format");
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        return false;
    }

    if (wake_detector_ == WakeDetector::kWakeNet) {
        afe_iface_->disable_wakenet(afe_data_);
    }
    if (ref_num > 0) {
        afe_iface_->disable_aec(afe_data_);
    }
    afe_iface_->print_pipeline(afe_data_);

    if (processing_task_stack_ == nullptr) {
        processing_task_stack_ = static_cast<StackType_t*>(
            heap_caps_malloc(kProcessingTaskStackSize, MALLOC_CAP_SPIRAM));
    }
    if (processing_task_buffer_ == nullptr) {
        processing_task_buffer_ =
            static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    }
    if (processing_task_stack_ == nullptr || processing_task_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate AFE processing task memory");
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        return false;
    }

    processing_task_ = xTaskCreateStatic(
        [](void* arg) {
            auto* engine = static_cast<AfeAudioEngine*>(arg);
            engine->ProcessingTask();
            vTaskDelete(nullptr);
        },
        "audio_afe", kProcessingTaskStackSize, this, 3, processing_task_stack_,
        processing_task_buffer_);
    if (processing_task_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create AFE processing task, internal free=%u largest=%u",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        return false;
    }

    const char* detector = wake_detector_ == WakeDetector::kWakeNet
                               ? "WakeNet"
                               : (wake_detector_ == WakeDetector::kMultiNet ? "MultiNet" : "none");
    ESP_LOGI(TAG, "Initialized FD AFE, detector: %s, NS: %s, feed: %d, fetch: %d", detector,
             CONFIG_WAVESHARE_3_5B_AEC_NS ? "webrtc" : "off",
             afe_iface_->get_feed_chunksize(afe_data_), afe_iface_->get_fetch_chunksize(afe_data_));
    ESP_LOGI(TAG, "After AFE create: free=%u min=%u largest=%u",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;
}

void AfeAudioEngine::Feed(std::vector<int16_t>&& data) {
    EventBits_t bits = xEventGroupGetBits(event_group_);
    if ((bits & kVoiceProcessingEnabled) && !kUseAfeForVoiceProcessing) {
        OutputRawAudio(data);
    }
    if (afe_data_ == nullptr || (bits & kAfeActive) == 0) {
        return;
    }

#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
    if (software_reference_ready_) {
        // Playback cancellation can reset this timeline from the main task.
        // Keep its counters and diagnostics under the reference lock as well.
        std::lock_guard<std::mutex> reference_lock(reference_mutex_);
        const int64_t end_us = esp_timer_get_time();
        const int64_t observed_start_us =
            end_us - static_cast<int64_t>(data.size()) * 1000000 / 16000;
        const int64_t origin_us = input_origin_us_.load();
        const int64_t expected_us =
            origin_us + static_cast<int64_t>(input_sample_index_) * 1000000 / 16000;
        int64_t start_us = expected_us;
        if (origin_us == 0 || observed_start_us < expected_us - 100000 ||
            observed_start_us > expected_us + 100000) {
            start_us = observed_start_us;
            input_origin_us_.store(start_us -
                                   static_cast<int64_t>(input_sample_index_) * 1000000 / 16000);
        }
        // DMA samples have a fixed cadence. Scheduling jitter must not duplicate
        // or skip reference samples at each 10 ms read boundary.
        aligned_reference_.resize(data.size());
        {
            const uint64_t oldest = reference_written_ > kReferenceCapacity
                                        ? reference_written_ - kReferenceCapacity
                                        : 0;
            const int64_t first_index = (start_us - reference_start_us_) * 16000 / 1000000;
            for (size_t i = 0; i < data.size(); ++i) {
                const int64_t index = first_index + static_cast<int64_t>(i);
                aligned_reference_[i] = reference_start_us_ != 0 && index >= 0 &&
                                                static_cast<uint64_t>(index) >= oldest &&
                                                static_cast<uint64_t>(index) < reference_written_
                                            ? reference_ring_[index % kReferenceCapacity]
                                            : 0;
            }
        }
        int aligned_max_abs = 0;
        for (const int16_t sample : aligned_reference_) {
            aligned_max_abs = std::max(aligned_max_abs, std::abs(static_cast<int>(sample)));
        }
        if (!aligned_reference_nonzero_logged_ && aligned_max_abs > 0) {
            aligned_reference_nonzero_logged_ = true;
            DebugTrace::Instance().Event("aec_ref_aligned_nonzero", "aligned reference has energy");
        }
        if (++reference_debug_feed_count_ % 100 == 0) {
            const int64_t first_index = (start_us - reference_start_us_) * 16000 / 1000000;
            const uint64_t oldest = reference_written_ > kReferenceCapacity
                                        ? reference_written_ - kReferenceCapacity
                                        : 0;
            char detail[160];
            std::snprintf(detail, sizeof(detail),
                          "max=%d start=%lld ref_start=%lld written=%llu oldest=%llu index=%lld",
                          aligned_max_abs, static_cast<long long>(start_us),
                          static_cast<long long>(reference_start_us_),
                          static_cast<unsigned long long>(reference_written_),
                          static_cast<unsigned long long>(oldest),
                          static_cast<long long>(first_index));
            DebugTrace::Instance().Event("aec_ref_align_stats", detail);
        }
        DebugTrace::Instance().AecPcm("aec_mic", data.data(), data.size(), start_us,
                                      input_sample_index_);
        DebugTrace::Instance().AecPcm("aec_ref", aligned_reference_.data(),
                                      aligned_reference_.size(), start_us, input_sample_index_);
        input_sample_index_ += data.size();
        interleaved_input_.resize(data.size() * 2);
        for (size_t i = 0; i < data.size(); ++i) {
            interleaved_input_[2 * i] = data[i];
            interleaved_input_[2 * i + 1] = aligned_reference_[i];
        }
    }
#endif

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if ((xEventGroupGetBits(event_group_) & kAfeActive) == 0) {
        return;
    }
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
    if (software_reference_ready_) {
        input_buffer_.insert(input_buffer_.end(), interleaved_input_.begin(),
                             interleaved_input_.end());
    } else
#endif
    {
        input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    }
    size_t chunk_size =
        afe_iface_->get_feed_chunksize(afe_data_) * afe_iface_->get_feed_channel_num(afe_data_);
    while (input_buffer_.size() >= chunk_size) {
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
        const int64_t feed_start_us = esp_timer_get_time();
#endif
        afe_iface_->feed(afe_data_, input_buffer_.data());
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
        // Wall time includes preemption and any AFE backpressure; task runtime
        // snapshots separately measure CPU use. Never time the blocking fetch as work.
        const uint32_t elapsed_us = esp_timer_get_time() - feed_start_us;
        const uint32_t samples = afe_iface_->get_feed_chunksize(afe_data_);
        diagnostic_feed_frames_.fetch_add(1, std::memory_order_relaxed);
        diagnostic_feed_samples_.fetch_add(samples, std::memory_order_relaxed);
        diagnostic_feed_wall_us_.fetch_add(elapsed_us, std::memory_order_relaxed);
        diagnostic_feed_max_us_.store(std::max(diagnostic_feed_max_us_.load(), elapsed_us));
        if (elapsed_us > samples * 1000000 / 16000)
            diagnostic_feed_overruns_.fetch_add(1, std::memory_order_relaxed);
#endif
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
    }
}

AudioEngineDiagnostics AfeAudioEngine::GetDiagnostics() const {
    AudioEngineDiagnostics result;
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
    result.feed_frames = diagnostic_feed_frames_.load();
    result.feed_samples = diagnostic_feed_samples_.load();
    result.feed_wall_us = diagnostic_feed_wall_us_.load();
    result.feed_max_us = diagnostic_feed_max_us_.load();
    result.feed_overruns = diagnostic_feed_overruns_.load();
    result.fetch_frames = diagnostic_fetch_frames_.load();
    result.fetch_samples = diagnostic_fetch_samples_.load();
    result.fetch_errors = diagnostic_fetch_errors_.load();
#endif
    return result;
}

void AfeAudioEngine::FeedPlaybackReference(const int16_t* data, size_t samples, int sample_rate,
                                           int64_t output_time_us) {
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
    if (!software_reference_ready_ || data == nullptr || samples == 0 || sample_rate != 24000) {
        return;
    }
    std::lock_guard<std::mutex> lock(reference_mutex_);
    uint32_t output_samples = 0;
    if (esp_ae_rate_cvt_get_max_out_sample_num(reference_resampler_, samples, &output_samples) !=
            ESP_AE_ERR_OK ||
        output_samples > kReferenceCapacity) {
        return;
    }
    reference_resampled_.resize(output_samples);
    if (esp_ae_rate_cvt_process(
            reference_resampler_, reinterpret_cast<esp_ae_sample_t>(const_cast<int16_t*>(data)),
            samples, reinterpret_cast<esp_ae_sample_t>(reference_resampled_.data()),
            &output_samples) != ESP_AE_ERR_OK) {
        return;
    }
    reference_resampled_.resize(output_samples);
    if (!reference_nonzero_logged_) {
        int max_abs = 0;
        uint64_t sum_squared = 0;
        for (const int16_t sample : reference_resampled_) {
            const int value = std::abs(static_cast<int>(sample));
            max_abs = std::max(max_abs, value);
            sum_squared += static_cast<uint64_t>(value) * static_cast<uint64_t>(value);
        }
        char detail[96];
        const unsigned rms = reference_resampled_.empty()
                                 ? 0
                                 : static_cast<unsigned>(
                                       std::sqrt(static_cast<double>(sum_squared) /
                                                 static_cast<double>(reference_resampled_.size())));
        std::snprintf(detail, sizeof(detail), "samples=%u max=%d rms=%u",
                      static_cast<unsigned>(reference_resampled_.size()), max_abs, rms);
        DebugTrace::Instance().Event("aec_ref_feed_stats", detail);
        reference_nonzero_logged_ = max_abs > 0;
    }
    DebugTrace::Instance().Event("aec_ref_feed", "playback reference accepted");
    if (playback_reference_start_us_ == 0) {
        playback_reference_start_us_ =
            output_time_us + static_cast<int64_t>(reference_delay_ms_.load()) * 1000;
    }
    const int64_t start_us = playback_reference_start_us_ +
                             static_cast<int64_t>(playback_reference_samples_) * 1000000 / 24000;
    playback_reference_samples_ += samples;
    if (reference_start_us_ == 0) {
        reference_start_us_ = start_us;
    }
    const int64_t expected = (start_us - reference_start_us_) * 16000 / 1000000;
    if (expected < 0 || expected > static_cast<int64_t>(reference_written_ + 16000)) {
        reference_start_us_ = start_us;
        reference_written_ = 0;
    }
    const int64_t index = std::max<int64_t>(expected, reference_written_);
    while (static_cast<int64_t>(reference_written_) < index) {
        reference_ring_[reference_written_++ % kReferenceCapacity] = 0;
    }
    for (size_t offset = 0; offset < output_samples; ++offset) {
        reference_ring_[reference_written_++ % kReferenceCapacity] = reference_resampled_[offset];
    }
#else
    (void)data;
    (void)samples;
    (void)sample_rate;
    (void)output_time_us;
#endif
}

void AfeAudioEngine::ResetPlaybackReference() {
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
    std::lock_guard<std::mutex> lock(reference_mutex_);
    reference_start_us_ = 0;
    reference_written_ = 0;
    playback_reference_start_us_ = 0;
    playback_reference_samples_ = 0;
    reference_nonzero_logged_ = false;
    aligned_reference_nonzero_logged_ = false;
    reference_debug_feed_count_ = 0;
    input_origin_us_.store(0);
    input_sample_index_ = 0;
    output_sample_index_ = 0;
    if (reference_resampler_ != nullptr) {
        esp_ae_rate_cvt_reset(reference_resampler_);
    }
#endif
}

void AfeAudioEngine::SetPlaybackReferenceDelay(int delay_ms) {
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
    reference_delay_ms_.store(std::clamp(delay_ms, 0, 250));
#else
    (void)delay_ms;
#endif
}

void AfeAudioEngine::EnableWakeWordDetection(bool enable) {
    if (!HasWakeWord()) {
        return;
    }

    // WakeNet enable/disable on the AFE instance is applied by ProcessingTask
    // (see ApplyAfeControls), driven by the kWakeWordEnabled bit.
    if (enable) {
        if (wake_detector_ == WakeDetector::kMultiNet) {
            custom_wake_word_->Start();
        }
        xEventGroupSetBits(event_group_, kWakeWordEnabled);
    } else {
        xEventGroupClearBits(event_group_, kWakeWordEnabled);
        if (wake_detector_ == WakeDetector::kMultiNet) {
            custom_wake_word_->Stop();
        }
    }
    UpdateActiveState();
}

void AfeAudioEngine::EnableVoiceProcessing(bool enable) {
    if (enable) {
        xEventGroupSetBits(event_group_, kVoiceProcessingEnabled);
    } else {
        xEventGroupClearBits(event_group_, kVoiceProcessingEnabled);
        is_speaking_ = false;
    }
    UpdateActiveState();
}

void AfeAudioEngine::EnableDeviceAec(bool enable) {
    device_aec_enabled_.store(enable);
    if (enable && (codec_ == nullptr || !(codec_->input_reference()
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
                                          || software_reference_ready_
#endif
                                          ))) {
        ESP_LOGW(TAG, "Device AEC requires a playback reference channel");
    }
    UpdateAecState();
}

bool AfeAudioEngine::HasWakeWord() const { return wake_detector_ != WakeDetector::kNone; }

std::vector<std::string> AfeAudioEngine::GetWakeWords() const {
    return custom_wake_word_ ? custom_wake_word_->GetWakeWords() : wake_words_;
}

bool AfeAudioEngine::IsWakeWordDetectionEnabled() const {
    return event_group_ != nullptr && (xEventGroupGetBits(event_group_) & kWakeWordEnabled) != 0;
}

bool AfeAudioEngine::IsVoiceProcessingEnabled() const {
    return event_group_ != nullptr &&
           (xEventGroupGetBits(event_group_) & kVoiceProcessingEnabled) != 0;
}

size_t AfeAudioEngine::GetFeedSize() const {
    return afe_data_ == nullptr ? frame_samples_ : afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioEngine::OnWakeWordDetected(
    std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = std::move(callback);
}

void AfeAudioEngine::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = std::move(callback);
}

void AfeAudioEngine::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = std::move(callback);
}

void AfeAudioEngine::UpdateActiveState() {
    EventBits_t bits = xEventGroupGetBits(event_group_);
    const bool afe_active =
        afe_data_ != nullptr && ((bits & kWakeWordEnabled) ||
                                 (kUseAfeForVoiceProcessing && (bits & kVoiceProcessingEnabled)));
    if (afe_active) {
        xEventGroupSetBits(event_group_, kAfeActive);
    } else {
        xEventGroupClearBits(event_group_, kAfeActive);
        control_generation_.fetch_add(1);
        std::lock_guard<std::mutex> lock(input_buffer_mutex_);
        input_buffer_.clear();
        if (afe_data_ != nullptr) {
            // Don't call reset_buffer() here: this runs in the main task while
            // ProcessingTask may be inside fetch_with_delay() on the same AFE
            // instance, and a concurrent reset corrupts the ring buffer state.
            // Defer the reset to ProcessingTask, which owns the fetch side.
            reset_pending_ = true;
        }
    }
    if ((bits & kVoiceProcessingEnabled) == 0) {
        // output_buffer_ is owned by the task that produces output frames,
        // so let that task clear it instead of racing with it here.
        output_reset_pending_ = true;
    }
    afe_control_dirty_ = true;
}

void AfeAudioEngine::UpdateAecState() {
    if (afe_data_ == nullptr || codec_ == nullptr ||
        !(codec_->input_reference()
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
          || software_reference_ready_
#endif
          )) {
        return;
    }
    afe_control_dirty_ = true;
}

void AfeAudioEngine::ApplyAfeControls() {
    EventBits_t bits = xEventGroupGetBits(event_group_);
    if (wake_detector_ == WakeDetector::kWakeNet) {
        if (bits & kWakeWordEnabled) {
            afe_iface_->enable_wakenet(afe_data_);
        } else {
            afe_iface_->disable_wakenet(afe_data_);
        }
    }
    if (codec_->input_reference()
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
        || software_reference_ready_
#endif
    ) {
        const bool enable_aec = (bits & kWakeWordEnabled) ||
                                (device_aec_enabled_.load() && (bits & kVoiceProcessingEnabled));
        if (enable_aec) {
            afe_iface_->enable_aec(afe_data_);
        } else {
            afe_iface_->disable_aec(afe_data_);
        }
    }
}

void AfeAudioEngine::ApplyPendingReset() {
    if (!reset_pending_.exchange(false)) {
        return;
    }
    // Discard audio recorded before (re)activation. Holding input_buffer_mutex_
    // serializes the reset against Feed(); fetch/reset both run in this task.
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    input_buffer_.clear();
    afe_iface_->reset_buffer(afe_data_);
}

void AfeAudioEngine::ProcessingTask() {
    while (true) {
        xEventGroupWaitBits(event_group_, kAfeActive, pdFALSE, pdTRUE, portMAX_DELAY);
        ApplyPendingReset();
        if ((xEventGroupGetBits(event_group_) & kAfeActive) == 0) {
            continue;
        }
        if (afe_control_dirty_.exchange(false)) {
            // WakeNet/AEC toggles are not safe against a concurrent fetch,
            // so they are applied here, in the task that owns the fetch side.
            ApplyAfeControls();
        }
        const uint32_t generation = control_generation_.load();
        auto* result = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if (generation != control_generation_.load() ||
            (xEventGroupGetBits(event_group_) & kAfeActive) == 0) {
            // A disable/re-enable may make an old blocked fetch return after the
            // AFE is active again. Reset immediately and never process that frame.
            ApplyPendingReset();
            continue;
        }
        if (result == nullptr || result->ret_value == ESP_FAIL) {
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
            diagnostic_fetch_errors_.fetch_add(1, std::memory_order_relaxed);
#endif
            if (result != nullptr) {
                ESP_LOGW(TAG, "AFE fetch failed: %d", result->ret_value);
            }
            continue;
        }

#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
        diagnostic_fetch_frames_.fetch_add(1, std::memory_order_relaxed);
        diagnostic_fetch_samples_.fetch_add(result->data_size / sizeof(int16_t),
                                            std::memory_order_relaxed);
#endif
        EventBits_t bits = xEventGroupGetBits(event_group_);
        if (bits & kWakeWordEnabled) {
            HandleWakeWordResult(result);
        }
        if (kUseAfeForVoiceProcessing && (bits & kVoiceProcessingEnabled)) {
            HandleVoiceResult(result);
        }
    }
}

void AfeAudioEngine::HandleWakeWordResult(const afe_fetch_result_t* result) {
    if (wake_detector_ == WakeDetector::kMultiNet) {
        custom_wake_word_->FeedMono(result->data, result->data_size / sizeof(int16_t));
        return;
    }

#if CONFIG_SEND_WAKE_WORD_DATA
    wake_word_audio_cache_.Store(result->data, result->data_size / sizeof(int16_t));
#endif
    if (result->wakeup_state != WAKENET_DETECTED) {
        return;
    }

    int model_index = result->wakenet_model_index - 1;
    if (model_index < 0 || model_index >= static_cast<int>(wake_words_.size())) {
        ESP_LOGE(TAG, "Invalid WakeNet model index: %d", result->wakenet_model_index);
        return;
    }

    last_detected_wake_word_ = wake_words_[model_index];
    xEventGroupClearBits(event_group_, kWakeWordEnabled);
    // UpdateActiveState marks the AFE controls dirty; the next loop iteration
    // of ProcessingTask disables WakeNet via ApplyAfeControls.
    UpdateActiveState();
    if (wake_word_detected_callback_) {
        wake_word_detected_callback_(last_detected_wake_word_);
    }
}

void AfeAudioEngine::HandleVoiceResult(const afe_fetch_result_t* result) {
    if (output_reset_pending_.exchange(false)) {
        output_buffer_.clear();
    }
    if (vad_state_change_callback_) {
        if (result->vad_state == VAD_SPEECH && !is_speaking_) {
            is_speaking_ = true;
            vad_state_change_callback_(true);
        } else if (result->vad_state == VAD_SILENCE && is_speaking_) {
            is_speaking_ = false;
            vad_state_change_callback_(false);
        }
    }
    if (!output_callback_) {
        return;
    }

    size_t samples = result->data_size / sizeof(int16_t);
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
    if (software_reference_ready_) {
        std::lock_guard<std::mutex> reference_lock(reference_mutex_);
        const int64_t start_us =
            input_origin_us_.load() + static_cast<int64_t>(output_sample_index_) * 1000000 / 16000;
        DebugTrace::Instance().AecPcm("aec_out", result->data, samples, start_us,
                                      output_sample_index_);
        output_sample_index_ += samples;
    }
#endif
    output_buffer_.insert(output_buffer_.end(), result->data, result->data + samples);
    while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
        if (output_buffer_.size() == static_cast<size_t>(frame_samples_)) {
            output_callback_(std::move(output_buffer_));
            output_buffer_.clear();
            output_buffer_.reserve(frame_samples_);
        } else {
            output_callback_(std::vector<int16_t>(output_buffer_.begin(),
                                                  output_buffer_.begin() + frame_samples_));
            output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
        }
    }
}

void AfeAudioEngine::OutputRawAudio(const std::vector<int16_t>& data) {
    if (!output_callback_ || codec_ == nullptr) {
        return;
    }
    if (output_reset_pending_.exchange(false)) {
        output_buffer_.clear();
    }
    const size_t channels = codec_->input_channels();
    if (channels <= 1) {
        output_buffer_.insert(output_buffer_.end(), data.begin(), data.end());
    } else {
        for (size_t i = 0; i < data.size(); i += channels) {
            output_buffer_.push_back(data[i]);
        }
    }
    while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
        output_callback_(
            std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
        output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
    }
}

void AfeAudioEngine::EncodeWakeWordData() {
    if (wake_detector_ == WakeDetector::kMultiNet) {
        custom_wake_word_->EncodeWakeWordData();
        return;
    }
    if (wake_detector_ != WakeDetector::kWakeNet) {
        return;
    }

    const size_t stack_size = 4096 * 6;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ =
            static_cast<StackType_t*>(heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM));
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ =
            static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic(
        [](void* arg) {
            auto* engine = static_cast<AfeAudioEngine*>(arg);
            auto start_time = esp_timer_get_time();
            esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
            void* encoder_handle = nullptr;
            auto ret =
                esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
            if (encoder_handle == nullptr) {
                ESP_LOGE(TAG, "Failed to create wake-word encoder: %d", ret);
                engine->wake_word_audio_cache_.Clear();
                {
                    std::lock_guard<std::mutex> lock(engine->wake_word_mutex_);
                    engine->wake_word_opus_.emplace_back();
                    engine->wake_word_cv_.notify_all();
                }
                vTaskDelete(nullptr);
                return;
            }

            int frame_size = 0;
            int outbuf_size = 0;
            esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
            frame_size /= sizeof(int16_t);
            int packets = 0;
            std::vector<int16_t> input(frame_size);
            esp_audio_enc_in_frame_t in = {};
            esp_audio_enc_out_frame_t out = {};

            const size_t cached_samples = engine->wake_word_audio_cache_.Size();
            for (size_t offset = 0; offset + static_cast<size_t>(frame_size) <= cached_samples;
                 offset += frame_size) {
                if (engine->wake_word_audio_cache_.Read(offset, input.data(), frame_size) !=
                    static_cast<size_t>(frame_size)) {
                    break;
                }
                std::vector<uint8_t> opus_buf(outbuf_size);
                in.buffer = reinterpret_cast<uint8_t*>(input.data());
                in.len = frame_size * sizeof(int16_t);
                out.buffer = opus_buf.data();
                out.len = outbuf_size;
                out.encoded_bytes = 0;
                ret = esp_opus_enc_process(encoder_handle, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    std::lock_guard<std::mutex> lock(engine->wake_word_mutex_);
                    engine->wake_word_opus_.emplace_back(opus_buf.data(),
                                                         opus_buf.data() + out.encoded_bytes);
                    engine->wake_word_cv_.notify_all();
                    ++packets;
                } else {
                    ESP_LOGE(TAG, "Failed to encode wake-word audio: %d", ret);
                }
            }

            engine->wake_word_audio_cache_.Clear();
            esp_opus_enc_close(encoder_handle);
            ESP_LOGI(TAG, "Encoded wake word into %d packets in %ld ms", packets,
                     static_cast<long>((esp_timer_get_time() - start_time) / 1000));
            {
                std::lock_guard<std::mutex> lock(engine->wake_word_mutex_);
                engine->wake_word_opus_.emplace_back();
                engine->wake_word_cv_.notify_all();
            }
            vTaskDelete(nullptr);
        },
        "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_,
        wake_word_encode_task_buffer_);
}

bool AfeAudioEngine::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    if (wake_detector_ == WakeDetector::kMultiNet) {
        return custom_wake_word_->GetWakeWordOpus(opus);
    }
    if (wake_detector_ != WakeDetector::kWakeNet) {
        return false;
    }
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() { return !wake_word_opus_.empty(); });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}

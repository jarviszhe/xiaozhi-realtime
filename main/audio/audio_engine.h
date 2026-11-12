#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <model_path.h>

#include "audio_codec.h"

struct AudioEngineDiagnostics {
    bool aec_enabled = false;
    uint32_t feed_frames = 0;
    uint32_t feed_samples = 0;
    uint32_t feed_wall_us = 0;
    uint32_t feed_max_us = 0;
    uint32_t feed_overruns = 0;
    uint32_t fetch_frames = 0;
    uint32_t fetch_samples = 0;
    uint32_t fetch_errors = 0;
};

class AudioEngine {
public:
    virtual ~AudioEngine() = default;

    virtual bool Initialize(AudioCodec* codec, int frame_duration_ms,
                            srmodel_list_t* models_list) = 0;
    virtual void Feed(std::vector<int16_t>&& data) = 0;
    virtual void FeedPlaybackReference(const int16_t* data, size_t samples, int sample_rate,
                                       int64_t output_time_us) {}
    virtual void ResetPlaybackReference() {}
    virtual void SetPlaybackReferenceDelay(int delay_ms) {}
    virtual void SetPlaybackAecOnly(bool enable) {}
    virtual void PreparePlayback() {}
    virtual bool HasAecReference() const { return false; }
    virtual AudioEngineDiagnostics GetDiagnostics() const { return {}; }

    virtual void EnableWakeWordDetection(bool enable) = 0;
    virtual void EnableVoiceProcessing(bool enable) = 0;
    virtual void EnableDeviceAec(bool enable) = 0;

    virtual bool HasWakeWord() const = 0;
    virtual bool IsWakeWordDetectionEnabled() const = 0;
    virtual bool IsVoiceProcessingEnabled() const = 0;
    virtual bool IsAfeWakeWord() const = 0;
    virtual std::vector<std::string> GetWakeWords() const = 0;
    virtual size_t GetFeedSize() const = 0;

    virtual void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) = 0;
    virtual void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) = 0;
    virtual void OnVadStateChange(std::function<void(bool speaking)> callback) = 0;

    virtual void EncodeWakeWordData() = 0;
    virtual bool GetWakeWordOpus(std::vector<uint8_t>& opus) = 0;
    virtual const std::string& GetLastDetectedWakeWord() const = 0;
};

#endif

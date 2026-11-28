#pragma once

enum class PhoneAiState { Unavailable, Ready, Connecting, Listening, Thinking, Speaking, Failed };

enum class PhoneAiFailure {
    None,
    CloudConnect,
    CloudDisconnected,
    GatewayControl,
    GatewayMediaTimeout,
    GatewayMediaChanged,
    AudioQueueOverflow,
    AudioMemory,
    WorkerResources
};

inline const char* PhoneAiFailureName(PhoneAiFailure failure) {
    switch (failure) {
        case PhoneAiFailure::None:
            return "none";
        case PhoneAiFailure::CloudConnect:
            return "cloud_connect";
        case PhoneAiFailure::CloudDisconnected:
            return "cloud_disconnected";
        case PhoneAiFailure::GatewayControl:
            return "gateway_control";
        case PhoneAiFailure::GatewayMediaTimeout:
            return "gateway_media_timeout";
        case PhoneAiFailure::GatewayMediaChanged:
            return "gateway_media_changed";
        case PhoneAiFailure::AudioQueueOverflow:
            return "audio_queue_overflow";
        case PhoneAiFailure::AudioMemory:
            return "audio_memory";
        case PhoneAiFailure::WorkerResources:
            return "worker_resources";
    }
    return "unknown";
}

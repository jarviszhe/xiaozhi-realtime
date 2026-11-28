#pragma once

#include <functional>
#include <string>
#include "realtime/config.h"

struct CallRecordCloudResult {
    bool transcript_complete = false;
    bool summary_complete = false;
    bool canceled = false;
    std::string summary;
    std::string error;
};

// Finalized WAVs only. Streams bounded chunks, performs independent ASR on both
// actual audio tracks, then summarizes the same transcript. No NVS access.
CallRecordCloudResult ProcessCallRecording(const std::string& directory,
                                           const AliyunRealtimeConfig& config,
                                           const std::function<bool()>& canceled);

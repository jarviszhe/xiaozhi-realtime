#pragma once

#include <cstddef>
#include <cstdint>

// Preserve sample cadence within a burst, but include silence between bursts.
// Call timestamps jitter while TX DMA buffers a continuous stream; a gap over
// 100 ms starts a new burst without resetting the AEC microphone timeline.
inline int64_t AdvancePlaybackTimeline(int64_t observed_us, int64_t& origin_us,
                                       uint64_t& samples_written, size_t samples, int rate) {
    const int64_t elapsed_us = static_cast<int64_t>(samples_written) * 1000000 / rate;
    const int64_t expected_us = origin_us + elapsed_us;
    if (origin_us == 0 || observed_us < expected_us - 100000 ||
        observed_us > expected_us + 100000) {
        origin_us = observed_us - elapsed_us;
    }
    samples_written += samples;
    return origin_us + elapsed_us;
}

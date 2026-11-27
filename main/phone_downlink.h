#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ima_adpcm.h"

static inline bool phone_downlink_payload_valid(unsigned codec, const uint8_t* payload,
                                                size_t bytes, size_t samples) {
    if (!payload || (samples != 160 && samples != 320))
        return false;
    return codec == 1 ? bytes == samples * 2
                      : codec == 2 && ima_adpcm_valid(payload, bytes, samples);
}

// Output is PCM16LE in a caller-owned, non-overlapping buffer. Invalid input
// leaves it untouched, before the caller advances session/sequence state.
static inline bool phone_downlink_decode(unsigned codec, const uint8_t* payload, size_t bytes,
                                         size_t samples, uint8_t* output, size_t capacity) {
    if (!output || capacity < samples * 2 ||
        !phone_downlink_payload_valid(codec, payload, bytes, samples))
        return false;
    if (codec == 2)
        return ima_adpcm_decode(payload, bytes, samples, output, capacity);
    memcpy(output, payload, bytes);
    return true;
}

// A missing datagram represents 20 ms of playout time. Bound synthetic audio
// to 100 ms; longer discontinuities start a fresh prebuffer instead.
#define PHONE_DOWNLINK_MAX_CONCEAL 5U

static inline uint32_t phone_downlink_missing(uint32_t expected, uint32_t sequence, bool* late,
                                              bool* rebuffer) {
    const int32_t delta = (int32_t)(sequence - expected);
    *late = delta < 0;
    *rebuffer = delta > (int32_t)PHONE_DOWNLINK_MAX_CONCEAL;
    return delta > 0 && !*rebuffer ? (uint32_t)delta : 0;
}

static inline bool phone_downlink_conceal(const int16_t* previous, int16_t* output, size_t samples,
                                          uint32_t missing_index) {
    if (!previous || !output || !samples || samples > 320 || !missing_index ||
        missing_index > PHONE_DOWNLINK_MAX_CONCEAL)
        return false;
    const int32_t gain = (int32_t)(PHONE_DOWNLINK_MAX_CONCEAL + 1 - missing_index);
    const int32_t divisor = (int32_t)(PHONE_DOWNLINK_MAX_CONCEAL + 1);
    for (size_t i = 0; i < samples; ++i)
        output[i] = (int16_t)((int32_t)previous[i] * gain / divisor);
    return true;
}

static inline bool phone_downlink_selftest(void) {
    bool late = false, rebuffer = false;
    if (phone_downlink_missing(UINT32_MAX, 0, &late, &rebuffer) != 1 || late || rebuffer)
        return false;
    if (phone_downlink_missing(0, UINT32_MAX, &late, &rebuffer) || !late || rebuffer)
        return false;
    if (phone_downlink_missing(20, 26, &late, &rebuffer) || late || !rebuffer)
        return false;
    const int16_t previous[4] = {32767, -32768, 12345, -12345};
    int16_t output[4];
    if (!phone_downlink_conceal(previous, output, 4, 1) || output[0] != 27305 ||
        output[1] != -27306 || output[2] != 10287 || output[3] != -10287)
        return false;
    if (!phone_downlink_conceal(previous, output, 4, 5) || output[0] != 5461 ||
        output[1] != -5461 || phone_downlink_conceal(previous, output, 4, 6))
        return false;
    return true;
}

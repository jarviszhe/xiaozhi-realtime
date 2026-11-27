// SPDX-License-Identifier: MIT
// Shared with xiaozhi-realtime/main/ima_adpcm.h. Wire authority: udp-audio-v1.md.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const int ima_steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int ima_index_delta[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static inline int ima_clamp(int n, int low, int high) {
    return n < low ? low : n > high ? high : n;
}

static inline size_t ima_adpcm_bytes(size_t samples) {
    return samples && samples <= 320 ? 4 + samples / 2 : 0;
}

static inline bool ima_adpcm_valid(const uint8_t* p, size_t bytes, size_t samples) {
    size_t expected = ima_adpcm_bytes(samples);
    return p && expected && bytes == expected && p[2] <= 88 && p[3] == 0 &&
           ((samples & 1) || !(p[bytes - 1] & 0xf0));
}

static inline int ima_apply(unsigned code, int* predictor, int* index) {
    int step = ima_steps[*index];
    int diff = step >> 3;
    if (code & 4)
        diff += step;
    if (code & 2)
        diff += step >> 1;
    if (code & 1)
        diff += step >> 2;
    *predictor = ima_clamp(*predictor + ((code & 8) ? -diff : diff), -32768, 32767);
    *index = ima_clamp(*index + ima_index_delta[code & 7], 0, 88);
    return *predictor;
}

// Each packet stores sample 0 and the initial step index, so loss never corrupts
// the next packet's decoder state. Remaining samples use low nibble first.
static inline size_t ima_adpcm_encode(const int16_t* pcm, size_t samples, unsigned stride,
                                      uint8_t* out, size_t capacity, uint8_t* next_index) {
    size_t bytes = ima_adpcm_bytes(samples);
    if (!pcm || !out || !next_index || *next_index > 88 || !bytes || capacity < bytes ||
        (stride != 1 && stride != 2))
        return 0;
    int predictor = pcm[0], index = *next_index;
    memset(out, 0, bytes);
    out[0] = (uint8_t)predictor;
    out[1] = (uint8_t)(predictor >> 8);
    out[2] = (uint8_t)index;
    for (size_t i = 1; i < samples; ++i) {
        int difference = (int)pcm[i * stride] - predictor;
        unsigned code = difference < 0 ? 8 : 0;
        if (difference < 0)
            difference = -difference;
        int step = ima_steps[index];
        if (difference >= step) {
            code |= 4;
            difference -= step;
        }
        if (difference >= (step >> 1)) {
            code |= 2;
            difference -= step >> 1;
        }
        if (difference >= (step >> 2))
            code |= 1;
        ima_apply(code, &predictor, &index);
        out[4 + (i - 1) / 2] |= (uint8_t)(code << (((i - 1) & 1) * 4));
    }
    *next_index = (uint8_t)index;
    return bytes;
}

// Write PCM16LE bytes; no alignment assumption or heap allocation.
static inline bool ima_adpcm_decode(const uint8_t* in, size_t bytes, size_t samples, uint8_t* pcm,
                                    size_t capacity) {
    if (!pcm || capacity < samples * 2 || !ima_adpcm_valid(in, bytes, samples))
        return false;
    int predictor = (int16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
    int index = in[2];
    for (size_t i = 0; i < samples; ++i) {
        if (i) {
            unsigned code = (in[4 + (i - 1) / 2] >> (((i - 1) & 1) * 4)) & 15;
            ima_apply(code, &predictor, &index);
        }
        pcm[i * 2] = (uint8_t)predictor;
        pcm[i * 2 + 1] = (uint8_t)(predictor >> 8);
    }
    return true;
}

static inline bool ima_adpcm_selftest(void) {
    // Independent CPython 3.11 audioop reference, initial state (sample0,32).
    static const int16_t input[] = {0,     100,    -100, 1000, -1000, 3000, -3000, 10000, -10000,
                                    32767, -32768, 0,    0,    42,    -42,  1234,  -2345, 0};
    static const uint8_t wire[] = {0, 0, 32, 0, 0xd2, 0xf7, 0xf7, 0xe7, 0xe7, 3, 8, 0x90, 0};
    static const int16_t expected[] = {0,     97,     -98,   293, -548,  1256, -2617, 5685,  -9740,
                                       21793, -31452, -2783, 941, -2444, 633,  3431,  -4199, -1887};
    uint8_t encoded[sizeof(wire)], decoded[sizeof(input)];
    uint8_t index = 32;
    if (ima_adpcm_encode(input, 18, 1, encoded, sizeof(encoded), &index) != sizeof(wire) ||
        memcmp(encoded, wire, sizeof(wire)) ||
        !ima_adpcm_decode(wire, sizeof(wire), 18, decoded, sizeof(decoded)))
        return false;
    for (size_t i = 0; i < 18; ++i)
        if ((int16_t)((uint16_t)decoded[2 * i] | ((uint16_t)decoded[2 * i + 1] << 8)) !=
            expected[i])
            return false;
    encoded[2] = 89;
    if (ima_adpcm_decode(encoded, sizeof(encoded), 18, decoded, sizeof(decoded)))
        return false;
    encoded[2] = 32;
    encoded[sizeof(encoded) - 1] |= 0x80;
    return !ima_adpcm_decode(encoded, sizeof(encoded), 18, decoded, sizeof(decoded)) &&
           !ima_adpcm_decode(wire, sizeof(wire) - 1, 18, decoded, sizeof(decoded)) &&
           !ima_adpcm_decode(wire, sizeof(wire), 18, decoded, sizeof(decoded) - 1);
}

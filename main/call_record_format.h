#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace call_record {
constexpr uint32_t kRate = 16000;
constexpr size_t kFrameSamples = 320;
// JSON escaping can expand each byte to six bytes; ASR text is bounded to 3071.
constexpr size_t kJsonLineBytes = 20 * 1024;

inline bool ReadLine(FILE* file, char* buffer, size_t capacity, bool& invalid) {
    if (!file || !std::fgets(buffer, capacity, file)) {
        invalid |= file && std::ferror(file);
        return false;
    }
    if (!std::strchr(buffer, '\n') && !std::feof(file)) {
        invalid = true;
        return false;
    }
    return true;
}

inline size_t Utf8Prefix(const char* text, size_t maximum) {
    size_t n = std::min(std::strlen(text), maximum);
    while (n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80)
        --n;
    return n;
}
constexpr uint64_t kMaxSamples =
    std::min<uint64_t>((UINT32_MAX - 36U) / 2U, (uint64_t(LONG_MAX) - 44) / 2);

inline bool ValidId(const char* id) {
    if (!id || std::strlen(id) != 17 || id[8] != '-')
        return false;
    for (unsigned i = 0; i < 17; ++i)
        if (i != 8 && !((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
            return false;
    return true;
}

inline void Put16(uint8_t* p, uint16_t n) {
    p[0] = n;
    p[1] = n >> 8;
}
inline void Put32(uint8_t* p, uint32_t n) {
    Put16(p, n);
    Put16(p + 2, n >> 16);
}
inline uint32_t Get32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline bool WavHeader(uint8_t (&h)[44], uint64_t samples) {
    if (samples > kMaxSamples)
        return false;
    std::memset(h, 0, sizeof(h));
    std::memcpy(h, "RIFF", 4);
    Put32(h + 4, 36 + uint32_t(samples * 2));
    std::memcpy(h + 8, "WAVEfmt ", 8);
    Put32(h + 16, 16);
    Put16(h + 20, 1);
    Put16(h + 22, 1);
    Put32(h + 24, kRate);
    Put32(h + 28, kRate * 2);
    Put16(h + 32, 2);
    Put16(h + 34, 16);
    std::memcpy(h + 36, "data", 4);
    Put32(h + 40, samples * 2);
    return true;
}
inline bool ValidWav(const uint8_t (&h)[44], uint64_t bytes) {
    return bytes >= 44 && !std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "WAVEfmt ", 8) &&
           Get32(h + 16) == 16 && h[20] == 1 && h[21] == 0 && h[22] == 1 && h[23] == 0 &&
           Get32(h + 24) == kRate && Get32(h + 28) == kRate * 2 && h[32] == 2 && h[33] == 0 &&
           h[34] == 16 && h[35] == 0 && !std::memcmp(h + 36, "data", 4) && Get32(h + 40) % 2 == 0 &&
           uint64_t(Get32(h + 40)) + 44 <= bytes &&
           uint64_t(Get32(h + 4)) == uint64_t(Get32(h + 40)) + 36;
}
// Preserve consecutive media frames despite bursty packet arrival. Only a real
// absence greater than 100 ms inserts silence; its extent is persisted separately.
inline uint64_t FramePosition(uint64_t written, uint64_t elapsed_ms) {
    const uint64_t clock = elapsed_ms / 20 * kFrameSamples;
    return clock > written + kRate / 10 ? clock : written;
}
// Incoming UDP arrival time includes Wi-Fi/coexistence jitter. Anchor the first
// frame to the call clock, then place frames by their authenticated media
// generation and sequence. A delayed burst must not invent silence or stretch
// the recording; actual missing sequence numbers retain their exact holes.
struct MediaClock {
    uint32_t token = 0, generation = 0, sequence = 0;
    uint64_t origin = 0;
    uint64_t Position(uint32_t call, uint32_t stream, uint32_t frame, uint64_t elapsed_ms) {
        if (call != token || stream != generation || int32_t(frame - sequence) < 0) {
            token = call;
            generation = stream;
            sequence = frame;
            origin = elapsed_ms / 20 * kFrameSamples;
        }
        return origin + uint64_t(uint32_t(frame - sequence)) * kFrameSamples;
    }
};
inline int16_t Mix(int16_t a, int16_t b) {
    return int16_t(std::clamp(int(a) + int(b), -32768, 32767));
}
// FatFS rename cannot replace an existing destination. Keep the previous
// committed file as a recovery copy instead of unlinking the only valid metadata.
inline bool CommitFile(const char* temporary, const char* target) {
    char backup[256];
    if (std::snprintf(backup, sizeof(backup), "%s.bak", target) >= int(sizeof(backup)))
        return false;
    FILE* exists = std::fopen(target, "rb");
    const bool had_target = exists;
    if (exists)
        std::fclose(exists);
    if (had_target) {
        std::remove(backup);
        if (std::rename(target, backup))
            return false;
    }
    if (!std::rename(temporary, target))
        return true;
    if (had_target)
        std::rename(backup, target);
    return false;
}
}  // namespace call_record

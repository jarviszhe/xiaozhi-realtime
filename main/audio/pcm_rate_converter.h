#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>
#include "esp_ae_rate_cvt.h"
#include "esp_log.h"

// Caller serializes processing and recreation of the handle and starts a new
// stream when its input frame size changes. The 1.3.0~1 streaming phase does not
// safely handle mixed frame sizes; increasing output capacity only delays it.
inline bool ConvertPcmRate(esp_ae_rate_cvt_handle_t handle, const std::vector<int16_t>& input,
                           unsigned channels, std::vector<int16_t>& output) {
    uint32_t advertised = 0;
    if (!handle || !channels || input.empty() || input.size() % channels ||
        input.size() / channels > std::numeric_limits<uint32_t>::max() ||
        esp_ae_rate_cvt_get_max_out_sample_num(handle, input.size() / channels, &advertised) !=
            ESP_AE_ERR_OK ||
        advertised == std::numeric_limits<uint32_t>::max()) {
        ESP_LOGE("PcmRateConverter", "Invalid input or output capacity query failed");
        output.clear();
        return false;
    }
    const uint32_t capacity = advertised;
#if CONFIG_HEAP_POISONING_LIGHT
    constexpr size_t guard_elements = 1024;
#else
    constexpr size_t guard_elements = 0;
#endif
    if (capacity >
        (std::numeric_limits<size_t>::max() / sizeof(int16_t) - guard_elements) / channels) {
        ESP_LOGE("PcmRateConverter", "Output capacity overflow");
        output.clear();
        return false;
    }
    const size_t elements = size_t(capacity) * channels;
    std::vector<int16_t> converted(elements + guard_elements);
    std::fill(converted.begin() + elements, converted.end(), int16_t(0x5a5a));
    uint32_t actual = capacity;
    const auto result =
        esp_ae_rate_cvt_process(handle, (esp_ae_sample_t)input.data(), input.size() / channels,
                                (esp_ae_sample_t)converted.data(), &actual);
    const bool guard_ok = std::all_of(converted.begin() + elements, converted.end(),
                                      [](int16_t sample) { return sample == 0x5a5a; });
    if (result != ESP_AE_ERR_OK || actual > capacity || !guard_ok) {
        ESP_LOGE("PcmRateConverter",
                 "Conversion failed capacity=%lu actual=%lu guard_ok=%d result=%d",
                 (unsigned long)capacity, (unsigned long)actual, guard_ok, int(result));
        output.clear();
        return false;
    }
    converted.resize(size_t(actual) * channels);
    output = std::move(converted);
    return true;
}

#pragma once

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <memory>

namespace call_record {
inline void AllocationFailure(const char* purpose, size_t bytes, unsigned caps) {
    const unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const unsigned external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGE("call_records",
             "allocation=%s bytes=%u caps=0x%x internal_free=%u min=%u largest=%u "
             "psram_free=%u largest=%u",
             purpose, unsigned(bytes), caps, unsigned(heap_caps_get_free_size(internal)),
             unsigned(heap_caps_get_minimum_free_size(internal)),
             unsigned(heap_caps_get_largest_free_block(internal)),
             unsigned(heap_caps_get_free_size(external)),
             unsigned(heap_caps_get_largest_free_block(external)));
}
struct BufferDeleter {
    void operator()(char* p) const { heap_caps_free(p); }
};
inline std::unique_ptr<char, BufferDeleter> Buffer(size_t bytes, const char* purpose) {
    const unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    char* p = static_cast<char*>(heap_caps_malloc(bytes, caps));
    if (!p)
        AllocationFailure(purpose, bytes, caps);
    return std::unique_ptr<char, BufferDeleter>(p);
}
}  // namespace call_record

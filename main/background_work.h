#pragma once

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <functional>
#include <new>

// Reviewed compute/network workers may use external stacks; NVS/flash and display/DMA tasks
// deliberately do not use this helper. Callers must review their call chains.
inline bool StartBackgroundWork(const char* name, uint32_t bytes, UBaseType_t priority,
                                std::function<void()> work, bool external = true) {
    auto* job = new (std::nothrow) std::function<void()>(std::move(work));
    BaseType_t result = pdFAIL;
    uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_SPIRAM
    if (external && heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) {
        caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        if (job)
            result = xTaskCreateWithCaps(
                [](void* arg) {
                    auto* fn = static_cast<std::function<void()>*>(arg);
                    (*fn)();
                    delete fn;
                    vTaskDeleteWithCaps(nullptr);
                },
                name, bytes, job, priority, nullptr, caps);
    } else
#endif
    {
        if (job)
            result = xTaskCreate(
                [](void* arg) {
                    auto* fn = static_cast<std::function<void()>*>(arg);
                    (*fn)();
                    delete fn;
                    vTaskDelete(nullptr);
                },
                name, bytes, job, priority, nullptr);
    }
    if (result != pdPASS) {
        delete job;
        const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        const uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        ESP_LOGE("background",
                 "task=%s bytes=%u caps=0x%lx internal_free=%u min=%u largest=%u psram_free=%u "
                 "largest=%u",
                 name, unsigned(bytes), (unsigned long)caps,
                 unsigned(heap_caps_get_free_size(internal)),
                 unsigned(heap_caps_get_minimum_free_size(internal)),
                 unsigned(heap_caps_get_largest_free_block(internal)),
                 unsigned(heap_caps_get_free_size(psram)),
                 unsigned(heap_caps_get_largest_free_block(psram)));
        return false;
    }
    return true;
}

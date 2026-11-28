#include "gateway_serial.h"
#include <cstdio>
#include <cstring>
#include "board.h"
#include "call_records.h"
#include "display.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_manager.h"
#if CONFIG_PHONE_BRIDGE
#include "application.h"
#endif

namespace {
bool HandlePhoneCommand(const char* line) {
    if (!std::strcmp(line, "phone help")) {
        puts(
            "PHONE_COMMANDS status | answer | reject | hangup | audio on|off | auto on|off|cancel");
        return true;
    }
    if (!std::strcmp(line, "phone status")) {
        gateway_manager_command("gateway status");
        gateway_manager_print_phone_status();
        return true;
    }
    uint8_t command = 0;
    if (!std::strcmp(line, "phone answer"))
        command = GATEWAY_PHONE_CMD_ANSWER;
    else if (!std::strcmp(line, "phone reject") || !std::strcmp(line, "phone hangup"))
        command = GATEWAY_PHONE_CMD_REJECT;
    else if (!std::strcmp(line, "phone audio on"))
        command = GATEWAY_PHONE_CMD_AUDIO_ON;
    else if (!std::strcmp(line, "phone audio off"))
        command = GATEWAY_PHONE_CMD_AUDIO_OFF;
    else if (!std::strcmp(line, "phone auto cancel"))
        command = GATEWAY_PHONE_CMD_AUTO_CANCEL;
    else if (!std::strcmp(line, "phone auto on"))
        command = GATEWAY_PHONE_CMD_AUTO_ON;
    else if (!std::strcmp(line, "phone auto off"))
        command = GATEWAY_PHONE_CMD_AUTO_OFF;
    else
        return false;
    printf("PHONE_COMMAND %s\n", gateway_manager_phone_command(command)
                                     ? "queued"
                                     : "rejected_busy_offline_unsupported_or_no_call");
    return true;
}

void ConsoleTask(void*) {
    char line[96];
    size_t used = 0;
    bool overflow = false;
    while (true) {
        char c;
        int count = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(50));
        if (count != 1)
            continue;
        if (c == '\r' || c == '\n') {
            if (used && !overflow) {
                line[used] = 0;
                if (!std::strcmp(line, "gateway netdiag")) {
                    wifi_ap_record_t ap{};
                    wifi_ps_type_t ps{};
                    const auto ap_result = esp_wifi_sta_get_ap_info(&ap);
                    const auto ps_result = esp_wifi_get_ps(&ps);
                    int tx_static = 0, rx_ba = 0;
#ifdef CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM
                    tx_static = CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM;
#endif
#ifdef CONFIG_ESP_WIFI_RX_BA_WIN
                    rx_ba = CONFIG_ESP_WIFI_RX_BA_WIN;
#endif
                    printf(
                        "GATEWAY_NETDIAG associated=%d rssi=%d channel=%u ps=%d "
                        "ps_result=%d rx_static=%d rx_dynamic=%d tx_static=%d rx_ba=%d\n",
                        ap_result == ESP_OK, ap.rssi, ap.primary, int(ps), int(ps_result),
                        CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM, CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM,
                        tx_static, rx_ba);
                    printf("GATEWAY_NETDIAG dump_result=%d\n",
                           int(esp_wifi_statis_dump(UINT32_MAX)));
                    used = 0;
                    overflow = false;
                    continue;
                }
                if (!std::strncmp(line, "ui ", 3)) {
                    if (!Board::GetInstance().GetDisplay()->HandleUiDebugCommand(line))
                        puts("UI_ERROR unsupported_command");
                    printf("UI_COMMAND_END stack_free=%u\n",
                           unsigned(uxTaskGetStackHighWaterMark(nullptr)));
                    if (!std::strcmp(line, "ui status")) {
                        // Check before walking heap statistics: a corrupted pool
                        // must fail here with its block address, not later as an
                        // unrelated LoadProhibited in the statistics walker.
                        configASSERT(heap_caps_check_integrity_all(true));
                        constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
                        constexpr uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
                        printf(
                            "UI_MEMORY internal_free=%u internal_min=%u internal_largest=%u "
                            "psram_free=%u psram_largest=%u\n",
                            unsigned(heap_caps_get_free_size(internal)),
                            unsigned(heap_caps_get_minimum_free_size(internal)),
                            unsigned(heap_caps_get_largest_free_block(internal)),
                            unsigned(heap_caps_get_free_size(external)),
                            unsigned(heap_caps_get_largest_free_block(external)));
                    }
                    used = 0;
                    overflow = false;
                    continue;
                }
#if CONFIG_PHONE_BRIDGE
                if (Application::GetInstance().GetAudioService().HandlePhoneDebugCommand(line)) {
                    used = 0;
                    overflow = false;
                    continue;
                }
#endif
                if (CallRecords::Instance().Command(line) || HandlePhoneCommand(line)) {
                    used = 0;
                    overflow = false;
                    continue;
                }
                if (!gateway_manager_command(line))
                    puts("GATEWAY_ERROR expected_gateway_command");
            } else if (overflow) {
                puts("GATEWAY_ERROR command_too_long");
            }
            used = 0;
            overflow = false;
        } else if (c == '\b' || c == 127) {
            if (used)
                --used;
        } else if (used < sizeof(line) - 1) {
            line[used++] = c;
        } else {
            overflow = true;
        }
    }
}
}  // namespace

void StartGatewaySerial() {
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK) {
        ESP_LOGE("gateway_serial", "USB console install failed: %s", esp_err_to_name(err));
        return;
    }
    // Console logs and checked snapshot lines must share the same interrupt-driven
    // TX queue. Leaving VFS in its default polling mode races direct FIFO writes
    // against the installed driver's ISR, corrupting both streams under load.
    usb_serial_jtag_vfs_use_driver();
    // This task only queues gateway/application work or uses thread-safe USB,
    // heap and LVGL APIs. It never performs NVS/flash writes or ISR work.
    // LVGL events/snapshots need more stack than the former text-only console.
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_SPIRAM
    constexpr size_t stack_size = 12288;
    constexpr uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    BaseType_t result =
        xTaskCreateWithCaps(ConsoleTask, "gateway_console", stack_size, nullptr, 2, nullptr, caps);
#else
    constexpr size_t stack_size = 8192;
    constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    BaseType_t result =
        xTaskCreate(ConsoleTask, "gateway_console", stack_size, nullptr, 2, nullptr);
#endif
    if (result != pdPASS)
        ESP_LOGE("gateway_serial",
                 "console allocation bytes=%u caps=%u internal_free=%u minimum=%u largest=%u "
                 "psram_free=%u largest=%u",
                 unsigned(stack_size), unsigned(caps),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                 unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

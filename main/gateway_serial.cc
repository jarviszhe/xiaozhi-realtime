#include "gateway_serial.h"
#include <cstdio>
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_manager.h"
#if CONFIG_PHONE_BRIDGE
#include "application.h"
#endif

namespace {
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
#if CONFIG_PHONE_BRIDGE
                if (Application::GetInstance().GetAudioService().HandlePhoneDebugCommand(line)) {
                    used = 0;
                    overflow = false;
                    continue;
                }
#endif
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
    if (xTaskCreate(ConsoleTask, "gateway_console", 4096, nullptr, 2, nullptr) != pdPASS)
        ESP_LOGE("gateway_serial", "console task allocation failed");
}

#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include "lcd_display.h"
#if CONFIG_GATEWAY_MANAGER
#include "phone_settings_view.h"
#endif

// // SPI LCD显示器
class CustomLcdDisplay : public LcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy);
    void SetupUI() override;
    void SetTheme(Theme* theme) override;
    bool HandleUiDebugCommand(const char* command) override;
    void AddPhoneTranscript(bool user, const char* text) override;

private:
#if CONFIG_GATEWAY_MANAGER
    std::unique_ptr<PhoneSettingsView> phone_settings_;
    bool ExportUiSnapshot();
#endif
    static bool lvgl_port_flush_io_ready_callback(esp_lcd_panel_io_handle_t panel_io,
                                                  esp_lcd_panel_io_event_data_t* edata,
                                                  void* user_ctx);
    static void lvgl_port_flush_callback(lv_display_t* drv, const lv_area_t* area,
                                         uint8_t* color_map);
};

#endif  // __CUSTOM_LCD_DISPLAY_H__

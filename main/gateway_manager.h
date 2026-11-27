#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool (*busy)(void);
    esp_err_t (*configure)(const char* ssid, const char* password);
    esp_err_t (*clear_wifi)(void);
    void (*peer)(uint32_t ip, bool ready);
    void (*media)(uint64_t* session, uint32_t* generation, unsigned* rate, int* hfp, int* sco,
                  uint32_t* rx, uint32_t* tx);
} gateway_manager_hooks_t;

// Gateway calls this after enabling Bluedroid; the S3 owns its BLE initialization.
esp_err_t gateway_manager_start(bool host, const gateway_manager_hooks_t* hooks);
bool gateway_manager_command(const char* line);
bool gateway_manager_accepts(uint32_t ip);
bool gateway_manager_ready(void);
// Peer receive capabilities from the authenticated control channel; legacy=PCM.
uint8_t gateway_manager_peer_audio_codecs(void);
void gateway_manager_note_media(bool transmit);
#ifdef __cplusplus
}
#endif

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "phone_control.h"

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
    esp_err_t (*phone_command)(uint8_t command, uint32_t call_id);
    void (*phone_status)(phone_status_t* status);
} gateway_manager_hooks_t;

// Gateway calls this after enabling Bluedroid; the S3 owns its BLE initialization.
esp_err_t gateway_manager_start(bool host, const gateway_manager_hooks_t* hooks);
bool gateway_manager_command(const char* line);
bool gateway_manager_accepts(uint32_t ip);
bool gateway_manager_ready(void);
// Peer receive capabilities from the authenticated control channel; legacy=PCM.
uint8_t gateway_manager_peer_audio_codecs(void);
void gateway_manager_note_media(bool transmit);
// Queue one idempotent phone command for the next authenticated heartbeat.
// Returns false when the previous command is still awaiting an ACK.
bool gateway_manager_phone_command(uint8_t command);
// Typed, fresh authenticated call state for serial and future UI consumers.
bool gateway_manager_get_phone_status(phone_status_t* status, int64_t* age_ms,
                                      uint32_t* duration_ms);
void gateway_manager_print_phone_status(void);
// Public, bounded UI snapshot: contains no credentials, keys or BLE payloads.
#define GATEWAY_UI_MAX_PEERS 8
typedef struct {
    char id[16];
    int rssi;
} gateway_ui_peer_t;
typedef struct {
    bool available, wifi, bound, ready, remote_fresh, scanning, provisioning, ble, config_sent;
    uint8_t hfp, sco;
    uint32_t scan_serial, scan_done_serial, remaining_ms;
    unsigned count;
    char peer_id[16], target_id[16], stage[32], error[48];
    gateway_ui_peer_t peers[GATEWAY_UI_MAX_PEERS];
} gateway_ui_status_t;
bool gateway_manager_get_ui_status(gateway_ui_status_t* status);
#ifdef __cplusplus
}
#endif

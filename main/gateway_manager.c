// SPDX-License-Identifier: MIT
// Protocol authority: docs/protocol/gateway-management-v1.md in Bluetooth-gateway.
#include "gateway_manager.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_gatts_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/platform_util.h"
#include "mdns.h"
#include "nvs.h"
#include "psa/crypto.h"
#ifndef CONFIG_GATEWAY_DEBUG_COMMANDS
#define CONFIG_GATEWAY_DEBUG_COMMANDS 0
#endif
#if CONFIG_IDF_TARGET_ESP32
#include "led_strip.h"
#endif

#define GM_SERVICE 0xFFF0
#define GM_CHAR 0xFFF1
#define GM_PORT 39002
#define GM_MEDIA_PORT 39001
#define GM_NS "gateway_pair"
#define GM_MAX_PEERS 8
enum {
    P_BEGIN = 1,
    P_CONFIG = 2,
    P_CONFIRM = 3,
    P_AUTHORIZED = 4,
    P_CONNECTING = 5,
    P_DONE = 6,
    P_ERROR = 7
};
enum { C_HELLO = 1, C_CHALLENGE, C_AUTH, C_READY, C_PING, C_PONG };
typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t op, flags;
    uint16_t reserved;
    uint8_t node[6], target[6], nonce[16], key[32];
    char ssid[33], password[65];
    uint8_t proof[32];
} provision_packet_t;
typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t type, version;
    uint16_t port;
    uint8_t node[6], target[6], client_nonce[16], server_nonce[16];
    uint64_t session;
    uint32_t generation, sequence, rx, tx;
    uint16_t rate;
    uint8_t hfp, sco;
    uint8_t reserved[16];
    uint8_t proof[32];
} control_packet_t;
typedef struct {
    uint32_t version;
    uint8_t peer[6], key[32];
    char ssid[33], password[65];
} binding_t;
typedef struct {
    int kind;
    uint16_t conn;
    uint16_t reason;
    uint8_t address[6];
    provision_packet_t packet;
    char command[96];
} event_t;
enum {
    E_COMMAND = 1,
    E_WRITE,
    E_REMOTE,
    E_DISCONNECT,
    E_AUTH,
    E_SHORT,
    E_LONG,
    E_LINK_READY,
    E_LINK_LOST,
    E_DISCOVERY_FAILED,
    E_DISCOVERY_STARTED
};
typedef struct {
    uint8_t address[6];
    uint8_t type;
    int rssi;
    char id[16];
} found_t;
static const char* TAG = "gateway_manager";
static bool s_host;
static atomic_bool s_bound, s_ready, s_encrypted, s_connected, s_adv, s_scanning;
static atomic_bool s_request, s_candidate;
static bool s_config_sent, s_reprovision, s_recovery_scan;
static uint8_t s_id[6], s_remote[6];
static char s_name[20], s_peer_id[16], s_stage[32] = "starting", s_error[48];
static gateway_manager_hooks_t s_hooks;
static binding_t s_binding, s_pending_binding, s_alternate;
static bool s_has_alternate;
static atomic_uint s_last_media, s_media_rx, s_media_tx;
static control_packet_t s_remote_status;
static atomic_bool s_have_remote;
static provision_packet_t s_pending, s_readback;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_mutex;
static esp_gatt_if_t s_if = ESP_GATT_IF_NONE;
static uint16_t s_conn, s_handle, s_start, s_end, s_mtu = 23;
static uint32_t s_peer_ip;
static int64_t s_window, s_deadline, s_last_poll, s_error_until, s_success_until;
static int64_t s_candidate_online_since;
static atomic_llong s_last_link;
static found_t s_found[GM_MAX_PEERS];
static unsigned s_found_count;
static found_t s_recovery_candidate;
static bool s_recovery_candidate_valid;
static int s_listener = -1;
static atomic_bool s_mdns;
static atomic_uint s_mdns_failures;
#if CONFIG_IDF_TARGET_ESP32
static led_strip_handle_t s_led;
#endif
static char s_led_name[24] = "off";

_Static_assert(sizeof(provision_packet_t) == 198, "provision wire size");
_Static_assert(sizeof(control_packet_t) == 128, "control wire size");
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static bool zero(const void* p, size_t n) {
    const uint8_t* b = p;
    uint8_t v = 0;
    for (size_t i = 0; i < n; ++i)
        v |= b[i];
    return !v;
}
static void id_text(const uint8_t* id, char* out) {
    snprintf(out, 16, "GW-%02X%02X%02X%02X%02X%02X", id[0], id[1], id[2], id[3], id[4], id[5]);
}
static void stage(const char* value) {
    if (strcmp(s_stage, value))
        ESP_LOGI(TAG, "stage=%s", value);
    strlcpy(s_stage, value, sizeof(s_stage));
}
static void fail(const char* why) {
    strlcpy(s_error, why, sizeof(s_error));
    s_error_until = now_ms() + 5000;
    ESP_LOGW(TAG, "error=%s", why);
}
static bool hmac(const uint8_t* key, const void* data, size_t len, uint8_t* out) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t id = 0;
    size_t written = 0;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_status_t err = psa_import_key(&attr, key, 32, &id);
    if (err == PSA_SUCCESS)
        err = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, len, out, 32, &written);
    psa_destroy_key(id);
    psa_reset_key_attributes(&attr);
    return err == PSA_SUCCESS && written == 32;
}
static bool proof(const uint8_t* key, const void* data, size_t len, const uint8_t* signature) {
    uint8_t out[32];
    if (!hmac(key, data, len, out))
        return false;
    unsigned difference = 0;
    for (unsigned i = 0; i < 32; ++i)
        difference |= out[i] ^ signature[i];
    mbedtls_platform_zeroize(out, sizeof(out));
    return difference == 0;
}
static bool crypto_selftest(void) {
    static const uint8_t expected[32] = {0xf8, 0x7c, 0xb2, 0x74, 0xc0, 0x53, 0x60, 0x52,
                                         0x22, 0x66, 0xab, 0x89, 0x48, 0x87, 0xdb, 0x8c,
                                         0x34, 0xae, 0xb5, 0x39, 0x69, 0xef, 0x7c, 0x28,
                                         0x72, 0x94, 0x5d, 0x13, 0xbd, 0xd8, 0x52, 0xcb};
    uint8_t key[32], corrupt[32];
    const char* message = "gateway-management-v1";
    for (unsigned i = 0; i < sizeof(key); ++i)
        key[i] = i;
    if (!proof(key, message, strlen(message), expected))
        return false;
    for (unsigned i = 0; i < sizeof(key); ++i) {
        memcpy(corrupt, expected, sizeof(corrupt));
        corrupt[i] ^= 1;
        if (proof(key, message, strlen(message), corrupt))
            return false;
    }
    return true;
}
static bool store_named(const char* name, const binding_t* binding) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(GM_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
        return false;
    err = nvs_set_blob(nvs, name, binding, sizeof(*binding));
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}
static bool store(const binding_t* binding) { return store_named("binding", binding); }
static void publish_binding(const binding_t* binding) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_binding = *binding;
    s_bound = true;
    s_has_alternate = false;
    xSemaphoreGive(s_mutex);
    id_text(s_binding.peer, s_peer_id);
}
static bool busy(void) {
    uint32_t at = atomic_load(&s_last_media);
    return (s_host && at && (uint32_t)now_ms() - at < 6000) || (s_hooks.busy && s_hooks.busy());
}
void gateway_manager_note_media(bool transmit) {
    if (transmit)
        atomic_fetch_add(&s_media_tx, 1);
    else {
        atomic_store(&s_last_media, (uint32_t)now_ms());
        atomic_fetch_add(&s_media_rx, 1);
    }
}
static void load(void) {
    nvs_handle_t nvs;
    size_t size = sizeof(s_binding);
    if (nvs_open(GM_NS, NVS_READONLY, &nvs) == ESP_OK) {
        esp_err_t err = nvs_get_blob(nvs, "binding", &s_binding, &size);
        s_bound = err == ESP_OK && size == sizeof(s_binding) && s_binding.version == 1 &&
                  !zero(s_binding.key, 32) && !zero(s_binding.peer, 6) &&
                  memchr(s_binding.ssid, 0, sizeof(s_binding.ssid)) &&
                  memchr(s_binding.password, 0, sizeof(s_binding.password));
        if (s_host) {
            binding_t candidate = {0};
            size_t candidate_size = sizeof(candidate);
            if (nvs_get_blob(nvs, "pending", &candidate, &candidate_size) == ESP_OK &&
                candidate_size == sizeof(candidate) && candidate.version == 1 &&
                !zero(candidate.key, 32) && !zero(candidate.peer, 6)) {
                if (s_bound && memcmp(s_binding.key, candidate.key, 32)) {
                    s_alternate = s_binding;
                    s_has_alternate = true;
                }
                s_binding = candidate;
                s_bound = true;
            }
        }
        nvs_close(nvs);
    }
    if (!s_bound)
        memset(&s_binding, 0, sizeof(s_binding));
}
static bool wifi_online(void) {
    wifi_ap_record_t ap;
    esp_netif_ip_info_t info;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return netif && esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
           esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr;
}
static void peer(uint32_t ip, bool ready) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_peer_ip = ready ? ip : 0;
    s_ready = ready;
    xSemaphoreGive(s_mutex);
    if (s_hooks.peer)
        s_hooks.peer(ip, ready);
}
bool gateway_manager_accepts(uint32_t ip) {
    if (!s_mutex)
        return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_ready && s_peer_ip == ip;
    xSemaphoreGive(s_mutex);
    return ok;
}
bool gateway_manager_ready(void) {
    if (!s_mutex)
        return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ready = s_ready;
    xSemaphoreGive(s_mutex);
    return ready;
}
static void status(void) {
    char id[16], other[16], address[16];
    control_packet_t remote;
    bool have_remote;
    id_text(s_id, id);
    id_text(s_binding.peer, other);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    remote = s_remote_status;
    have_remote = s_have_remote;
    xSemaphoreGive(s_mutex);
    struct in_addr ip = {.s_addr = s_peer_ip};
    inet_ntoa_r(ip, address, sizeof(address));
    int64_t link = atomic_load(&s_last_link);
    int64_t age = link ? now_ms() - link : -1;
    printf(
        "GATEWAY {\"id\":\"%s\",\"role\":\"%s\",\"bound\":%s,"
        "\"peer_id\":\"%s\",\"wifi\":%s,\"ready\":%s,\"peer_ip\":\"%s\","
        "\"stage\":\"%s\",\"ble\":%s,\"encrypted\":%s,\"window\":%s,"
        "\"led\":\"%s\",\"last_error\":\"%s\",\"uptime_ms\":%" PRId64 ",\"link_age_ms\":%" PRId64
        ",\"remote_fresh\":%s,\"remote_hfp\":%u"
        ",\"remote_sco\":%u,\"remote_rate\":%u,\"remote_rx\":%" PRIu32 ",\"remote_tx\":%" PRIu32
        ",\"media_rx\":%" PRIu32 ",\"media_tx\":%" PRIu32
        ",\"mdns\":%s,\"mdns_failures\":%u,\"internal_free\":%u,\"internal_largest\":%u"
        ",\"psram_free\":%u}\n",
        id, s_host ? "s3" : "gateway", s_bound ? "true" : "false", other,
        wifi_online() ? "true" : "false", s_ready ? "true" : "false", address, s_stage,
        s_connected ? "true" : "false", s_encrypted ? "true" : "false",
        now_ms() < s_window ? "true" : "false", s_led_name, s_error, now_ms(), age,
        have_remote && age >= 0 && age < 7000 ? "true" : "false", remote.hfp, remote.sco,
        remote.rate, remote.rx, remote.tx, (uint32_t)atomic_load(&s_media_rx),
        (uint32_t)atomic_load(&s_media_tx), s_mdns ? "true" : "false",
        (unsigned)atomic_load(&s_mdns_failures),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
static bool enqueue(event_t* event) { return s_queue && xQueueSend(s_queue, event, 0) == pdTRUE; }
bool gateway_manager_command(const char* line) {
    if (strncmp(line, "gateway ", 8))
        return false;
    event_t event = {.kind = E_COMMAND};
    if (strlen(line) >= sizeof(event.command)) {
        puts("GATEWAY_ERROR command_too_long");
        return true;
    }
    strlcpy(event.command, line, sizeof(event.command));
    if (!enqueue(&event))
        puts("GATEWAY_ERROR busy");
    return true;
}
static esp_ble_adv_params_t adv_params = {.adv_int_min = 0x100,
                                          .adv_int_max = 0x180,
                                          .adv_type = ADV_TYPE_IND,
                                          .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                                          .channel_map = ADV_CHNL_ALL,
                                          .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY};
static void advertising(bool enable) {
    ESP_LOGI(TAG, "BLE advertising request enable=%d current=%d name=%s", enable, s_adv, s_name);
    if (enable == s_adv)
        return;
    esp_err_t err =
        enable ? esp_ble_gap_start_advertising(&adv_params) : esp_ble_gap_stop_advertising();
    ESP_LOGI(TAG, "BLE advertising %s result=%s", enable ? "start" : "stop", esp_err_to_name(err));
    if (err == ESP_OK)
        s_adv = enable;
}
static void gap(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* p) {
    if (event == ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT && !s_host) {
        ESP_LOGI(TAG, "BLE adv data configured; starting commissioning window");
        event_t e = {.kind = E_COMMAND};
        strlcpy(e.command, "gateway boot", sizeof(e.command));
        enqueue(&e);
    } else if (event == ESP_GAP_BLE_SCAN_RESULT_EVT && s_host &&
               p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
        uint8_t length = 0;
        uint8_t* name =
            esp_ble_resolve_adv_data(p->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &length);
        if (!name)
            name =
                esp_ble_resolve_adv_data(p->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_SHORT, &length);
        ESP_LOGI(TAG, "BLE scan adv addr=%02x:%02x:%02x:%02x:%02x:%02x len=%u rssi=%d name=%.*s",
                 p->scan_rst.bda[0], p->scan_rst.bda[1], p->scan_rst.bda[2], p->scan_rst.bda[3],
                 p->scan_rst.bda[4], p->scan_rst.bda[5], length, p->scan_rst.rssi,
                 name ? length : 0, name ? (char*)name : "");
        if (!name || length < 3 || memcmp(name, "GW-", 3))
            return;
        // Callback only copies bounded advertisements; the worker owns the table.
        event_t e = {.kind = E_COMMAND};
        memcpy(e.address, p->scan_rst.bda, 6);
        snprintf(e.command, sizeof(e.command), "found %.*s %u %d", length, name,
                 p->scan_rst.ble_addr_type, p->scan_rst.rssi);
        enqueue(&e);
    } else if (event == ESP_GAP_BLE_SEC_REQ_EVT) {
        esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, true);
    } else if (event == ESP_GAP_BLE_AUTH_CMPL_EVT) {
        event_t e = {.kind = E_AUTH};
        memcpy(e.address, p->ble_security.auth_cmpl.bd_addr, 6);
        e.packet.flags = p->ble_security.auth_cmpl.success &&
                         (p->ble_security.auth_cmpl.auth_mode & ESP_LE_AUTH_REQ_SC_ONLY);
        enqueue(&e);
    }
}
static void gatts(esp_gatts_cb_event_t event, esp_gatt_if_t interface,
                  esp_ble_gatts_cb_param_t* p) {
    if (event == ESP_GATTS_REG_EVT && p->reg.status == ESP_GATT_OK) {
        s_if = interface;
        esp_gatt_srvc_id_t service = {.is_primary = true};
        service.id.uuid.len = ESP_UUID_LEN_16;
        service.id.uuid.uuid.uuid16 = GM_SERVICE;
        esp_ble_gatts_create_service(interface, &service, 4);
    } else if (event == ESP_GATTS_CREATE_EVT && p->create.status == ESP_GATT_OK) {
        s_start = p->create.service_handle;
        esp_ble_gatts_start_service(s_start);
        esp_bt_uuid_t uuid = {.len = ESP_UUID_LEN_16, .uuid.uuid16 = GM_CHAR};
        esp_attr_control_t control = {.auto_rsp = ESP_GATT_RSP_BY_APP};
        esp_attr_value_t value = {.attr_max_len = sizeof(provision_packet_t)};
        esp_ble_gatts_add_char(
            s_start, &uuid, ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
            ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE, &value, &control);
    } else if (event == ESP_GATTS_ADD_CHAR_EVT) {
        s_handle = p->add_char.attr_handle;
        esp_ble_gap_set_device_name(s_name);
        uint8_t adv[24] = {2, 1, 6, 3, 3, 0xF0, 0xFF, 16, 9};
        memcpy(adv + 9, s_name, 15);
        esp_ble_gap_config_adv_data_raw(adv, sizeof(adv));
    } else if (event == ESP_GATTS_CONNECT_EVT) {
        s_conn = p->connect.conn_id;
        s_connected = true;
        s_adv = false;
        s_encrypted = false;
        memcpy(s_remote, p->connect.remote_bda, 6);
        esp_ble_set_encryption(s_remote, ESP_BLE_SEC_ENCRYPT_NO_MITM);
    } else if (event == ESP_GATTS_MTU_EVT) {
        s_mtu = p->mtu.mtu;
    } else if (event == ESP_GATTS_DISCONNECT_EVT) {
        s_connected = false;
        s_encrypted = false;
        event_t e = {
            .kind = E_DISCONNECT, .conn = p->disconnect.conn_id, .reason = p->disconnect.reason};
        enqueue(&e);
    } else if (event == ESP_GATTS_READ_EVT) {
        esp_gatt_rsp_t rsp = {0};
        rsp.attr_value.handle = p->read.handle;
        esp_gatt_status_t result = ESP_GATT_OK;
        if (p->read.handle != s_handle || p->read.offset || !s_encrypted)
            result = ESP_GATT_READ_NOT_PERMIT;
        else {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            memcpy(rsp.attr_value.value, &s_readback, sizeof(s_readback));
            xSemaphoreGive(s_mutex);
            rsp.attr_value.len = sizeof(s_readback);
        }
        esp_ble_gatts_send_response(interface, p->read.conn_id, p->read.trans_id, result, &rsp);
    } else if (event == ESP_GATTS_WRITE_EVT) {
        esp_gatt_status_t result = ESP_GATT_WRITE_NOT_PERMIT;
        if (p->write.handle == s_handle && !p->write.is_prep && !p->write.offset &&
            p->write.len == sizeof(provision_packet_t) && s_encrypted) {
            event_t e = {.kind = E_WRITE, .conn = p->write.conn_id};
            memcpy(&e.packet, p->write.value, sizeof(e.packet));
            result = enqueue(&e) ? ESP_GATT_OK : ESP_GATT_NO_RESOURCES;
        }
        if (p->write.need_rsp)
            esp_ble_gatts_send_response(interface, p->write.conn_id, p->write.trans_id, result,
                                        NULL);
    } else if (event == ESP_GATTS_EXEC_WRITE_EVT) {
        esp_ble_gatts_send_response(interface, p->exec_write.conn_id, p->exec_write.trans_id,
                                    ESP_GATT_REQ_NOT_SUPPORTED, NULL);
    }
}
static void gattc(esp_gattc_cb_event_t event, esp_gatt_if_t interface,
                  esp_ble_gattc_cb_param_t* p) {
    if (event == ESP_GATTC_REG_EVT && p->reg.status == ESP_GATT_OK) {
        s_if = interface;
        esp_ble_scan_params_t scan = {.scan_type = BLE_SCAN_TYPE_ACTIVE,
                                      .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                                      .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
                                      .scan_interval = 0x100,
                                      .scan_window = 0x50,
                                      .scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE};
        esp_ble_gap_set_scan_params(&scan);
    } else if (event == ESP_GATTC_CONNECT_EVT) {
        s_conn = p->connect.conn_id;
        s_connected = true;
        s_encrypted = false;
        memcpy(s_remote, p->connect.remote_bda, 6);
        esp_ble_gattc_send_mtu_req(interface, s_conn);
    } else if (event == ESP_GATTC_CFG_MTU_EVT && p->cfg_mtu.status == ESP_GATT_OK) {
        s_mtu = p->cfg_mtu.mtu;
        esp_bt_uuid_t uuid = {.len = ESP_UUID_LEN_16, .uuid.uuid16 = GM_SERVICE};
        esp_ble_gattc_search_service(interface, s_conn, &uuid);
    } else if (event == ESP_GATTC_SEARCH_RES_EVT &&
               p->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
               p->search_res.srvc_id.uuid.uuid.uuid16 == GM_SERVICE) {
        s_start = p->search_res.start_handle;
        s_end = p->search_res.end_handle;
    } else if (event == ESP_GATTC_SEARCH_CMPL_EVT && p->search_cmpl.status == ESP_GATT_OK &&
               s_start) {
        esp_gattc_char_elem_t element;
        uint16_t count = 1;
        esp_bt_uuid_t uuid = {.len = ESP_UUID_LEN_16, .uuid.uuid16 = GM_CHAR};
        if (esp_ble_gattc_get_char_by_uuid(interface, s_conn, s_start, s_end, uuid, &element,
                                           &count) == ESP_GATT_OK &&
            count) {
            s_handle = element.char_handle;
            esp_ble_set_encryption(s_remote, ESP_BLE_SEC_ENCRYPT_NO_MITM);
        }
    } else if (event == ESP_GATTC_READ_CHAR_EVT) {
        if (p->read.status == ESP_GATT_OK && p->read.value_len == sizeof(provision_packet_t)) {
            event_t e = {.kind = E_REMOTE};
            memcpy(&e.packet, p->read.value, sizeof(e.packet));
            enqueue(&e);
        }
    } else if (event == ESP_GATTC_DISCONNECT_EVT) {
        s_connected = false;
        s_encrypted = false;
        event_t e = {
            .kind = E_DISCONNECT, .conn = p->disconnect.conn_id, .reason = p->disconnect.reason};
        enqueue(&e);
    } else if (event == ESP_GATTC_OPEN_EVT && p->open.status != ESP_GATT_OK) {
        event_t e = {.kind = E_DISCONNECT, .reason = p->open.status};
        enqueue(&e);
    }
}
static void readback(uint8_t state) {
    provision_packet_t p = {0};
    memcpy(p.magic, "GWP1", 4);
    p.op = state;
    memcpy(p.node, s_id, 6);
    memcpy(p.target, s_pending.node, 6);
    memcpy(p.nonce, s_pending.nonce, 16);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_readback = p;
    xSemaphoreGive(s_mutex);
}
static void close_ble(void) {
    if (s_connected) {
        if (s_host)
            esp_ble_gattc_close(s_if, s_conn);
        else
            esp_ble_gatts_close(s_if, s_conn);
    }
    s_request = false;
    s_config_sent = false;
    mbedtls_platform_zeroize(&s_pending, sizeof(s_pending));
}
static void clear_wifi(void) {
    if (busy()) {
        fail("busy_call");
        return;
    }
    binding_t cleared = s_binding;
    memset(cleared.ssid, 0, sizeof(cleared.ssid));
    memset(cleared.password, 0, sizeof(cleared.password));
    // Commit the manager record first: it is authoritative on subsequent boots.
    if (s_bound && !store(&cleared)) {
        fail("clear_nvs_failed");
        return;
    }
    if (!s_hooks.clear_wifi || s_hooks.clear_wifi() != ESP_OK) {
        fail("clear_wifi_failed");
        return;
    }
    publish_binding(&cleared);
    s_candidate = false;
    close_ble();
    peer(0, false);
    s_window = now_ms() + 120000;
    s_success_until = now_ms() + 500;
    stage("waiting_provision");
    advertising(true);
    puts("GATEWAY_EVENT wifi_cleared binding_preserved=true");
}
static void receive_config(const provision_packet_t* p) {
    if (memcmp(p->magic, "GWP1", 4) || memcmp(p->target, s_id, 6) || p->reserved || p->flags > 1 ||
        !s_connected || !s_encrypted || (busy()))
        return;
    if (p->op == P_BEGIN) {
        if (s_request || now_ms() >= s_window || s_candidate) {
            fail("busy_or_window_closed");
            return;
        }
        s_pending = *p;
        esp_fill_random(s_pending.nonce, 16);
        s_request = true;
        s_deadline = now_ms() + 60000;
        readback(p->flags ? P_AUTHORIZED : P_CONFIRM);
        stage(p->flags ? "verifying_binding" : "waiting_button");
        puts("GATEWAY_EVENT provision_request");
    } else if (p->op == P_CONFIG) {
        if (!s_request || s_candidate || now_ms() >= s_deadline ||
            memcmp(p->node, s_pending.node, 6) || memcmp(p->nonce, s_pending.nonce, 16) ||
            p->flags != s_pending.flags || !memchr(p->ssid, 0, sizeof(p->ssid)) || !p->ssid[0] ||
            !memchr(p->password, 0, sizeof(p->password))) {
            fail("invalid_config");
            return;
        }
        if (p->flags) {
            if (!s_bound || memcmp(p->node, s_binding.peer, 6) ||
                !proof(s_binding.key, p, offsetof(provision_packet_t, proof), p->proof)) {
                fail("binding_auth_failed");
                close_ble();
                return;
            }
        } else if (s_readback.op != P_AUTHORIZED || zero(p->key, 32)) {
            fail("button_required");
            return;
        }
        s_pending_binding = s_binding;
        s_pending_binding.version = 1;
        memcpy(s_pending_binding.peer, p->node, 6);
        if (!p->flags)
            memcpy(s_pending_binding.key, p->key, 32);
        strlcpy(s_pending_binding.ssid, p->ssid, sizeof(s_pending_binding.ssid));
        strlcpy(s_pending_binding.password, p->password, sizeof(s_pending_binding.password));
        if (!s_hooks.configure || s_hooks.configure(p->ssid, p->password) != ESP_OK) {
            fail("wifi_apply_failed");
            readback(P_ERROR);
            return;
        }
        peer(0, false);
        s_candidate = true;
        s_candidate_online_since = 0;
        s_deadline = now_ms() + 30000;
        stage("wifi_connecting");
        readback(P_CONNECTING);
    }
}
static void remote_status(const provision_packet_t* p) {
    if (!s_request || memcmp(p->magic, "GWP1", 4) || memcmp(p->node, s_pending.target, 6) ||
        memcmp(p->target, s_id, 6))
        return;
    if (p->op == P_CONFIRM)
        stage("waiting_button");
    if (p->op == P_AUTHORIZED && !s_config_sent) {
        provision_packet_t out = s_pending;
        out.op = P_CONFIG;
        memcpy(out.nonce, p->nonce, 16);
        if (!s_reprovision) {
            binding_t candidate = {.version = 1};
            memcpy(candidate.peer, out.target, 6);
            memcpy(candidate.key, out.key, 32);
            if (!store_named("pending", &candidate)) {
                fail("pending_save_failed");
                close_ble();
                return;
            }
        }
        if (s_reprovision)
            hmac(s_binding.key, &out, offsetof(provision_packet_t, proof), out.proof);
        if (esp_ble_gattc_write_char(s_if, s_conn, s_handle, sizeof(out), (uint8_t*)&out,
                                     ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE) == ESP_OK) {
            s_config_sent = true;
            stage("wifi_connecting");
        } else
            fail("ble_write_failed");
        mbedtls_platform_zeroize(&out, sizeof(out));
    }
    if (p->op == P_DONE) {
        if (!s_reprovision) {
            binding_t binding = {.version = 1};
            memcpy(binding.peer, s_pending.target, 6);
            memcpy(binding.key, s_pending.key, 32);
            if (!store(&binding)) {
                fail("binding_save_failed");
                close_ble();
                return;
            }
            publish_binding(&binding);
            if (!store_named("pending", &binding))
                fail("pending_finalize_failed");
        }
        s_error[0] = 0;
        stage("discovering");
        close_ble();
    } else if (p->op == P_ERROR) {
        fail("remote_provision_failed");
        close_ble();
    }
}
static void begin_provision(found_t* found, bool recovery) {
    if (!wifi_online()) {
        if (recovery) {
            s_recovery_candidate = *found;
            s_recovery_candidate_valid = true;
            stage("waiting_wifi");
            ESP_LOGW(TAG, "reprovision deferred until Wi-Fi is online");
            return;
        }
        fail("s3_wifi_offline");
        return;
    }
    wifi_config_t config;
    if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK || !config.sta.ssid[0]) {
        fail("wifi_credentials_unavailable");
        return;
    }
    memset(&s_pending, 0, sizeof(s_pending));
    memcpy(s_pending.magic, "GWP1", 4);
    s_pending.op = P_BEGIN;
    s_pending.flags = recovery;
    memcpy(s_pending.node, s_id, 6);
    // ID is derived from the gateway's Wi-Fi MAC, while the BLE address may differ.
    unsigned b[6];
    if (sscanf(found->id, "GW-%2x%2x%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return;
    for (unsigned i = 0; i < 6; ++i)
        s_pending.target[i] = b[i];
    if (recovery && (!s_bound || memcmp(s_pending.target, s_binding.peer, 6))) {
        fail("not_bound_target");
        return;
    }
    memcpy(s_pending.ssid, config.sta.ssid, sizeof(config.sta.ssid));
    memcpy(s_pending.password, config.sta.password, sizeof(config.sta.password));
    mbedtls_platform_zeroize(&config, sizeof(config));
    if (!recovery)
        esp_fill_random(s_pending.key, 32);
    s_reprovision = recovery;
    s_request = true;
    s_config_sent = false;
    s_start = s_end = s_handle = 0;
    s_deadline = now_ms() + 90000;
    s_error[0] = 0;
    // The scan-complete callback may already have stopped the controller's
    // scan procedure. Calling stop again races that state and produces
    // BTM_BleScan scan-not-active warnings immediately before GATT connect.
    if (s_scanning) {
        esp_err_t scan_stop = esp_ble_gap_stop_scanning();
        ESP_LOGI(TAG, "BLE scan stop before connect result=%s", esp_err_to_name(scan_stop));
        s_scanning = false;
    }
    stage("ble_connecting");
    esp_err_t err = esp_ble_gattc_open(s_if, found->address, found->type, true);
    if (err != ESP_OK) {
        fail("ble_connect_failed");
        close_ble();
    }
}
static void process_command(event_t* e) {
    const char* cmd = e->command;
    if (!strcmp(cmd, "gateway status")) {
        status();
        return;
    }
    if (!strncmp(cmd, "found ", 6) && s_host) {
        found_t f = {0};
        unsigned type;
        if (sscanf(cmd + 6, "%15s %u %d", f.id, &type, &f.rssi) != 3)
            return;
        memcpy(f.address, e->address, 6);
        f.type = type;
        for (unsigned i = 0; i < s_found_count; ++i)
            if (!strcmp(f.id, s_found[i].id)) {
                s_found[i] = f;
                return;
            }
        if (s_found_count < GM_MAX_PEERS) {
            s_found[s_found_count++] = f;
            printf("GATEWAY_FOUND id=%s rssi=%d binding=%s\n", f.id, f.rssi,
                   s_bound && !strcmp(f.id, s_peer_id) ? "stored_unverified" : "unknown");
        }
        if (s_recovery_scan && s_bound && !strcmp(f.id, s_peer_id)) {
            s_recovery_scan = false;
            begin_provision(&f, true);
        }
        return;
    }
    if (busy()) {
        fail("busy_call");
        return;
    }
    if (!s_host && !strcmp(cmd, "gateway boot")) {
        if (s_bound && s_binding.ssid[0])
            return;
        s_window = now_ms() + 120000;
        if (!s_bound || !s_binding.ssid[0])
            stage("waiting_provision");
        advertising(true);
        return;
    }
    if (!s_host && !strcmp(cmd, "gateway button short")) {
#if CONFIG_GATEWAY_DEBUG_COMMANDS
        if (s_request && !s_candidate && s_readback.op == P_CONFIRM && now_ms() < s_deadline) {
            readback(P_AUTHORIZED);
            stage("button_confirmed");
            puts("GATEWAY_EVENT simulated_short_confirmed");
        } else
            fail("no_pending_confirmation");
#else
        fail("debug_disabled");
#endif
        return;
    }
    if (!s_host && !strcmp(cmd, "gateway button long")) {
#if CONFIG_GATEWAY_DEBUG_COMMANDS
        clear_wifi();
#else
        fail("debug_disabled");
#endif
        return;
    }
    if (s_request) {
        fail("busy_operation");
        return;
    }
    if (s_host && !strcmp(cmd, "gateway reprovision")) {
        if (!s_bound) {
            fail("not_bound");
            return;
        }
        if (s_scanning) {
            fail("busy_scan");
            return;
        }
        s_recovery_scan = true;
        s_found_count = 0;
        if (esp_ble_gap_start_scanning(8) == ESP_OK) {
            s_scanning = true;
            s_last_poll = now_ms();
            stage("recovery_scanning");
        } else {
            s_recovery_scan = false;
            fail("scan_failed");
        }
        return;
    }
    if (s_host && !strcmp(cmd, "gateway scan")) {
        if (s_scanning) {
            fail("busy_scan");
            return;
        }
        s_found_count = 0;
        esp_err_t err = esp_ble_gap_start_scanning(8);
        if (err == ESP_OK) {
            s_scanning = true;
            s_last_poll = now_ms();
            puts("GATEWAY_EVENT scan_started");
        } else
            fail("scan_failed");
    } else if (s_host &&
               (!strncmp(cmd, "gateway provision ", 18) || !strcmp(cmd, "gateway reprovision"))) {
        bool recovery = !strcmp(cmd, "gateway reprovision");
        char id[16];
        id_text(s_binding.peer, id);
        const char* target = recovery ? id : cmd + 18;
        for (unsigned i = 0; i < s_found_count; ++i)
            if (!strcmp(target, s_found[i].id)) {
                begin_provision(&s_found[i], recovery);
                return;
            }
        fail("target_not_scanned_use_gateway_scan");
    } else if (!strcmp(cmd, "gateway reconnect")) {
        peer(0, false);
        s_last_link = 0;
        stage("discovering");
    } else
        fail("unknown_command");
}

// TCP owns its own worker: finite timeouts never block BLE/button handling.
static bool transfer(int fd, void* data, size_t size, bool writing) {
    size_t offset = 0;
    int64_t until = now_ms() + 6000;
    while (offset < size && now_ms() < until) {
        int n = writing ? send(fd, (uint8_t*)data + offset, size - offset, 0)
                        : recv(fd, (uint8_t*)data + offset, size - offset, 0);
        if (n > 0)
            offset += n;
        else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            return false;
    }
    return offset == size;
}
static void packet_init(control_packet_t* p, int type, const binding_t* binding) {
    memset(p, 0, sizeof(*p));
    memcpy(p->magic, "GWC1", 4);
    p->type = type;
    p->version = 1;
    p->port = GM_MEDIA_PORT;
    p->rate = 16000;
    p->reserved[0] = 3;  // PCM16 mono, 8 kHz and 16 kHz capabilities.
    memcpy(p->node, s_id, 6);
    memcpy(p->target, binding->peer, 6);
    if (s_hooks.media) {
        uint64_t session;
        uint32_t gen, rx, tx;
        unsigned rate;
        int hfp, sco;
        s_hooks.media(&session, &gen, &rate, &hfp, &sco, &rx, &tx);
        p->session = session;
        p->generation = gen;
        p->rate = rate;
        p->hfp = hfp;
        p->sco = sco;
        p->rx = rx;
        p->tx = tx;
    }
}
static bool control_send(int fd, control_packet_t* p, const binding_t* binding) {
    if (!hmac(binding->key, p, offsetof(control_packet_t, proof), p->proof))
        return false;
    return transfer(fd, p, sizeof(*p), true);
}
static bool control_receive(int fd, control_packet_t* p, int type, bool authenticated,
                            const binding_t* binding) {
    if (!transfer(fd, p, sizeof(*p), false) || memcmp(p->magic, "GWC1", 4) || p->version != 1 ||
        p->type != type || p->port != GM_MEDIA_PORT || memcmp(p->node, binding->peer, 6) ||
        memcmp(p->target, s_id, 6))
        return false;
    return !authenticated || proof(binding->key, p, offsetof(control_packet_t, proof), p->proof);
}
static void session(int fd, uint32_t ip, const binding_t* binding) {
    struct timeval timeout = {.tv_usec = 250000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    control_packet_t out, in;
    uint8_t client[16], server[16];
    if (s_host) {
        packet_init(&out, C_HELLO, binding);
        esp_fill_random(out.client_nonce, 16);
        memcpy(client, out.client_nonce, 16);
        if (!control_send(fd, &out, binding) ||
            !control_receive(fd, &in, C_CHALLENGE, true, binding) ||
            memcmp(in.client_nonce, client, 16))
            goto done;
        memcpy(server, in.server_nonce, 16);
        packet_init(&out, C_AUTH, binding);
        memcpy(out.client_nonce, client, 16);
        memcpy(out.server_nonce, server, 16);
        if (!control_send(fd, &out, binding) || !control_receive(fd, &in, C_READY, true, binding) ||
            memcmp(in.client_nonce, client, 16) || memcmp(in.server_nonce, server, 16))
            goto done;
    } else {
        if (!control_receive(fd, &in, C_HELLO, true, binding))
            goto done;
        memcpy(client, in.client_nonce, 16);
        esp_fill_random(server, 16);
        packet_init(&out, C_CHALLENGE, binding);
        memcpy(out.client_nonce, client, 16);
        memcpy(out.server_nonce, server, 16);
        if (!control_send(fd, &out, binding) || !control_receive(fd, &in, C_AUTH, true, binding) ||
            memcmp(in.client_nonce, client, 16) || memcmp(in.server_nonce, server, 16))
            goto done;
        packet_init(&out, C_READY, binding);
        memcpy(out.client_nonce, client, 16);
        memcpy(out.server_nonce, server, 16);
        if (!control_send(fd, &out, binding))
            goto done;
    }
    peer(ip, true);
    event_t connected = {.kind = E_LINK_READY};
    enqueue(&connected);
    s_last_link = now_ms();
    ESP_LOGI(TAG, "authenticated control ready");
    for (uint32_t seq = 1; seq && wifi_online() && gateway_manager_ready(); ++seq) {
        if (s_host) {
            packet_init(&out, C_PING, binding);
            out.sequence = seq;
            memcpy(out.client_nonce, client, 16);
            memcpy(out.server_nonce, server, 16);
            if (!control_send(fd, &out, binding) ||
                !control_receive(fd, &in, C_PONG, true, binding))
                break;
        } else {
            if (!control_receive(fd, &in, C_PING, true, binding))
                break;
        }
        if (in.sequence != seq || memcmp(in.client_nonce, client, 16) ||
            memcmp(in.server_nonce, server, 16))
            break;
        s_last_link = now_ms();
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_remote_status = in;
        s_have_remote = true;
        xSemaphoreGive(s_mutex);
        if (!s_host) {
            packet_init(&out, C_PONG, binding);
            out.sequence = seq;
            memcpy(out.client_nonce, client, 16);
            memcpy(out.server_nonce, server, 16);
            if (!control_send(fd, &out, binding))
                break;
        } else if (seq == 1 || seq % 15 == 0) {
            ESP_LOGI(TAG, "remote heartbeat hfp=%u sco=%u rate=%u rx=%" PRIu32 " tx=%" PRIu32,
                     in.hfp, in.sco, in.rate, in.rx, in.tx);
        }
        vTaskDelay(pdMS_TO_TICKS(s_host ? 2000 : 10));
    }
done:
    peer(0, false);
    close(fd);
    event_t lost = {.kind = E_LINK_LOST};
    enqueue(&lost);
}
static void control_worker(void* arg) {
    int64_t mdns_retry_at = 0;
    uint32_t mdns_retry_ms = 1000;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!wifi_online() || !s_bound || s_candidate)
            continue;
        binding_t binding;
        static unsigned attempt;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        binding = (s_has_alternate && (++attempt & 1)) ? s_alternate : s_binding;
        xSemaphoreGive(s_mutex);
        if (!s_mdns) {
            if (now_ms() < mdns_retry_at)
                continue;
            esp_err_t err = mdns_init();
            char hostname[24];
            snprintf(hostname, sizeof(hostname), "%s-%02x%02x%02x", s_host ? "s3" : "gateway",
                     s_id[3], s_id[4], s_id[5]);
            if (err == ESP_OK) {
                err = mdns_hostname_set(hostname);
                if (err == ESP_OK && !s_host)
                    err = mdns_instance_name_set(s_name);
                if (err == ESP_OK && !s_host)
                    err = mdns_service_add(s_name, "_hfp-gw", "_tcp", GM_PORT, NULL, 0);
                if (err != ESP_OK)
                    mdns_free();
            }
            if (err != ESP_OK) {
                atomic_fetch_add(&s_mdns_failures, 1);
                ESP_LOGE(TAG,
                         "mDNS startup failed err=%s internal_free=%u internal_largest=%u "
                         "psram_free=%u retry_ms=%" PRIu32,
                         esp_err_to_name(err),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                                    MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                         mdns_retry_ms);
                mdns_retry_at = now_ms() + mdns_retry_ms;
                mdns_retry_ms = mdns_retry_ms < 16000 ? mdns_retry_ms * 2 : 30000;
                event_t failed = {.kind = E_DISCOVERY_FAILED};
                enqueue(&failed);
                continue;
            }
            s_mdns = true;
            mdns_retry_ms = 1000;
            ESP_LOGI(
                TAG, "mDNS started internal_free=%u internal_largest=%u psram_free=%u",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            event_t started = {.kind = E_DISCOVERY_STARTED};
            enqueue(&started);
        }
        if (s_host) {
            if (s_request)
                continue;
            mdns_result_t* results = NULL;
            if (mdns_query_ptr("_hfp-gw", "_tcp", 1000, 8, &results) != ESP_OK)
                continue;
            uint32_t ip = 0;
            char wanted[16];
            id_text(binding.peer, wanted);
            for (mdns_result_t* r = results; r; r = r->next) {
                if (!r->instance_name || strcmp(r->instance_name, wanted) || r->port != GM_PORT)
                    continue;
                for (mdns_ip_addr_t* a = r->addr; a; a = a->next)
                    if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                        ip = a->addr.u_addr.ip4.addr;
                        break;
                    }
            }
            mdns_query_results_free(results);
            if (!ip)
                continue;
            int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (fd < 0)
                continue;
            struct sockaddr_in address = {
                .sin_family = AF_INET, .sin_port = htons(GM_PORT), .sin_addr.s_addr = ip};
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            int result = connect(fd, (struct sockaddr*)&address, sizeof(address));
            if (result && errno == EINPROGRESS) {
                fd_set writable;
                FD_ZERO(&writable);
                FD_SET(fd, &writable);
                struct timeval wait = {.tv_sec = 2};
                if (select(fd + 1, NULL, &writable, NULL, &wait) > 0) {
                    int error = 0;
                    socklen_t len = sizeof(error);
                    result = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 ? error : -1;
                } else
                    result = -1;
            }
            fcntl(fd, F_SETFL, flags);
            if (!result)
                session(fd, ip, &binding);
            else
                close(fd);
        } else {
            if (s_listener < 0) {
                s_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (s_listener < 0)
                    continue;
                int one = 1;
                setsockopt(s_listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(GM_PORT)};
                if (bind(s_listener, (struct sockaddr*)&address, sizeof(address)) ||
                    listen(s_listener, 1)) {
                    close(s_listener);
                    s_listener = -1;
                    continue;
                }
                fcntl(s_listener, F_SETFL, O_NONBLOCK);
            }
            struct sockaddr_in from;
            socklen_t size = sizeof(from);
            int fd = accept(s_listener, (struct sockaddr*)&from, &size);
            if (fd >= 0)
                session(fd, from.sin_addr.s_addr, &binding);
        }
    }
}
static void led_update(int64_t now) {
#if CONFIG_IDF_TARGET_ESP32
    if (s_host || !s_led)
        return;
    const char* state = "off";
    int r = 0, g = 0, b = 0;
    bool on = true;
    if (now < s_error_until) {
        state = "error";
        r = 15;
        on = now % 1000 < 500;
    } else if (now < s_success_until) {
        state = "wifi_cleared";
        b = 15;
    } else if (s_request && s_readback.op == P_CONFIRM) {
        state = "confirm";
        b = 15;
        on = now % 500 < 250;
    } else if (now < s_window && !s_candidate && (!s_bound || !s_binding.ssid[0])) {
        state = "provision";
        b = 15;
        on = now % 2000 < 1000;
    } else if (!s_ready && (s_candidate || (s_bound && s_binding.ssid[0]))) {
        state = "connecting";
        r = g = 15;
        on = now % 2000 < 1000;
    } else if (s_ready) {
        state = "ready";
        g = 15;
        if (busy()) {
            state = "call";
            on = now % 2000 < 1000;
        }
    }
    static int previous = -1;
    int value = on ? (r << 16) | (g << 8) | b : 0;
    strlcpy(s_led_name, state, sizeof(s_led_name));
    if (value != previous &&
        led_strip_set_pixel(s_led, 0, on ? r : 0, on ? g : 0, on ? b : 0) == ESP_OK &&
        led_strip_refresh(s_led) == ESP_OK)
        previous = value;
#endif
}
static void worker(void* arg) {
    bool released = false, down = false, long_sent = false;
    int64_t down_at = 0, changed_at = 0, offline_since = now_ms();
    int previous_level = 1;
    if (!s_host && s_bound && s_binding.ssid[0] && s_hooks.configure)
        s_hooks.configure(s_binding.ssid, s_binding.password);
    while (true) {
        event_t e;
        if (xQueueReceive(s_queue, &e, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (e.kind == E_COMMAND)
                process_command(&e);
            else if (e.kind == E_AUTH && !memcmp(e.address, s_remote, 6)) {
                s_encrypted = e.packet.flags;
                if (!s_encrypted) {
                    fail("ble_secure_connection_failed");
                    close_ble();
                } else
                    ESP_LOGI(TAG, "BLE secure_connections encrypted=true");
            } else if (e.kind == E_WRITE && !s_host && e.conn == s_conn)
                receive_config(&e.packet);
            else if (e.kind == E_REMOTE && s_host)
                remote_status(&e.packet);
            else if (e.kind == E_DISCOVERY_FAILED) {
                fail("mdns_init_failed");
                if (!s_request && !s_ready)
                    stage("discovery_failed");
            } else if (e.kind == E_DISCOVERY_STARTED && !strcmp(s_error, "mdns_init_failed")) {
                s_error[0] = 0;
                if (!s_request && !s_ready)
                    stage("discovering");
            } else if (e.kind == E_LINK_READY) {
                s_error[0] = 0;
                stage("ready");
            } else if (e.kind == E_LINK_LOST && s_bound && !s_request && (s_host || wifi_online()))
                stage("discovering");
            else if (e.kind == E_DISCONNECT) {
                ESP_LOGI(TAG,
                         "BLE disconnected conn=%u reason=0x%02x stage=%s pending=%d "
                         "config_sent=%d candidate=%d",
                         e.conn, e.reason, s_stage, (int)s_request, s_config_sent,
                         (int)s_candidate);
                if (s_request && !s_candidate) {
                    if (!s_host && s_readback.op == P_DONE)
                        close_ble();
                    else {
                        fail("ble_disconnected");
                        close_ble();
                        stage("provision_failed");
                    }
                }
                if (!s_host && now_ms() < s_window)
                    advertising(true);
            }
            mbedtls_platform_zeroize(&e, sizeof(e));
        }
        int64_t now = now_ms();
        if (s_recovery_candidate_valid && !s_scanning && wifi_online() && !s_request) {
            found_t candidate = s_recovery_candidate;
            s_recovery_candidate_valid = false;
            ESP_LOGI(TAG, "retrying deferred reprovision after Wi-Fi became online");
            begin_provision(&candidate, true);
        }
        if (s_scanning && now - s_last_poll >= 8500) {
            s_scanning = false;
            if (s_recovery_scan) {
                s_recovery_scan = false;
                fail("recovery_gateway_not_found");
            }
            printf("GATEWAY_EVENT scan_done count=%u\n", s_found_count);
        }
        if (s_host && s_request && s_connected && s_encrypted && s_handle && s_mtu >= 201 &&
            now - s_last_poll > 500) {
            if (!s_config_sent && !strcmp(s_stage, "ble_connecting")) {
                provision_packet_t begin = {0};
                memcpy(begin.magic, "GWP1", 4);
                begin.op = P_BEGIN;
                begin.flags = s_pending.flags;
                memcpy(begin.node, s_pending.node, 6);
                memcpy(begin.target, s_pending.target, 6);
                if (esp_ble_gattc_write_char(s_if, s_conn, s_handle, sizeof(begin),
                                             (uint8_t*)&begin, ESP_GATT_WRITE_TYPE_RSP,
                                             ESP_GATT_AUTH_REQ_NONE) == ESP_OK)
                    stage("waiting_remote");
            } else
                esp_ble_gattc_read_char(s_if, s_conn, s_handle, ESP_GATT_AUTH_REQ_NONE);
            s_last_poll = now;
        }
        if (s_candidate && !s_host) {
            if (wifi_online()) {
                if (!s_candidate_online_since)
                    s_candidate_online_since = now;
                // Require a stable, fresh association before committing. This
                // prevents an old DHCP/IP state from accepting a candidate
                // before the new SSID has actually stayed online.
                if (now - s_candidate_online_since >= 2000) {
                    if (store(&s_pending_binding)) {
                        publish_binding(&s_pending_binding);
                        s_candidate = false;
                        s_error[0] = 0;
                        readback(P_DONE);
                        stage("discovering");
                        s_window = 0;
                        advertising(false);
                        s_deadline = now + 10000;
                        puts("GATEWAY_EVENT provision_committed");
                    } else {
                        fail("binding_save_failed");
                        readback(P_ERROR);
                        s_deadline = now;
                    }
                }
            } else if (now >= s_deadline) {
                s_candidate_online_since = 0;
                fail("wifi_connection_timeout");
                s_candidate = false;
                readback(P_ERROR);
                if (s_bound && s_binding.ssid[0] && s_hooks.configure)
                    s_hooks.configure(s_binding.ssid, s_binding.password);
                else if (s_hooks.clear_wifi)
                    s_hooks.clear_wifi();
                s_deadline = now + 5000;
            }
        }
        if (s_request && !s_candidate && now >= s_deadline) {
            ESP_LOGW(TAG, "provision deadline expired stage=%s config_sent=%d", s_stage,
                     s_config_sent);
            fail("provision_timeout");
            close_ble();
            stage("provision_failed");
        }
        if (!s_host) {
            bool in_call = busy();
            if (in_call) {
                advertising(false);
                if (s_request && !s_candidate)
                    close_ble();
            } else {
                if (wifi_online())
                    offline_since = now;
                else if (s_bound && now - offline_since > 60000 && now >= s_window) {
                    s_window = now + 120000;
                    offline_since = now;
                    advertising(true);
                }
                if (now >= s_window && s_adv && !s_request)
                    advertising(false);
            }
#if CONFIG_IDF_TARGET_ESP32
            int level = gpio_get_level(GPIO_NUM_0);
            if (level != previous_level) {
                previous_level = level;
                changed_at = now;
            }
            if (now - changed_at >= 40) {
                if (level) {
                    if (down && !long_sent && released && s_request && s_readback.op == P_CONFIRM &&
                        now < s_deadline) {
                        readback(P_AUTHORIZED);
                        stage("button_confirmed");
                        puts("GATEWAY_EVENT physical_short_confirmed");
                    }
                    down = false;
                    released = true;
                } else if (released) {
                    if (!down) {
                        down = true;
                        down_at = now;
                        long_sent = false;
                    }
                    if (!long_sent && now - down_at >= 3000) {
                        long_sent = true;
                        clear_wifi();
                    }
                }
            }
#endif
            led_update(now);
        }
    }
}
esp_err_t gateway_manager_start(bool host, const gateway_manager_hooks_t* hooks) {
    if (psa_crypto_init() != PSA_SUCCESS)
        return ESP_FAIL;
    if (!crypto_selftest())
        return ESP_FAIL;
    ESP_LOGI(TAG, "HMAC selftest=passed known_vector corrupted_proofs=32");
    s_host = host;
    if (hooks)
        s_hooks = *hooks;
    ESP_ERROR_CHECK(esp_read_mac(s_id, ESP_MAC_WIFI_STA));
    id_text(s_id, s_name);
    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(8, sizeof(event_t));
    if (!s_mutex || !s_queue)
        return ESP_ERR_NO_MEM;
    load();
    id_text(s_binding.peer, s_peer_id);
    if (host) {
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_bt_controller_init(&cfg));
        ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
        ESP_ERROR_CHECK(esp_bluedroid_init());
        ESP_ERROR_CHECK(esp_bluedroid_enable());
    }
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap));
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(247));
    uint8_t auth = ESP_LE_AUTH_REQ_SC_ONLY, io = ESP_IO_CAP_NONE, size = 16;
    uint8_t specified = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
    ESP_ERROR_CHECK(
        esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, sizeof(io)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &size, sizeof(size)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH,
                                                   &specified, sizeof(specified)));
    if (host) {
        ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc));
        ESP_ERROR_CHECK(esp_ble_gattc_app_register(0x47));
    } else {
#if CONFIG_IDF_TARGET_ESP32
        gpio_config_t button = {
            .pin_bit_mask = 1ULL << GPIO_NUM_0, .mode = GPIO_MODE_INPUT, .pull_up_en = true};
        ESP_ERROR_CHECK(gpio_config(&button));
        led_strip_config_t led = {.strip_gpio_num = 27,
                                  .max_leds = 1,
                                  .led_model = LED_MODEL_WS2812,
                                  .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB};
        led_strip_rmt_config_t rmt = {.resolution_hz = 10000000, .mem_block_symbols = 64};
        esp_err_t err = led_strip_new_rmt_device(&led, &rmt, &s_led);
        if (err != ESP_OK) {
            s_led = NULL;
            fail("rgb_init_failed");
        }
#endif
        ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts));
        ESP_ERROR_CHECK(esp_ble_gatts_app_register(0x47));
    }
    if (!host)
        s_window = now_ms() + 120000;
    stage(s_bound ? "discovering" : "unconfigured");
    // Keep the commissioning tasks small enough for the full XiaoZhi image;
    // their queues and network buffers are bounded and neither task needs a
    // large stack for packet payloads.
    const uint32_t worker_stack = host ? 3072 : 4096;
    // Provisioning callbacks perform BLE teardown, NVS commit and Wi-Fi
    // startup in one control path; the former 2048-byte stack overflowed.
    const uint32_t control_stack = host ? 4096 : 4096;
    if (xTaskCreate(worker, "gateway_manager", worker_stack, NULL, 3, NULL) != pdPASS ||
        xTaskCreate(control_worker, "gateway_control", control_stack, NULL, 3, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "started role=%s id=%s bound=%d debug_buttons=%d", host ? "s3" : "gateway",
             s_name, s_bound, CONFIG_GATEWAY_DEBUG_COMMANDS);
    return ESP_OK;
}

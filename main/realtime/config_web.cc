#include "config_web.h"

#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
#include <esp_wifi.h>
#endif
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "application.h"
#include "config.h"
#include "debug_trace.h"

namespace {
constexpr char kTag[] = "AliyunConfigWeb";
httpd_handle_t server = nullptr;
extern const char config_html_start[] asm("_binary_config_html_start");
extern const char config_html_end[] asm("_binary_config_html_end");

esp_err_t Reply(httpd_req_t* request, const char* status, const char* body) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, body);
}

#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
cJSON* ResourceSnapshot() {
    cJSON* root = cJSON_CreateObject();
    if (!root)
        return nullptr;
    cJSON_AddNumberToObject(root, "time_us", esp_timer_get_time());
    auto& app = Application::GetInstance();
    cJSON_AddStringToObject(root, "state", DeviceStateMachine::GetStateName(app.GetDeviceState()));
    const auto mode = app.GetConversationMode();
    cJSON_AddStringToObject(root, "mode",
                            mode == kListeningModeManualStop ? "manual"
                            : mode == kListeningModeRealtime ? "realtime"
                                                             : "auto");
    cJSON_AddNumberToObject(root, "cores", CONFIG_FREERTOS_NUMBER_OF_CORES);
    cJSON_AddNumberToObject(root, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "internal_min_since_boot",
                            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "internal_largest",
                            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "psram_min_since_boot",
                            heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    const auto audio = Application::GetInstance().GetAudioService().GetEngineDiagnostics();
    cJSON_AddNumberToObject(root, "input_gain_db",
                            Board::GetInstance().GetAudioCodec()->input_gain());
    cJSON_AddNumberToObject(root, "output_volume",
                            Board::GetInstance().GetAudioCodec()->output_volume());
    cJSON_AddBoolToObject(root, "codec_reference",
                          Board::GetInstance().GetAudioCodec()->input_reference());
    cJSON_AddNumberToObject(root, "input_channels",
                            Board::GetInstance().GetAudioCodec()->input_channels());
    cJSON* engine = cJSON_AddObjectToObject(root, "engine");
    cJSON_AddBoolToObject(engine, "aec_enabled", audio.aec_enabled);
    cJSON_AddNumberToObject(engine, "feed_frames", audio.feed_frames);
    cJSON_AddNumberToObject(engine, "feed_samples", audio.feed_samples);
    cJSON_AddNumberToObject(engine, "feed_wall_us", audio.feed_wall_us);
    cJSON_AddNumberToObject(engine, "feed_max_us_since_boot", audio.feed_max_us);
    cJSON_AddNumberToObject(engine, "feed_overruns", audio.feed_overruns);
    cJSON_AddNumberToObject(engine, "fetch_frames", audio.fetch_frames);
    cJSON_AddNumberToObject(engine, "fetch_samples", audio.fetch_samples);
    cJSON_AddNumberToObject(engine, "fetch_errors", audio.fetch_errors);
#if configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS
    // This runs on the HTTP task, never on the main event loop or an audio task.
    std::vector<TaskStatus_t> tasks(uxTaskGetNumberOfTasks() + 8);
    configRUN_TIME_COUNTER_TYPE total = 0;
    const UBaseType_t count = uxTaskGetSystemState(tasks.data(), tasks.size(), &total);
    cJSON_AddBoolToObject(root, "runtime_valid", count > 0);
    cJSON_AddNumberToObject(root, "runtime_total", total);
    cJSON_AddNumberToObject(root, "runtime_bits", sizeof(total) * 8);
    cJSON* list = cJSON_AddArrayToObject(root, "tasks");
    for (UBaseType_t i = 0; i < count; ++i) {
        cJSON* task = cJSON_CreateObject();
        if (!task)
            break;
        cJSON_AddNumberToObject(task, "id", tasks[i].xTaskNumber);
        cJSON_AddStringToObject(task, "name", tasks[i].pcTaskName);
        cJSON_AddNumberToObject(task, "runtime", tasks[i].ulRunTimeCounter);
        // ESP-IDF reports the task stack high-water mark in bytes.
        cJSON_AddNumberToObject(task, "stack_min_free_bytes", tasks[i].usStackHighWaterMark);
        cJSON_AddItemToArray(list, task);
    }
#else
    cJSON_AddBoolToObject(root, "runtime_valid", false);
#endif
    return root;
}
#endif

esp_err_t GetTestStats(httpd_req_t* request) {
#if CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
    cJSON* root = ResourceSnapshot();
    char* json = root ? cJSON_PrintUnformatted(root) : nullptr;
    cJSON_Delete(root);
    if (!json)
        return Reply(request, "500 Internal Server Error", "{}");
    const auto result = Reply(request, "200 OK", json);
    cJSON_free(json);
    return result;
#else
    return Reply(request, "404 Not Found", "{\"error\":\"software_aec_disabled\"}");
#endif
}

esp_err_t GetPage(httpd_req_t* request) {
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, config_html_start, config_html_end - config_html_start);
}

esp_err_t PostTestChat(httpd_req_t* request) {
    if (!LoadAliyunRealtimeConfig().debug_enabled)
        return Reply(request, "403 Forbidden", "{\"error\":\"debug_disabled\"}");
    char query[64] = {};
    char action[16] = {};
    if (request->content_len != 0 ||
        httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "action", action, sizeof(action)) != ESP_OK)
        return Reply(request, "400 Bad Request", "{\"error\":\"invalid_action\"}");
    auto& app = Application::GetInstance();
    if (strcmp(action, "cycle") == 0)
        app.CycleListeningMode();
    else if (strcmp(action, "click") == 0)
        app.ToggleChatState();
    else if (strcmp(action, "stop") == 0)
        app.EndConversation();
    else if (strcmp(action, "trace-aec") == 0 || strcmp(action, "trace-basic") == 0) {
        if (app.GetDeviceState() != kDeviceStateIdle)
            return Reply(request, "409 Conflict", "{\"error\":\"audio_busy\"}");
        DebugTrace::Instance().SetConversationAecCapture(strcmp(action, "trace-aec") == 0);
    } else if (strcmp(action, "aec-on") == 0 || strcmp(action, "aec-off") == 0) {
        if (app.GetDeviceState() != kDeviceStateIdle)
            return Reply(request, "409 Conflict", "{\"error\":\"audio_busy\"}");
        app.SetAecMode(strcmp(action, "aec-on") == 0 ? kAecOnDeviceSide : kAecOnServerSide);
    } else if (strncmp(action, "volume-", 7) == 0) {
        const char* value = action + 7;
        if (value[0] == '\0')
            return Reply(request, "400 Bad Request", "{\"error\":\"invalid_volume\"}");
        for (const char* digit = value; *digit != '\0'; ++digit) {
            if (*digit < '0' || *digit > '9')
                return Reply(request, "400 Bad Request", "{\"error\":\"invalid_volume\"}");
        }
        const int volume = std::atoi(value);
        if (volume < 1 || volume > 100)
            return Reply(request, "400 Bad Request", "{\"error\":\"invalid_volume\"}");
        if (app.GetDeviceState() != kDeviceStateIdle)
            return Reply(request, "409 Conflict", "{\"error\":\"audio_busy\"}");
        app.Schedule([volume]() { Board::GetInstance().GetAudioCodec()->SetOutputVolume(volume); });
    } else
        return Reply(request, "400 Bad Request", "{\"error\":\"invalid_action\"}");
    return Reply(request, "202 Accepted", "{\"queued\":true}");
}

esp_err_t GetConfig(httpd_req_t* request) {
    auto config = LoadAliyunRealtimeConfig();
    cJSON* root = cJSON_CreateObject();
    if (!root)
        return Reply(request, "500 Internal Server Error", "{}");
    cJSON_AddBoolToObject(root, "selected", config.selected);
    cJSON_AddStringToObject(root, "region", config.region.c_str());
    cJSON_AddStringToObject(root, "workspace_id", config.workspace_id.c_str());
    cJSON_AddStringToObject(root, "websocket_url", config.websocket_url.c_str());
    cJSON_AddBoolToObject(root, "api_key_set", !config.api_key.empty());
    cJSON_AddBoolToObject(root, "debug_enabled", config.debug_enabled);
    cJSON_AddStringToObject(root, "debug_address", config.debug_address.c_str());
    cJSON_AddNumberToObject(root, "debug_level", config.debug_level);
    cJSON_AddBoolToObject(root, "debug_input_pcm", config.debug_input_pcm);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return Reply(request, "500 Internal Server Error", "{}");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    auto result = httpd_resp_sendstr(request, json);
    cJSON_free(json);
    return result;
}

bool ReadBool(const cJSON* root, const char* name, bool& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsBool(item))
        return false;
    out = cJSON_IsTrue(item);
    return true;
}

bool ReadString(const cJSON* root, const char* name, std::string& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item))
        return false;
    out = item->valuestring;
    return true;
}

esp_err_t PutConfig(httpd_req_t* request) {
    if (request->content_len <= 0 || request->content_len > 1024) {
        return Reply(request, "413 Content Too Large", "{\"error\":\"invalid_size\"}");
    }
    std::string body(request->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        int count = httpd_req_recv(request, body.data() + received, body.size() - received);
        if (count <= 0)
            return Reply(request, "400 Bad Request", "{\"error\":\"incomplete_body\"}");
        received += count;
    }
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return Reply(request, "400 Bad Request", "{\"error\":\"invalid_json\"}");
    }
    auto config = LoadAliyunRealtimeConfig();
    bool clear_key = false;
    const cJSON* level = cJSON_GetObjectItemCaseSensitive(root, "debug_level");
    const cJSON* input_pcm = cJSON_GetObjectItemCaseSensitive(root, "debug_input_pcm");
    const cJSON* key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    const bool input_pcm_valid = input_pcm == nullptr || cJSON_IsBool(input_pcm);
    bool valid = ReadBool(root, "selected", config.selected) &&
                 ReadString(root, "region", config.region) &&
                 ReadString(root, "workspace_id", config.workspace_id) &&
                 ReadString(root, "websocket_url", config.websocket_url) &&
                 ReadBool(root, "debug_enabled", config.debug_enabled) &&
                 ReadString(root, "debug_address", config.debug_address) &&
                 ReadBool(root, "clear_api_key", clear_key) && cJSON_IsNumber(level) &&
                 level->valuedouble == level->valueint && input_pcm_valid && cJSON_IsString(key);
    if (valid) {
        config.debug_level = level->valueint;
        if (input_pcm != nullptr) {
            config.debug_input_pcm = cJSON_IsTrue(input_pcm);
        }
        if (clear_key)
            config.api_key.clear();
        else if (key->valuestring[0] != '\0')
            config.api_key = key->valuestring;
        valid = ValidateAliyunRealtimeConfig(config);
    }
    cJSON_Delete(root);
    if (!valid)
        return Reply(request, "400 Bad Request", "{\"error\":\"invalid_config\"}");
    if (!SaveAliyunRealtimeConfig(config)) {
        return Reply(request, "500 Internal Server Error", "{\"error\":\"save_failed\"}");
    }
    return Reply(request, "200 OK", "{\"ok\":true,\"restart_required\":true}");
}

esp_err_t PostTestPcm(httpd_req_t* request) {
#if !CONFIG_WAVESHARE_3_5B_SOFTWARE_AEC
    return Reply(request, "501 Not Implemented", "{\"error\":\"software_aec_disabled\"}");
#else
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    if (app.GetDeviceState() != kDeviceStateIdle || !audio.IsPlaybackIdle() ||
        audio.IsAudioProcessorRunning()) {
        return Reply(request, "409 Conflict", "{\"error\":\"audio_busy\"}");
    }
    int delay_ms = CONFIG_WAVESHARE_3_5B_AEC_DELAY_MS;
    int aec_enabled = 1;
    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length > 80) {
        return Reply(request, "400 Bad Request", "{\"error\":\"invalid_query\"}");
    }
    if (query_length > 0) {
        char query[81] = {};
        if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
            return Reply(request, "400 Bad Request", "{\"error\":\"invalid_query\"}");
        }
        auto read_integer = [&](const char* name, int maximum, int& result) {
            char value[16] = {};
            const auto error = httpd_query_key_value(query, name, value, sizeof(value));
            if (error == ESP_ERR_NOT_FOUND)
                return true;
            if (error != ESP_OK || value[0] == '\0')
                return false;
            char* end = nullptr;
            const long number = std::strtol(value, &end, 10);
            if (*end != '\0' || number < 0 || number > maximum)
                return false;
            result = static_cast<int>(number);
            return true;
        };
        if (!read_integer("delay_ms", 250, delay_ms) || !read_integer("aec", 1, aec_enabled)) {
            return Reply(request, "400 Bad Request", "{\"error\":\"invalid_test_options\"}");
        }
    }
    constexpr size_t kMaxBytes = 24000 * 2 * 20;
    if (request->content_len <= 0 || request->content_len > kMaxBytes ||
        (request->content_len & 1u) != 0) {
        return Reply(request, "413 Content Too Large", "{\"error\":\"invalid_pcm_size\"}");
    }
    std::vector<uint8_t> pcm(request->content_len);
    size_t received = 0;
    while (received < pcm.size()) {
        int count = httpd_req_recv(request, reinterpret_cast<char*>(pcm.data() + received),
                                   pcm.size() - received);
        if (count <= 0)
            return Reply(request, "400 Bad Request", "{\"error\":\"incomplete_body\"}");
        received += static_cast<size_t>(count);
    }
    DebugTrace::Instance().Configure(LoadAliyunRealtimeConfig());
    // Manual AEC captures must keep the AFE running without WakeNet. A
    // deterministic sweep can otherwise trigger the wake-word path and stop
    // the capture before the playback and reference tracks are complete.
    const bool restore_wake_word = audio.IsWakeWordRunning();
    const bool restore_aec = audio.IsDeviceAecEnabled();
    wifi_ps_type_t power_save = WIFI_PS_NONE;
    const bool restore_power_save = esp_wifi_get_ps(&power_save) == ESP_OK;
    // Idle normally uses Wi-Fi modem sleep; continuous diagnostic PCM needs
    // the same throughput as an active voice session.
    if (restore_power_save)
        esp_wifi_set_ps(WIFI_PS_NONE);
    audio.EnableWakeWordDetection(false);
    // Let the last 10 ms read finish before clearing the test time line.
    vTaskDelay(pdMS_TO_TICKS(50));
    DebugTrace::Instance().Open("manual", true);
    audio.EnableDeviceAec(aec_enabled != 0);
    audio.EnableAecMonitoring(delay_ms);
    char options[80];
    std::snprintf(options, sizeof(options), "delay_ms=%d aec=%d preroll_ms=500", delay_ms,
                  aec_enabled);
    DebugTrace::Instance().Event("aec_test_options", options);
    // Drain ADC DMA history and allow the input resampler to settle before playback.
    vTaskDelay(pdMS_TO_TICKS(500));
    cJSON* stats_before = ResourceSnapshot();

    auto finish = [&](const char* status, const char* body) {
        cJSON* result = cJSON_Parse(body);
        if (result) {
            cJSON_AddItemToObject(result, "stats_before", stats_before);
            cJSON_AddItemToObject(result, "stats_after", ResourceSnapshot());
        } else {
            cJSON_Delete(stats_before);
        }
        stats_before = nullptr;
        char* response = result ? cJSON_PrintUnformatted(result) : nullptr;
        cJSON_Delete(result);
        // Keep the AFE alive until playback is drained and the acoustic tail
        // has passed through the configured reference delay.
        audio.EnableVoiceProcessing(false);
        vTaskDelay(pdMS_TO_TICKS(50));
        DebugTrace::Instance().Close();
        audio.EnableDeviceAec(restore_aec);
        audio.EnableWakeWordDetection(restore_wake_word);
        if (restore_power_save)
            esp_wifi_set_ps(power_save);
        const auto error = Reply(request, status, response ? response : body);
        cJSON_free(response);
        return error;
    };

    constexpr size_t kChunkBytes = 4800;
    for (size_t offset = 0; offset < pcm.size(); offset += kChunkBytes) {
        const size_t end = std::min(offset + kChunkBytes, pcm.size());
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->pcm = true;
        packet->sample_rate = 24000;
        packet->frame_duration = 100;
        packet->payload.assign(pcm.begin() + offset, pcm.begin() + end);
        if (!Application::GetInstance().GetAudioService().PushPacketToDecodeQueue(std::move(packet),
                                                                                  true)) {
            return finish("503 Service Unavailable", "{\"error\":\"playback_queue_full\"}");
        }
    }

    const int64_t deadline = esp_timer_get_time() + 30000000;
    while (!Application::GetInstance().GetAudioService().IsPlaybackIdle() &&
           esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!Application::GetInstance().GetAudioService().IsPlaybackIdle()) {
        return finish("504 Gateway Timeout", "{\"error\":\"playback_timeout\"}");
    }
    vTaskDelay(pdMS_TO_TICKS(delay_ms + 500));
    return finish("200 OK", "{\"ok\":true}");
#endif
}

}  // namespace

void StartAliyunConfigWeb() {
    if (server)
        return;
    httpd_config_t settings = HTTPD_DEFAULT_CONFIG();
    settings.server_port = 80;
    settings.max_uri_handlers = 6;
    if (httpd_start(&server, &settings) != ESP_OK) {
        ESP_LOGE(kTag, "Failed to start configuration server");
        server = nullptr;
        return;
    }
    httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = GetPage};
    httpd_uri_t get = {.uri = "/api/config", .method = HTTP_GET, .handler = GetConfig};
    httpd_uri_t put = {.uri = "/api/config", .method = HTTP_PUT, .handler = PutConfig};
    httpd_uri_t test_pcm = {.uri = "/api/test/pcm", .method = HTTP_POST, .handler = PostTestPcm};
    httpd_uri_t test_stats = {
        .uri = "/api/test/stats", .method = HTTP_GET, .handler = GetTestStats};
    httpd_uri_t test_chat = {.uri = "/api/test/chat", .method = HTTP_POST, .handler = PostTestChat};
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &get);
    httpd_register_uri_handler(server, &put);
    httpd_register_uri_handler(server, &test_pcm);
    httpd_register_uri_handler(server, &test_stats);
    httpd_register_uri_handler(server, &test_chat);
    ESP_LOGI(kTag, "Configuration page listening on port 80");
}

void StopAliyunConfigWeb() {
    if (!server)
        return;
    httpd_stop(server);
    server = nullptr;
}

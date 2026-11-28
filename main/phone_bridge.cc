#include "phone_bridge.h"
#include "call_records.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <lwip/sockets.h>
#include <sys/socket.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "board.h"
#include "driver/uart.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "ima_adpcm.h"
#include "phone_downlink.h"
#if CONFIG_GATEWAY_MANAGER
#include "gateway_manager.h"
#include "gateway_serial.h"
#endif

static const char* TAG = "phone_bridge";
static constexpr uint16_t kPort = 39001;
static constexpr size_t kHeader = 32;
static constexpr size_t kFrame8 = 160;
static constexpr size_t kFrame16 = 320;
static constexpr uart_port_t kPhoneUart = UART_NUM_1;
// Waveshare ESP32-S3 Touch LCD 3.5B exposes GPIO43 (ESP_TXD) and GPIO0 on J8.
// GPIO44 is ES8311 MCLK and cannot be used for the media UART. GPIO43 is free
// after the console is routed to USB Serial/JTAG in the board build.
static constexpr int kPhoneUartTx = 43;
static constexpr int kPhoneUartRx = 0;
static constexpr int kPhoneUartBaud = 921600;
static constexpr size_t kUartFrameHeader = 8;
static constexpr size_t kUartFrameMax = kUartFrameHeader + 32 + kFrame16 * 2;

static uint16_t r16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
static uint32_t r32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
static void w16(uint8_t* p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v;
}
static void w32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static uint16_t uart_r16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
static uint32_t uart_r32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

static size_t uart_make_hfap(uint8_t* out, size_t capacity, uint8_t kind, uint8_t flags,
                             uint8_t codec, uint64_t session, uint32_t generation,
                             uint32_t sequence, uint16_t rate, const uint8_t* payload,
                             uint16_t payload_bytes) {
    if (!out || capacity < 32 + payload_bytes || !session || !generation ||
        (rate != 8000 && rate != 16000))
        return 0;
    std::memcpy(out, "HFAP", 4);
    out[4] = 1;
    out[5] = kind;
    out[6] = flags;
    out[7] = codec;
    w32(out + 8, uint32_t(session >> 32));
    w32(out + 12, uint32_t(session));
    w32(out + 16, generation);
    w32(out + 20, sequence);
    w32(out + 24, kind == 3 ? sequence * uint32_t(rate / 50) : 0);
    w16(out + 28, rate);
    w16(out + 30, payload_bytes);
    if (payload_bytes)
        std::memcpy(out + 32, payload, payload_bytes);
    return 32 + payload_bytes;
}

static bool uart_send_frame(const uint8_t* hfap, size_t hfap_size, SemaphoreHandle_t lock) {
    if (!hfap || !hfap_size || hfap_size > 32 + kFrame16 * 2 || !lock)
        return false;
    uint8_t frame[kUartFrameMax];
    frame[0] = 0x55;
    frame[1] = 0xaa;
    frame[2] = 1;
    frame[3] = 0;
    w16(frame + 4, uint16_t(hfap_size));
    const uint16_t crc = esp_crc16_be(0, hfap, hfap_size);
    w16(frame + 6, crc);
    std::memcpy(frame + kUartFrameHeader, hfap, hfap_size);
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;
    const int written = uart_write_bytes(kPhoneUart, frame, kUartFrameHeader + hfap_size);
    xSemaphoreGive(lock);
    return written == int(kUartFrameHeader + hfap_size);
}

static void upsample_to_24k(const int16_t* in, size_t samples, unsigned rate, int16_t* out) {
    const size_t out_samples = samples * 24000 / rate;
    for (size_t i = 0; i < out_samples; ++i) {
        const uint64_t pos = uint64_t(i) * rate;
        const size_t index = size_t(pos / 24000);
        const int32_t frac = int32_t(pos % 24000);
        const int16_t a = in[index < samples ? index : samples - 1];
        const int16_t b = in[index + 1 < samples ? index + 1 : samples - 1];
        out[i] = int16_t(int32_t(a) + (int32_t(b) - a) * frac / 24000);
    }
}

bool PhoneBridge::SetAiRoute(bool enabled) {
    std::lock_guard<std::mutex> lock(ai_mutex_);
    constexpr size_t capacity = 24000 * 30;
    if (enabled && !ai_storage_) {
        const uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        ai_storage_ = static_cast<int16_t*>(heap_caps_malloc(capacity * 2, external));
        if (!ai_storage_) {
            ESP_LOGE(TAG,
                     "AI buffer allocation bytes=%u caps=0x%lx internal_free=%u min=%u largest=%u "
                     "psram_free=%u largest=%u",
                     unsigned(capacity * 2), (unsigned long)external,
                     unsigned(heap_caps_get_free_size(internal)),
                     unsigned(heap_caps_get_minimum_free_size(internal)),
                     unsigned(heap_caps_get_largest_free_block(internal)),
                     unsigned(heap_caps_get_free_size(external)),
                     unsigned(heap_caps_get_largest_free_block(external)));
            return false;
        }
        ai_downlink_.Attach(ai_storage_, capacity);
    }
    ai_route_.store(enabled);
    ++ai_epoch_;
    ai_downlink_.Clear();
    phone_uplink_.clear();
    ai_played_us_ = 0;
    ai_failure_.store(PhoneAiFailure::None);
    if (enabled)
        ai_rx_samples_ = ai_tx_samples_ = ai_uplink_drops_ = 0;
    ESP_LOGI(TAG,
             "PHONE_AI audio_route=%d epoch=%lu cloud_rx_samples=%lu hfp_tx_samples=%lu "
             "uplink_drops=%lu",
             enabled, (unsigned long)ai_epoch_.load(), (unsigned long)ai_rx_samples_,
             (unsigned long)ai_tx_samples_, (unsigned long)ai_uplink_drops_);
    return true;
}

void PhoneBridge::InvalidateAiMedia(const char* reason, PhoneAiFailure failure) {
    std::lock_guard<std::mutex> lock(ai_mutex_);
    if (!ai_route_.load())
        return;
    if (AiAudioFailed())
        return;
    ai_failure_.store(failure);
    ai_downlink_.Clear();
    phone_uplink_.clear();
    ESP_LOGW(TAG, "PHONE_AI failure=media_changed reason=%s", reason);
}

void PhoneBridge::QueueAiDownlink(std::unique_ptr<AudioStreamPacket> packet) {
    if (!packet || !packet->pcm || packet->sample_rate != 24000 || packet->payload.empty() ||
        packet->payload.size() % 2)
        return;
    std::lock_guard<std::mutex> lock(ai_mutex_);
    if (!ai_route_.load() || AiAudioFailed())
        return;
    const size_t samples = packet->payload.size() / 2;
    if (!ai_downlink_.Push(reinterpret_cast<const int16_t*>(packet->payload.data()), samples)) {
        ai_failure_.store(PhoneAiFailure::AudioQueueOverflow);
        ESP_LOGE(TAG, "PHONE_AI failure=downlink_overflow queued_samples=%u incoming=%u",
                 unsigned(ai_downlink_.size()), unsigned(samples));
        return;
    }
    ai_rx_samples_ += samples;
    if (ai_rx_samples_ == samples || ai_rx_samples_ % 24000 < samples)
        ESP_LOGI(TAG, "PHONE_AI cloud_rx_samples=%lu queued_ms=%u", (unsigned long)ai_rx_samples_,
                 unsigned(ai_downlink_.size() / 24));
}

std::unique_ptr<AudioStreamPacket> PhoneBridge::PopPhoneUplink() {
    std::lock_guard<std::mutex> lock(ai_mutex_);
    if (phone_uplink_.empty() || !ai_route_.load())
        return nullptr;
    auto packet = std::move(phone_uplink_.front());
    phone_uplink_.pop_front();
    return packet;
}

bool PhoneBridge::AiPlaybackIdle() {
    std::lock_guard<std::mutex> lock(ai_mutex_);
    return !ai_downlink_.size() &&
           (!ai_played_us_ || esp_timer_get_time() - ai_played_us_ > 250000);
}

static bool resampler_selftest() {
    int16_t input[320];
    int16_t output[480];
    for (unsigned rate : {8000U, 16000U}) {
        const size_t samples = rate / 50;
        for (size_t i = 0; i < samples; ++i)
            input[i] = (i & 1) ? -32768 : 32767;
        upsample_to_24k(input, samples, rate, output);
        for (size_t i = 0; i < 480; ++i) {
            const size_t index = i * rate / 24000;
            const int64_t frac = i * rate % 24000;
            const int64_t a = input[index];
            const int64_t b = input[index + 1 < samples ? index + 1 : index];
            const int64_t expected = a + (b - a) * frac / 24000;
            if (output[i] != expected) {
                ESP_LOGE(TAG,
                         "resampler selftest failed rate=%u sample=%u actual=%d expected=%" PRId64,
                         rate, unsigned(i), output[i], expected);
                return false;
            }
        }
    }
    return true;
}

void PhoneBridge::Start() {
    if (!task_handle_) {
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_SPIRAM
        // The bridge keeps sizeable parser/codec workspaces on its task stack.
        // Keep that stack in PSRAM so Wi-Fi/Bluetooth retain their capability-
        // constrained internal heap during startup and SCO media.
        const BaseType_t result =
            xTaskCreateWithCaps(Task, "phone_bridge", 12288, this, 5, &task_handle_,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "Start task result=%d handle=%p stack_caps=psram", int(result), task_handle_);
#else
        const BaseType_t result = xTaskCreate(Task, "phone_bridge", 12288, this, 5, &task_handle_);
        ESP_LOGI(TAG, "Start task result=%d handle=%p stack_caps=internal", int(result),
                 task_handle_);
#endif
    }
}
void PhoneBridge::Task(void* arg) {
    ESP_LOGI(TAG, "Task entered");
    static_cast<PhoneBridge*>(arg)->Run();
    vTaskDelete(nullptr);
}

void PhoneBridge::Run() {
#if CONFIG_PHONE_BRIDGE_UART
    RunUart();
#else
    RunUdp();
#endif
}

#if CONFIG_PHONE_BRIDGE_UART
void PhoneBridge::RunUart() {
    if (resampler_selftest())
        ESP_LOGI(TAG, "UART_MEDIA selftest=passed baud=%d tx=%d rx=%d", kPhoneUartBaud,
                 kPhoneUartTx, kPhoneUartRx);
    else
        ESP_LOGE(TAG, "UART_MEDIA selftest=failed; continuing with logged interpolation path");

    const uart_config_t config = {.baud_rate = kPhoneUartBaud,
                                  .data_bits = UART_DATA_8_BITS,
                                  .parity = UART_PARITY_DISABLE,
                                  .stop_bits = UART_STOP_BITS_1,
                                  .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                                  .source_clk = UART_SCLK_DEFAULT};
    esp_err_t err = uart_driver_install(kPhoneUart, 4096, 2048, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "UART_MEDIA driver_install failed=%s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(uart_param_config(kPhoneUart, &config));
    ESP_ERROR_CHECK(uart_set_pin(kPhoneUart, kPhoneUartTx, kPhoneUartRx, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    SemaphoreHandle_t tx_lock = xSemaphoreCreateMutex();
    if (!tx_lock) {
        ESP_LOGE(TAG, "UART_MEDIA mutex allocation failed");
        return;
    }

    audio_.DiscardUplink();
    audio_.SetPcmTransport(true);
    audio_.EnablePcmCapture(true);
    audio_.EnableDeviceAec(false);
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    uint8_t rx[kUartFrameMax];
    size_t rx_used = 0;
    uint64_t session = 0;
    uint32_t generation = 0, expected_rx_seq = 0, tx_seq = 0;
    unsigned peer_rate = 16000;
    bool have_session = false;
    std::vector<int16_t> uplink_pending;
    uplink_pending.reserve(kFrame16 * 8);
    std::array<std::unique_ptr<AudioStreamPacket>, 8> prebuffer;
    size_t prebuffer_count = 0;
    bool prefilled = false;
    uint32_t rx_packets = 0, tx_packets = 0, rx_gaps = 0, tx_errors = 0, queue_drops = 0;
    uint32_t wire_bytes = 0, wire_frames = 0, wire_bad_crc = 0, wire_bad_header = 0;
    int64_t next_wire_diag_us = esp_timer_get_time() + INT64_C(5000000);

    auto clear_prebuffer = [&]() {
        for (auto& packet : prebuffer)
            packet.reset();
        prebuffer_count = 0;
        prefilled = false;
    };
    auto submit_pcm = [&](const uint8_t* payload, unsigned rate) {
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->pcm = true;
        packet->sample_rate = 24000;
        packet->frame_duration = 20;
        packet->payload.resize(480 * sizeof(int16_t));
        upsample_to_24k(reinterpret_cast<const int16_t*>(payload), rate / 50, rate,
                        reinterpret_cast<int16_t*>(packet->payload.data()));
        if (!prefilled) {
            prebuffer[prebuffer_count++] = std::move(packet);
            if (prebuffer_count == prebuffer.size()) {
                for (auto& pending : prebuffer)
                    if (!audio_.PushPacketToDecodeQueue(std::move(pending)))
                        ++queue_drops;
                prefilled = true;
            }
        } else if (!audio_.PushPacketToDecodeQueue(std::move(packet))) {
            ++queue_drops;
        }
    };
    ESP_LOGI(TAG, "UART_MEDIA ready waiting_for_hello");

    while (true) {
        uint8_t byte;
        if (uart_read_bytes(kPhoneUart, &byte, 1, pdMS_TO_TICKS(2)) == 1) {
            ++wire_bytes;
            if (rx_used == 0) {
                if (byte == 0x55)
                    rx[rx_used++] = byte;
            } else if (rx_used == 1 && byte != 0xaa) {
                rx_used = byte == 0x55 ? 1 : 0;
                if (rx_used)
                    rx[0] = byte;
            } else if (rx_used < sizeof(rx)) {
                rx[rx_used++] = byte;
                if (rx_used >= kUartFrameHeader) {
                    const size_t size = size_t(rx[4] << 8) | rx[5];
                    if (rx[2] != 1 || size > 32 + kFrame16 * 2) {
                        ++wire_bad_header;
                        rx_used = 0;
                    } else if (rx_used == kUartFrameHeader + size) {
                        const uint16_t crc = uart_r16(rx + 6);
                        if (esp_crc16_be(0, rx + kUartFrameHeader, size) == crc && size >= 32 &&
                            std::memcmp(rx + kUartFrameHeader, "HFAP", 4) == 0 &&
                            rx[kUartFrameHeader + 4] == 1) {
                            const uint8_t* p = rx + kUartFrameHeader;
                            const uint8_t kind = p[5];
                            const uint8_t flags = p[6];
                            const uint8_t codec = p[7];
                            const uint64_t incoming_session =
                                (uint64_t(uart_r32(p + 8)) << 32) | uart_r32(p + 12);
                            const uint32_t incoming_generation = uart_r32(p + 16);
                            const uint32_t sequence = uart_r32(p + 20);
                            const unsigned rate = uart_r16(p + 28);
                            const size_t payload_bytes = uart_r16(p + 30);
                            const size_t frame_samples = rate == 16000 ? kFrame16 : kFrame8;
                            if (incoming_session && incoming_generation &&
                                (rate == 8000 || rate == 16000) && codec == 1 &&
                                payload_bytes == frame_samples * 2 && size == 32 + payload_bytes &&
                                kind == 3 && (flags == 1 || flags == 3)) {
                                if (!have_session || incoming_session != session ||
                                    incoming_generation != generation || rate != peer_rate) {
                                    session = incoming_session;
                                    generation = incoming_generation;
                                    peer_rate = rate;
                                    expected_rx_seq = sequence;
                                    tx_seq = 0;
                                    have_session = true;
                                    clear_prebuffer();
                                    uplink_pending.clear();
                                    ESP_LOGI(TAG,
                                             "UART_MEDIA session=%" PRIu64 " generation=%" PRIu32
                                             " rate=%u",
                                             session, generation, peer_rate);
                                }
                                if (sequence >= expected_rx_seq &&
                                    sequence - expected_rx_seq < 32) {
                                    rx_gaps += sequence - expected_rx_seq;
                                    expected_rx_seq = sequence + 1;
                                    submit_pcm(p + 32, rate);
                                    ++rx_packets;
                                }
                            } else if (kind == 1 && incoming_session &&
                                       (rate == 8000 || rate == 16000) && payload_bytes == 0) {
                                const uint32_t accepted_generation =
                                    incoming_generation ? incoming_generation : 1;
                                session = incoming_session;
                                generation = accepted_generation;
                                peer_rate = rate;
                                expected_rx_seq = 0;
                                tx_seq = 0;
                                have_session = true;
                                clear_prebuffer();
                                uplink_pending.clear();
                                uint8_t ack[kUartFrameMax]{};
                                const size_t ack_size = uart_make_hfap(
                                    ack + kUartFrameHeader, sizeof(ack) - kUartFrameHeader, 2, 0, 1,
                                    session, generation, sequence, uint16_t(rate), nullptr, 0);
                                if (!ack_size ||
                                    !uart_send_frame(ack + kUartFrameHeader, ack_size, tx_lock)) {
                                    ESP_LOGW(TAG, "UART_MEDIA ack_send_failed session=%" PRIu64,
                                             session);
                                } else {
                                    ESP_LOGI(TAG,
                                             "UART_MEDIA ack_sent session=%" PRIu64
                                             " generation=%" PRIu32 " rate=%u",
                                             session, generation, peer_rate);
                                }
                                ESP_LOGI(TAG,
                                         "UART_MEDIA hello session=%" PRIu64 " generation=%" PRIu32
                                         " rate=%u",
                                         session, generation, peer_rate);
                            } else if (kind == 5 && incoming_session == session &&
                                       incoming_generation == generation) {
                                have_session = false;
                                clear_prebuffer();
                                uplink_pending.clear();
                                ESP_LOGI(TAG, "UART_MEDIA stop");
                            }
                            ++wire_frames;
                        } else {
                            ++wire_bad_crc;
                        }
                        rx_used = 0;
                    }
                }
            }
        }

        for (unsigned i = 0; i < 4 && have_session; ++i) {
            auto packet = audio_.PopPacketFromSendQueue();
            if (!packet || !packet->pcm || packet->sample_rate != 16000)
                break;
            const size_t samples = packet->payload.size() / sizeof(int16_t);
            const int16_t* pcm = reinterpret_cast<const int16_t*>(packet->payload.data());
            uplink_pending.insert(uplink_pending.end(), pcm, pcm + samples);
        }
        const size_t input_samples = kFrame16;
        const size_t output_samples = peer_rate == 16000 ? kFrame16 : kFrame8;
        if (have_session && uplink_pending.size() >= input_samples) {
            uint8_t packet[32 + kFrame16 * 2];
            uint8_t payload[kFrame16 * 2];
            for (size_t i = 0; i < output_samples; ++i) {
                const size_t source = peer_rate == 16000 ? i : i * 2;
                const int16_t sample = uplink_pending[source];
                payload[i * 2] = uint8_t(sample);
                payload[i * 2 + 1] = uint8_t(sample >> 8);
            }
            const size_t size =
                uart_make_hfap(packet, sizeof(packet), 3, 0, 1, session, generation, tx_seq,
                               uint16_t(peer_rate), payload, uint16_t(output_samples * 2));
            if (!uart_send_frame(packet, size, tx_lock))
                ++tx_errors;
            else
                ++tx_packets;
            uplink_pending.erase(uplink_pending.begin(), uplink_pending.begin() + input_samples);
            ++tx_seq;
        }
        if ((rx_packets && rx_packets % 250 == 0) || (tx_packets && tx_packets % 250 == 0)) {
            ESP_LOGI(TAG,
                     "UART_MEDIA rx=%" PRIu32 " tx=%" PRIu32 " gaps=%" PRIu32
                     " queue_drops=%" PRIu32 " tx_errors=%" PRIu32,
                     rx_packets, tx_packets, rx_gaps, queue_drops, tx_errors);
        }
        if (esp_timer_get_time() >= next_wire_diag_us) {
            ESP_LOGI(TAG,
                     "UART_MEDIA wire bytes=%" PRIu32 " frames=%" PRIu32 " bad_crc=%" PRIu32
                     " bad_header=%" PRIu32 " media_rx=%" PRIu32 " media_tx=%" PRIu32,
                     wire_bytes, wire_frames, wire_bad_crc, wire_bad_header, rx_packets,
                     tx_packets);
            next_wire_diag_us = esp_timer_get_time() + INT64_C(5000000);
        }
        vTaskDelay(1);
    }
    vSemaphoreDelete(tx_lock);
}
#endif

void PhoneBridge::RunUdp() {
    configASSERT(resampler_selftest());
    configASSERT(ima_adpcm_selftest());
    configASSERT(phone_downlink_selftest());
    ESP_LOGI(TAG, "IMA_ADPCM selftest=passed independent_audioop_vector malformed_headers");
    ESP_LOGI(TAG, "PCM resampler selftest=passed rates=8000,16000 output=24000");
    ESP_LOGI(TAG, "PCM downlink selftest=passed wrap late bounded_fade rebuffer");
    // Previous and current maximum-size source frames, allocated once outside
    // the audio path. PSRAM is preferred; no-PSRAM boards have a 1280-byte budget.
    constexpr uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t external_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const uint32_t history_caps =
        heap_caps_get_total_size(external_caps) ? external_caps : internal_caps;
    std::unique_ptr<int16_t, decltype(&heap_caps_free)> previous_pcm(
        static_cast<int16_t*>(heap_caps_malloc(2 * kFrame16 * sizeof(int16_t), history_caps)),
        heap_caps_free);
    if (!previous_pcm) {
        ESP_LOGE(TAG,
                 "downlink history allocation failed bytes=%u caps=0x%" PRIx32
                 " internal_free=%u min=%u largest=%u psram_free=%u largest=%u",
                 unsigned(2 * kFrame16 * sizeof(int16_t)), history_caps,
                 unsigned(heap_caps_get_free_size(internal_caps)),
                 unsigned(heap_caps_get_minimum_free_size(internal_caps)),
                 unsigned(heap_caps_get_largest_free_block(internal_caps)),
                 unsigned(heap_caps_get_free_size(external_caps)),
                 unsigned(heap_caps_get_largest_free_block(external_caps)));
        return;
    }
    int16_t* decoded = previous_pcm.get() + kFrame16;
    // The application constructs the bridge before the board's Wi-Fi path has
    // initialized lwIP. Creating a socket earlier reaches lwIP with an
    // uninitialized tcpip mailbox and triggers an IDF assertion. Keep the
    // task alive while the network stack comes up; the listener itself is
    // independent of the later DHCP result.
    vTaskDelay(pdMS_TO_TICKS(5000));
    audio_.DiscardUplink();
    audio_.EnablePcmCapture(false);
    audio_.EnableDeviceAec(false);
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed");
        return;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind failed errno=%d", errno);
        close(fd);
        return;
    }
#if CONFIG_LWIP_SO_RCVBUF
    // Bound bytes separately from the per-socket mailbox packet count. Wi-Fi
    // payload allocation follows the board's supported PSRAM configuration.
    int requested_rcvbuf = 32768;
    int actual_rcvbuf = 0;
    socklen_t rcvbuf_len = sizeof(actual_rcvbuf);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &requested_rcvbuf, sizeof(requested_rcvbuf)) < 0 ||
        getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual_rcvbuf, &rcvbuf_len) < 0 ||
        actual_rcvbuf < requested_rcvbuf) {
        ESP_LOGE(TAG, "UDP receive buffer setup failed requested=%d actual=%d errno=%d",
                 requested_rcvbuf, actual_rcvbuf, errno);
        close(fd);
        return;
    }
    ESP_LOGI(TAG, "UDP receive buffer requested=%d actual=%d mailbox_packets=%d", requested_rcvbuf,
             actual_rcvbuf, CONFIG_LWIP_UDP_RECVMBOX_SIZE);
#endif
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "UDP nonblocking setup failed errno=%d", errno);
        close(fd);
        return;
    }
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    bool have_session = false;
    uint64_t session = 0;
    uint32_t generation = 0;
    uint32_t tx_seq = 0;
    unsigned peer_rate = 8000;
    ESP_LOGI(TAG, "UDP listening port=%u", kPort);
    // One extra byte makes truncated oversized datagrams fail exact length checks.
    uint8_t buf[673];
    uint32_t rx_packets = 0;
    uint32_t tx_packets = 0;
    uint32_t tx_errors = 0;
    uint32_t rx_socket_errors = 0;
    unsigned rx_burst_peak = 0;
    uint8_t tx_adpcm_index = 32;
    unsigned tx_codec = 1;
    int64_t next_tx_us = 0;
    uint32_t tx_queue_dropped_frames = 0;
    uint32_t expected_seq = 0, sequence_gaps = 0, reordered = 0, queue_drops = 0;
    uint32_t tx_window_samples = 0;
    int64_t tx_window_sum_sq = 0;
    int16_t tx_window_peak = 0;
    int64_t last_rx_us = 0, max_gap_us = 0;
    int64_t last_power_keepalive_us = 0;
    std::array<std::unique_ptr<AudioStreamPacket>, 8> prebuffer;
    size_t prebuffer_count = 0;
    bool prefilled = false;
    bool have_previous_pcm = false;
    uint32_t concealed_packets = 0, rebuffer_count = 0;
    uint32_t source_concealed_packets = 0;
    std::vector<int16_t> uplink_pending;
    PhoneRateConverter ai_input_converter, ai_output_converter;
    unsigned ai_peer_rate = 0;
    uint32_t route_epoch = ai_epoch_.load();
    int16_t ai_frame[480]{}, wire_pcm[320]{};
    constexpr size_t kMaxUplinkSamples = kFrame16 * 12;
    uplink_pending.reserve(kMaxUplinkSamples + 960);
    ESP_LOGI(TAG, "PCM prebuffer_ms=160 conceal_limit_ms=100 history_caps=0x%" PRIx32,
             history_caps);
    auto clear_prebuffer = [&]() {
        for (auto& pending : prebuffer)
            pending.reset();
        prebuffer_count = 0;
        prefilled = false;
        have_previous_pcm = false;
    };
    auto submit_pcm = [&](const int16_t* samples) {
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->pcm = true;
        packet->sample_rate = 24000;
        packet->frame_duration = 20;
        packet->payload.resize(480 * sizeof(int16_t));
        upsample_to_24k(samples, peer_rate / 50, peer_rate,
                        reinterpret_cast<int16_t*>(packet->payload.data()));
        if (!prefilled) {
            prebuffer[prebuffer_count++] = std::move(packet);
            if (prebuffer_count == prebuffer.size()) {
                for (auto& pending : prebuffer)
                    if (!audio_.PushPacketToDecodeQueue(std::move(pending)))
                        ++queue_drops;
                prefilled = true;
            }
        } else if (!audio_.PushPacketToDecodeQueue(std::move(packet))) {
            ++queue_drops;
        }
    };
    while (true) {
        // Keep the uplink queue drained under Bluetooth/Wi-Fi coexistence. A single
        // 60 ms capture packet may accumulate while the task is briefly preempted.
        vTaskDelay(pdMS_TO_TICKS(2) > 0 ? pdMS_TO_TICKS(2) : 1);
        // Stop transmitting to a stale authenticated UDP peer after unbinding
        // or control-link loss. UART owns its separate lifetime in RunUart().
#if CONFIG_GATEWAY_MANAGER
        if (have_session && !gateway_manager_ready()) {
            InvalidateAiMedia("control_lost", PhoneAiFailure::GatewayControl);
            have_session = false;
            uplink_pending.clear();
            clear_prebuffer();
            audio_.DiscardUplink();
            ESP_LOGW(TAG, "UDP control not ready; stop stale uplink");
        }
#endif
        if (have_session && last_rx_us && esp_timer_get_time() - last_rx_us > 1000000) {
            InvalidateAiMedia("media_timeout", PhoneAiFailure::GatewayMediaTimeout);
            ESP_LOGW(TAG, "phone media expired; stop stale uplink generation=%" PRIu32, generation);
            have_session = false;
            uplink_pending.clear();
            clear_prebuffer();
            audio_.DiscardUplink();
        }
        const bool ai_route = ai_route_.load();
        if (route_epoch != ai_epoch_.load()) {
            route_epoch = ai_epoch_.load();
            ai_peer_rate = 0;
            uplink_pending.clear();
            clear_prebuffer();
            audio_.DiscardUplink();
        }
        bool call_active = false;
#if CONFIG_GATEWAY_MANAGER
        phone_status_t phone_status{};
        int64_t phone_age_ms = 0;
        uint32_t phone_duration_ms = 0;
        const bool phone_fresh =
            gateway_manager_get_phone_status(&phone_status, &phone_age_ms, &phone_duration_ms);
        // A missed control heartbeat is unknown, not a hangup. Preserve the
        // current call only while authenticated media is still arriving; the
        // one-second media lease above bounds recovery on an actual link loss.
        call_active =
            phone_fresh ? phone_status.phase == PHONE_ACTIVE : last_call_active_ && have_session;
#endif
        const bool local = have_session && !ai_route && !call_active;
        if (local_audio_.exchange(local) != local) {
            audio_.DiscardUplink();
            audio_.EnablePcmCapture(local);
        }
        if (call_active != last_call_active_) {
            last_call_active_ = call_active;
            if (callbacks_.on_call_active)
                callbacks_.on_call_active(call_active);
            ESP_LOGI(TAG, "PHONE_ROUTE call_active=%d ai_route=%d", call_active, ai_route);
        }
        // Drain a bounded burst before yielding to capture/transmit. Sleeping
        // one 10 ms tick per datagram magnifies a short scheduling stall; an
        // unbounded drain would instead starve the uplink under socket load.
        for (unsigned received = 0; received < 8; ++received) {
            sockaddr_in incoming_peer{};
            socklen_t incoming_peer_len = sizeof(incoming_peer);
            const int n = recvfrom(fd, buf, sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&incoming_peer), &incoming_peer_len);
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ++rx_socket_errors;
                    if (rx_socket_errors == 1 || rx_socket_errors % 100 == 0)
                        ESP_LOGW(TAG, "UDP receive failed count=%" PRIu32 " errno=%d",
                                 rx_socket_errors, errno);
                }
                break;
            }
            rx_burst_peak = std::max(rx_burst_peak, received + 1);
#if CONFIG_GATEWAY_MANAGER
            if (!gateway_manager_accepts(incoming_peer.sin_addr.s_addr))
                continue;
#endif
            const unsigned rate = n >= int(kHeader) ? r16(buf + 28) : 0;
            const size_t frame = rate == 16000 ? kFrame16 : kFrame8;
            if (n >= int(kHeader) && n == int(kHeader + r16(buf + 30)) &&
                std::memcmp(buf, "HFAP", 4) == 0 && buf[4] == 1 && buf[5] == 3 &&
                (buf[6] == 1 || buf[6] == 3) && (rate == 8000 || rate == 16000) &&
                phone_downlink_payload_valid(buf[7], buf + kHeader, n - kHeader, frame) &&
                r32(buf + 24) == r32(buf + 20) * uint32_t(frame)) {
                const uint64_t incoming_session = (uint64_t(r32(buf + 8)) << 32) | r32(buf + 12);
                const uint32_t incoming_generation = r32(buf + 16);
                const uint32_t sequence = r32(buf + 20);
                if (!incoming_session || !incoming_generation)
                    continue;
                peer = incoming_peer;
                peer_len = incoming_peer_len;
#if CONFIG_GATEWAY_MANAGER
                gateway_manager_note_media(false);
#endif
                if (!have_session || incoming_session != session ||
                    incoming_generation != generation || rate != peer_rate) {
                    if (have_session && (incoming_session != session ||
                                         incoming_generation != generation || rate != peer_rate))
                        InvalidateAiMedia("new_generation", PhoneAiFailure::GatewayMediaChanged);
                    // Startup/idle handling can lower Wi-Fi power after this task starts.
                    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                    expected_seq = sequence;
                    sequence_gaps = reordered = queue_drops = 0;
                    concealed_packets = rebuffer_count = 0;
                    source_concealed_packets = 0;
                    last_rx_us = max_gap_us = 0;
                    tx_seq = 0;
                    next_tx_us = 0;
                    tx_adpcm_index = 32;
                    uplink_pending.clear();
                    ai_peer_rate = 0;
                    clear_prebuffer();
                }
                bool late = false, large_loss = false;
                const uint32_t missing =
                    phone_downlink_missing(expected_seq, sequence, &late, &large_loss);
                const int32_t delta = int32_t(sequence - expected_seq);
                if (late) {
                    ++reordered;
                    continue;
                }
                sequence_gaps += uint32_t(delta);
                expected_seq = sequence + 1;
                const int64_t now_us = esp_timer_get_time();
                // The board power-save timer only knows about the normal XiaoZhi
                // application state. A phone bridge session can be active while
                // that state remains idle, so periodically re-arm the timer while
                // UDP audio is flowing. Without this, the display/backlight can
                // enter its 60-second idle mode during a call even though audio
                // tasks and the bridge are still running.
                if (!last_power_keepalive_us || now_us - last_power_keepalive_us >= 5000000) {
                    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                    last_power_keepalive_us = now_us;
                    ESP_LOGI(TAG, "phone session active; power-save timer rearmed");
                }
                if (last_rx_us && now_us - last_rx_us > max_gap_us)
                    max_gap_us = now_us - last_rx_us;
                if (large_loss || (last_rx_us && now_us - last_rx_us > 200000)) {
                    clear_prebuffer();
                    ++rebuffer_count;
                }
                last_rx_us = now_us;
                session = incoming_session;
                generation = incoming_generation;
                peer_rate = rate;
                have_session = session != 0 && generation != 0;
                const uint8_t* in = buf + kHeader;
                if (have_previous_pcm) {
                    for (uint32_t lost = 1; lost <= missing; ++lost) {
                        phone_downlink_conceal(previous_pcm.get(), decoded, frame, lost);
                        if (!ai_route && local_audio_.load())
                            submit_pcm(decoded);
                        ++concealed_packets;
                    }
                }
                if (!phone_downlink_decode(buf[7], in, n - kHeader, frame,
                                           reinterpret_cast<uint8_t*>(decoded), kFrame16 * 2))
                    continue;
                CallRecords::Instance().Capture(false, decoded, frame, peer_rate, sequence,
                                                incoming_generation);
                if (ai_route) {
                    auto packet = std::make_unique<AudioStreamPacket>();
                    packet->pcm = true;
                    packet->sample_rate = 16000;
                    packet->frame_duration = 20;
                    const size_t output_samples = peer_rate == 16000 ? frame : frame * 2;
                    packet->payload.resize(output_samples * sizeof(int16_t));
                    auto* output = reinterpret_cast<int16_t*>(packet->payload.data());
                    if (ai_peer_rate != peer_rate) {
                        ai_input_converter.Configure(peer_rate, 16000);
                        ai_output_converter.Configure(24000, peer_rate);
                        ai_peer_rate = peer_rate;
                    }
                    if (peer_rate == 16000) {
                        std::memcpy(output, decoded, frame * sizeof(int16_t));
                    } else {
                        ai_input_converter.Process(decoded, frame, output, output_samples);
                    }
                    std::lock_guard<std::mutex> lock(ai_mutex_);
                    if (ai_route_.load() && route_epoch == ai_epoch_.load()) {
                        if (phone_uplink_.size() >= 24) {
                            phone_uplink_.pop_front();
                            if (++ai_uplink_drops_ == 1 || ai_uplink_drops_ % 50 == 0)
                                ESP_LOGW(TAG, "PHONE_AI uplink queue dropped=%lu",
                                         (unsigned long)ai_uplink_drops_);
                        }
                        phone_uplink_.push_back(std::move(packet));
                    }
                } else if (local_audio_.load()) {
                    submit_pcm(decoded);
                }
                std::memcpy(previous_pcm.get(), decoded, frame * sizeof(int16_t));
                have_previous_pcm = true;
                // Flag 2 means the sender already concealed HFP input before UDP.
                // Keep this separate from sequence gaps concealed by this receiver.
                if (buf[6] & 2)
                    ++source_concealed_packets;
                if (++rx_packets == 1 || (rx_packets % 250) == 0)
                    ESP_LOGI(TAG,
                             "RX packets=%" PRIu32 " rate=%u seq=%" PRIu32 " gaps=%" PRIu32
                             " reordered=%" PRIu32 " queue_drops=%" PRIu32 " max_gap_ms=%" PRId64
                             " plc_packets=%" PRIu32 " rebuffers=%" PRIu32
                             " stack_low_bytes=%u codec=%u payload_bytes=%u"
                             " rx_burst_peak=%u rx_socket_errors=%" PRIu32
                             " source_concealed_packets=%" PRIu32,
                             rx_packets, rate, sequence, sequence_gaps, reordered, queue_drops,
                             max_gap_us / 1000, concealed_packets, rebuffer_count,
                             unsigned(uxTaskGetStackHighWaterMark(nullptr)), unsigned(buf[7]),
                             unsigned(n - kHeader), rx_burst_peak, rx_socket_errors,
                             source_concealed_packets);
            }
        }
        for (int i = 0; i < 4; ++i) {
            if (!local_audio_.load())
                break;
            auto packet = audio_.PopPacketFromSendQueue();
            if (!packet || !packet->pcm || packet->sample_rate != 16000 || !have_session)
                break;
            const size_t samples = packet->payload.size() / 2;
            if (!samples)
                continue;
            const int16_t* packet_pcm = reinterpret_cast<const int16_t*>(packet->payload.data());
            uplink_pending.insert(uplink_pending.end(), packet_pcm, packet_pcm + samples);
            if (uplink_pending.size() > kMaxUplinkSamples) {
                const size_t dropped_frames =
                    (uplink_pending.size() - kMaxUplinkSamples + kFrame16 - 1) / kFrame16;
                uplink_pending.erase(uplink_pending.begin(),
                                     uplink_pending.begin() + dropped_frames * kFrame16);
                tx_seq += dropped_frames;
                const uint32_t previous_drops = tx_queue_dropped_frames;
                tx_queue_dropped_frames += dropped_frames;
                if (!previous_drops || previous_drops / 50 != tx_queue_dropped_frames / 50)
                    ESP_LOGW(TAG, "TX source queue dropped_frames=%" PRIu32,
                             tx_queue_dropped_frames);
            }
        }
        const size_t out_samples = peer_rate == 16000 ? kFrame16 : kFrame8;
        const int64_t tx_now_us = esp_timer_get_time();
        // Capture arrives in 60 ms blocks. Pace the three media datagrams instead
        // of submitting them together to the Wi-Fi/Bluetooth coexistence window.
        // After a scheduling stall, permit at most one packet per task iteration.
        if (have_session && (ai_route || uplink_pending.size() >= kFrame16) &&
            tx_now_us >= next_tx_us) {
            next_tx_us = std::max(next_tx_us + INT64_C(20000), tx_now_us + INT64_C(10000));
            if (ai_route) {
                std::lock_guard<std::mutex> lock(ai_mutex_);
                const size_t samples = ai_downlink_.Pop(ai_frame, 480);
                if (samples) {
                    ai_played_us_ = tx_now_us;
                    ai_tx_samples_ += samples;
                }
                ai_output_converter.Process(ai_frame, 480, wire_pcm, out_samples);
            } else {
                for (size_t i = 0; i < out_samples; ++i)
                    wire_pcm[i] = uplink_pending[peer_rate == 16000 ? i : i * 2];
            }
            uint8_t out[672]{};
            std::memcpy(out, "HFAP", 4);
            out[4] = 1;
            out[5] = 3;
            out[6] = 0;  // S3 -> HF direction.
            out[7] = 1;
            w32(out + 8, uint32_t(session >> 32));
            w32(out + 12, uint32_t(session));
            w32(out + 16, generation);
            w32(out + 20, tx_seq);
            w32(out + 24, tx_seq * out_samples);
            w16(out + 28, uint16_t(peer_rate));
            w16(out + 30, uint16_t(out_samples * 2));
            for (size_t i = 0; i < out_samples; ++i) {
                const int16_t s = wire_pcm[i];
                tx_window_sum_sq += int64_t(s) * s;
                tx_window_peak = std::max<int16_t>(tx_window_peak, std::abs(s));
                ++tx_window_samples;
                out[kHeader + i * 2] = uint8_t(s);
                out[kHeader + i * 2 + 1] = uint8_t(s >> 8);
            }
            size_t payload_bytes = out_samples * 2;
#if CONFIG_GATEWAY_MANAGER
            if (gateway_manager_peer_audio_codecs() & 2) {
                size_t compressed = ima_adpcm_encode(wire_pcm, out_samples, 1, out + kHeader,
                                                     sizeof(out) - kHeader, &tx_adpcm_index);
                // Bounds/rate/index are owned here; fail the session explicitly
                // if these invariants ever break rather than sending malformed PCM.
                if (!compressed) {
                    ESP_LOGE(TAG, "IMA encoder rejected samples=%u index=%u", unsigned(out_samples),
                             tx_adpcm_index);
                    have_session = false;
                    uplink_pending.clear();
                    continue;
                }
                payload_bytes = compressed;
                out[7] = 2;
                w16(out + 30, uint16_t(payload_bytes));
            }
#endif
            if (tx_codec != out[7]) {
                tx_codec = out[7];
                ESP_LOGI(TAG, "TX codec=%u payload_bytes=%u reason=authenticated_peer_capability",
                         tx_codec, unsigned(payload_bytes));
            }
            const int sent = sendto(fd, out, kHeader + payload_bytes, 0,
                                    reinterpret_cast<sockaddr*>(&peer), peer_len);
            if (sent == int(kHeader + payload_bytes))
                CallRecords::Instance().Capture(true, wire_pcm, out_samples, peer_rate);
#if CONFIG_GATEWAY_MANAGER
            if (sent == int(kHeader + payload_bytes))
                gateway_manager_note_media(true);
#endif
            if (sent != int(kHeader + payload_bytes)) {
                const int send_errno = errno;
                if (++tx_errors == 1 || (tx_errors % 50) == 0) {
                    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
                    constexpr uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
                    ESP_LOGW(TAG,
                             "UDP send failed errno=%d sent=%d payload_bytes=%u errors=%" PRIu32
                             " internal_caps=0x%" PRIx32
                             " free=%u min=%u largest=%u"
                             " psram_caps=0x%" PRIx32 " free=%u largest=%u",
                             send_errno, sent, unsigned(kHeader + payload_bytes), tx_errors,
                             internal, unsigned(heap_caps_get_free_size(internal)),
                             unsigned(heap_caps_get_minimum_free_size(internal)),
                             unsigned(heap_caps_get_largest_free_block(internal)), external,
                             unsigned(heap_caps_get_free_size(external)),
                             unsigned(heap_caps_get_largest_free_block(external)));
                }
            }
            if (!ai_route)
                uplink_pending.erase(uplink_pending.begin(), uplink_pending.begin() + kFrame16);
            tx_seq++;
            if (++tx_packets == 1 || (tx_packets % 250) == 0) {
                const uint32_t rms =
                    tx_window_samples
                        ? static_cast<uint32_t>(
                              std::sqrt(static_cast<double>(tx_window_sum_sq) / tx_window_samples))
                        : 0;
                ESP_LOGI(TAG,
                         "TX packets=%" PRIu32 " rate=%u rms=%" PRIu32 " peak=%d samples=%" PRIu32,
                         tx_packets, peer_rate, rms, tx_window_peak, tx_window_samples);
                tx_window_samples = 0;
                tx_window_sum_sq = 0;
                tx_window_peak = 0;
            }
        }
    }
}

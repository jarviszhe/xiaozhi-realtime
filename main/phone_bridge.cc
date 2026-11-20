#include "phone_bridge.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <lwip/sockets.h>
#include <sys/socket.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "phone_bridge";
static constexpr uint16_t kPort = 39001;
static constexpr size_t kHeader = 32;
static constexpr size_t kFrame8 = 160;
static constexpr size_t kFrame16 = 320;

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
            if (output[i] != a + (b - a) * frac / 24000)
                return false;
        }
    }
    return true;
}

void PhoneBridge::Start() {
    if (!task_handle_) {
        const BaseType_t result =
            xTaskCreate(Task, "phone_bridge", 4096, this, 5, &task_handle_);
        ESP_LOGI(TAG, "Start task result=%d handle=%p", int(result), task_handle_);
    }
}
void PhoneBridge::Task(void* arg) {
    ESP_LOGI(TAG, "Task entered");
    static_cast<PhoneBridge*>(arg)->Run();
    vTaskDelete(nullptr);
}

void PhoneBridge::Run() {
    configASSERT(resampler_selftest());
    ESP_LOGI(TAG, "PCM resampler selftest=passed rates=8000,16000 output=24000");
    // The application constructs the bridge before the board's Wi-Fi path has
    // initialized lwIP. Creating a socket earlier reaches lwIP with an
    // uninitialized tcpip mailbox and triggers an IDF assertion. Keep the
    // task alive while the network stack comes up; the listener itself is
    // independent of the later DHCP result.
    vTaskDelay(pdMS_TO_TICKS(5000));
    audio_.DiscardUplink();
    audio_.SetPcmTransport(true);
    audio_.EnablePcmCapture(true);
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
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    bool have_session = false;
    uint64_t session = 0;
    uint32_t generation = 0;
    uint32_t tx_seq = 0;
    unsigned peer_rate = 8000;
    ESP_LOGI(TAG, "UDP listening port=%u", kPort);
    uint8_t buf[672];
    uint32_t rx_packets = 0;
    uint32_t tx_packets = 0;
    uint32_t expected_seq = 0, sequence_gaps = 0, reordered = 0, queue_drops = 0;
    uint32_t tx_window_samples = 0;
    int64_t tx_window_sum_sq = 0;
    int16_t tx_window_peak = 0;
    int64_t last_rx_us = 0, max_gap_us = 0;
    int64_t last_power_keepalive_us = 0;
    std::array<std::unique_ptr<AudioStreamPacket>, 4> prebuffer;
    size_t prebuffer_count = 0;
    bool prefilled = false;
    std::vector<int16_t> uplink_pending;
    uplink_pending.reserve(960);
    ESP_LOGI(TAG, "PCM prebuffer_ms=80");
    while (true) {
        // Keep the uplink queue drained under Bluetooth/Wi-Fi coexistence. A single
        // 60 ms capture packet may accumulate while the task is briefly preempted.
        vTaskDelay(pdMS_TO_TICKS(2) > 0 ? pdMS_TO_TICKS(2) : 1);
        int n = recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        const unsigned rate = n >= int(kHeader) ? r16(buf + 28) : 0;
        const size_t frame = rate == 16000 ? kFrame16 : kFrame8;
        if (n == int(kHeader + frame * 2) && std::memcmp(buf, "HFAP", 4) == 0 && buf[4] == 1 &&
            buf[5] == 3 && (buf[6] == 1 || buf[6] == 3) && buf[7] == 1 &&
            (rate == 8000 || rate == 16000) && r16(buf + 30) == frame * 2 &&
            r32(buf + 24) == r32(buf + 20) * uint32_t(frame)) {
            const uint64_t incoming_session = (uint64_t(r32(buf + 8)) << 32) | r32(buf + 12);
            const uint32_t incoming_generation = r32(buf + 16);
            const uint32_t sequence = r32(buf + 20);
            if (!incoming_session || !incoming_generation)
                continue;
            if (!have_session || incoming_session != session || incoming_generation != generation) {
                // Startup/idle handling can lower Wi-Fi power after this task starts.
                Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                expected_seq = sequence;
                sequence_gaps = reordered = queue_drops = 0;
                last_rx_us = max_gap_us = 0;
                tx_seq = 0;
                for (auto& pending : prebuffer)
                    pending.reset();
                prebuffer_count = 0;
                prefilled = false;
            }
            const int32_t delta = int32_t(sequence - expected_seq);
            if (delta < 0) {
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
            if (last_rx_us && now_us - last_rx_us > 200000) {
                for (auto& pending : prebuffer)
                    pending.reset();
                prebuffer_count = 0;
                prefilled = false;
            }
            last_rx_us = now_us;
            session = incoming_session;
            generation = incoming_generation;
            peer_rate = rate;
            have_session = session != 0 && generation != 0;
            std::vector<int16_t> pcm(frame * 24000 / rate);
            const uint8_t* in = buf + kHeader;
            std::vector<int16_t> decoded(frame);
            for (size_t i = 0; i < frame; ++i)
                decoded[i] = int16_t(uint16_t(in[i * 2]) | (uint16_t(in[i * 2 + 1]) << 8));
            upsample_to_24k(decoded.data(), frame, rate, pcm.data());
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->pcm = true;
            packet->sample_rate = 24000;
            packet->frame_duration = 20;
            packet->payload.resize(pcm.size() * 2);
            std::memcpy(packet->payload.data(), pcm.data(), packet->payload.size());
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
            if (++rx_packets == 1 || (rx_packets % 250) == 0)
                ESP_LOGI(TAG,
                         "RX packets=%" PRIu32 " rate=%u seq=%" PRIu32 " gaps=%" PRIu32
                         " reordered=%" PRIu32 " queue_drops=%" PRIu32 " max_gap_ms=%" PRId64,
                         rx_packets, rate, sequence, sequence_gaps, reordered, queue_drops,
                         max_gap_us / 1000);
        }
        for (int i = 0; i < 4; ++i) {
            auto packet = audio_.PopPacketFromSendQueue();
            if (!packet || !packet->pcm || packet->sample_rate != 16000 || !have_session)
                break;
            const size_t samples = packet->payload.size() / 2;
            if (!samples)
                continue;
            const int16_t* packet_pcm = reinterpret_cast<const int16_t*>(packet->payload.data());
            uplink_pending.insert(uplink_pending.end(), packet_pcm, packet_pcm + samples);
            const size_t out_samples = peer_rate == 16000 ? kFrame16 : kFrame8;
            while (uplink_pending.size() >= (peer_rate == 16000 ? kFrame16 : kFrame8)) {
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
                    const size_t src_index = peer_rate == 16000 ? i : i * 2;
                    int16_t s = uplink_pending[src_index];
                    tx_window_sum_sq += int64_t(s) * s;
                    tx_window_peak = std::max<int16_t>(tx_window_peak, std::abs(s));
                    ++tx_window_samples;
                    out[kHeader + i * 2] = uint8_t(s);
                    out[kHeader + i * 2 + 1] = uint8_t(s >> 8);
                }
                const int sent = sendto(fd, out, kHeader + out_samples * 2, 0,
                                        reinterpret_cast<sockaddr*>(&peer), peer_len);
                if (sent != int(kHeader + out_samples * 2) &&
                    (tx_packets == 0 || (tx_packets % 50) == 0))
                    ESP_LOGW(TAG, "UDP send failed errno=%d sent=%d len=%u", errno, sent,
                             unsigned(kHeader + out_samples * 2));
                uplink_pending.erase(uplink_pending.begin(),
                                     uplink_pending.begin() + (peer_rate == 16000 ? kFrame16 : kFrame8));
                tx_seq++;
                if (++tx_packets == 1 || (tx_packets % 250) == 0) {
                    const uint32_t rms = tx_window_samples
                                             ? static_cast<uint32_t>(std::sqrt(
                                                   static_cast<double>(tx_window_sum_sq) /
                                                   tx_window_samples))
                                             : 0;
                    ESP_LOGI(TAG, "TX packets=%" PRIu32 " rate=%u rms=%" PRIu32
                                  " peak=%d samples=%" PRIu32,
                             tx_packets, peer_rate, rms, tx_window_peak, tx_window_samples);
                    tx_window_samples = 0;
                    tx_window_sum_sq = 0;
                    tx_window_peak = 0;
                }
            }
        }
    }
}

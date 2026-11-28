// SPDX-License-Identifier: MIT
// Read-only diagnostics for the ESP-IDF 6.x lwIP socket owner. Keep the paired
// gateway copy identical. The TCP/IP callback never logs or changes a PCB.
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/api.h"
#include "lwip/priv/sockets_priv.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcpip.h"

typedef struct {
    atomic_int state;  // 0 idle, 1 one queued callback, 2 result ready.
    int fd;
    bool writing, valid;
    int64_t elapsed_ms, queued_us, queue_delay_ms;
    unsigned session, tcp_state, retries, rto_ms, inflight, unsent, unacked;
    unsigned send_window, receive_window;
    int receive_events;
} gateway_tcp_diag_t;
static gateway_tcp_diag_t s_tcp_diag;
static unsigned s_tcp_diag_session;

static void gateway_tcp_diag_collect(void* arg) {
    gateway_tcp_diag_t* d = arg;
    d->queue_delay_ms = (esp_timer_get_time() - d->queued_us) / 1000;
    struct lwip_sock* socket = lwip_socket_dbg_get_socket(d->fd);
    d->valid = socket && socket->conn && NETCONNTYPE_GROUP(socket->conn->type) == NETCONN_TCP &&
               socket->conn->pcb.tcp;
    if (d->valid) {
        const struct tcp_pcb* pcb = socket->conn->pcb.tcp;
        d->tcp_state = pcb->state;
        d->retries = pcb->nrtx;
        d->rto_ms = pcb->rto * TCP_SLOW_INTERVAL;
        d->inflight = pcb->snd_nxt - pcb->lastack;
        d->unsent = pcb->unsent != NULL;
        d->unacked = pcb->unacked != NULL;
        d->send_window = pcb->snd_wnd;
        d->receive_window = pcb->rcv_wnd;
#if LWIP_SOCKET_SELECT || LWIP_SOCKET_POLL
        d->receive_events = socket->rcvevent;
#endif
    }
    atomic_store_explicit(&d->state, 2, memory_order_release);
}
static void gateway_tcp_diag_poll(void) {
    gateway_tcp_diag_t* d = &s_tcp_diag;
    if (atomic_load_explicit(&d->state, memory_order_acquire) != 2)
        return;
    ESP_LOGW("gateway_manager",
             "control_tcp session=%u op=%s waited_ms=%lld queue_delay_ms=%lld valid=%d state=%u "
             "retries=%u "
             "rto_ms=%u inflight=%u unsent=%u unacked=%u snd_wnd=%u rcv_wnd=%u "
             "recv_events=%d stack_low_bytes=%u",
             d->session, d->writing ? "send" : "recv", (long long)d->elapsed_ms,
             (long long)d->queue_delay_ms, d->valid, d->tcp_state, d->retries, d->rto_ms,
             d->inflight, d->unsent, d->unacked, d->send_window, d->receive_window,
             d->receive_events, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    atomic_store_explicit(&d->state, 0, memory_order_release);
}
static void gateway_tcp_diag_request(int fd, bool writing, int64_t elapsed_ms) {
    gateway_tcp_diag_poll();
    gateway_tcp_diag_t* d = &s_tcp_diag;
    if (atomic_load_explicit(&d->state, memory_order_acquire))
        return;
    d->fd = fd;
    d->writing = writing;
    d->elapsed_ms = elapsed_ms;
    d->queued_us = esp_timer_get_time();
    d->session = s_tcp_diag_session;
    d->valid = false;
    d->tcp_state = d->retries = d->rto_ms = d->inflight = d->unsent = d->unacked = 0;
    d->send_window = d->receive_window = d->receive_events = 0;
    atomic_store_explicit(&d->state, 1, memory_order_release);
    // A single static request survives a late callback. No waits or PCM-path
    // allocation. This task owns fd; its subsequent close is queued after this
    // callback on the same TCP/IP thread, before the descriptor can be reused.
    if (tcpip_try_callback(gateway_tcp_diag_collect, d) != ERR_OK)
        atomic_store_explicit(&d->state, 0, memory_order_release);
}

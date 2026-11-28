// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Mirrored in xiaozhi-realtime. See docs/protocol/gateway-management-v1.md.
enum {
    GATEWAY_PHONE_CMD_ANSWER = 1,
    GATEWAY_PHONE_CMD_REJECT,
    GATEWAY_PHONE_CMD_AUDIO_ON,
    GATEWAY_PHONE_CMD_AUDIO_OFF,
    GATEWAY_PHONE_CMD_AUTO_CANCEL,
    GATEWAY_PHONE_CMD_AUTO_ON,
    GATEWAY_PHONE_CMD_AUTO_OFF,
};
enum { PHONE_IDLE, PHONE_INCOMING, PHONE_OUTGOING, PHONE_ACTIVE, PHONE_ENDED };
enum {
    PHONE_AUTO_ENABLED = 1,
    PHONE_AUTO_PENDING = 2,
    PHONE_AUDIO_SUSPENDED = 4,
    PHONE_CALLER_TRUNCATED = 8
};
enum {
    PHONE_ACCEPTED,
    PHONE_ERROR,
    PHONE_DISCONNECTED,
    PHONE_TIMEOUT,
    PHONE_UNSUPPORTED,
    PHONE_PENDING = 255
};
#define PHONE_CONTROL_VERSION 1
#define PHONE_STATUS_TYPE 7
#define PHONE_COMMAND_TIMEOUT_MS 5000

// The 44-byte payload occupies session..reserved in an authenticated type-7
// packet. Header, nonces, heartbeat sequence and HMAC retain their usual roles;
// the sequence occupies payload bytes 12..15 and is not part of the snapshot.
typedef struct __attribute__((packed)) {
    uint32_t call_id, revision, started_ms, sequence, ended_ms;
    uint8_t phase, setup, active, flags;
    uint16_t remaining_ms;
    uint8_t at_result, caller_length;
    char caller[16];
} phone_status_t;
#ifdef __cplusplus
static_assert(sizeof(phone_status_t) == 44, "phone status payload size");
#else
_Static_assert(sizeof(phone_status_t) == 44, "phone status payload size");
#endif

static inline bool phone_status_valid(const phone_status_t* s) {
    if (s->phase > PHONE_ENDED || s->setup > 3 || s->active > 1 || (s->flags & ~15u) ||
        s->remaining_ms > 3000 || s->caller_length >= sizeof(s->caller) ||
        s->caller[s->caller_length] != 0)
        return false;
    if ((s->active != (s->phase == PHONE_ACTIVE)) || (s->phase != PHONE_IDLE && !s->call_id) ||
        (s->remaining_ms &&
         (!(s->flags & PHONE_AUTO_PENDING) || !(s->flags & PHONE_AUTO_ENABLED) || s->setup != 1)))
        return false;
    for (unsigned i = 0; i < s->caller_length; ++i) {
        char c = s->caller[i];
        if ((c < '0' || c > '9') && c != '+' && c != '*' && c != '#')
            return false;
    }
    return true;
}
static inline bool phone_command_valid(uint8_t command) {
    return command >= GATEWAY_PHONE_CMD_ANSWER && command <= GATEWAY_PHONE_CMD_AUTO_OFF;
}
static inline bool phone_command_scoped(uint8_t command) {
    return command == GATEWAY_PHONE_CMD_ANSWER || command == GATEWAY_PHONE_CMD_REJECT ||
           command == GATEWAY_PHONE_CMD_AUTO_CANCEL;
}
static inline bool phone_request_new(uint16_t previous, uint16_t request) {
    return request && (!previous || (int16_t)(request - previous) > 0);
}

typedef struct {
    uint16_t sequence;
    uint8_t command, last_command, result;
    uint32_t call_id;
    int64_t deadline_ms;
} phone_request_t;

// Caller serializes access. Terminal outcomes retain request identity for logs.
static inline void phone_request_finish(phone_request_t* r, uint8_t result) {
    r->command = 0;
    r->result = result;
    r->deadline_ms = 0;
}
static inline bool phone_request_queue(phone_request_t* r, uint8_t command, uint32_t call_id,
                                       int64_t now_ms) {
    if (!phone_command_valid(command) || r->command)
        return false;
    if (!++r->sequence)
        ++r->sequence;
    r->command = r->last_command = command;
    r->call_id = call_id;
    r->result = PHONE_PENDING;
    r->deadline_ms = now_ms + PHONE_COMMAND_TIMEOUT_MS;
    return true;
}
static inline bool phone_request_ack(phone_request_t* r, uint8_t command, uint16_t sequence,
                                     uint8_t result) {
    if (!r->command || command != r->command || sequence != r->sequence || result > PHONE_ERROR)
        return false;
    phone_request_finish(r, result);
    return true;
}
static inline bool phone_request_expire(phone_request_t* r, int64_t now_ms) {
    if (!r->command || now_ms < r->deadline_ms)
        return false;
    phone_request_finish(r, PHONE_TIMEOUT);
    return true;
}

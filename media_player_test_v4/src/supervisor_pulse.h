#ifndef SUPERVISOR_PULSE_H
#define SUPERVISOR_PULSE_H

#include <stdint.h>
#include <time.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>

#define SUP_CHANNEL_NAME      "qnx_supervisor"
#define PULSE_CODE_HEARTBEAT  (_PULSE_CODE_MINAVAIL + 1)

typedef enum {
    PROC_LCD = 0,
    PROC_SENSOR,
    PROC_MEDIA,
    PROC_COUNT
} proc_id_t;

typedef struct {
    int       coid;
    proc_id_t id;
    int64_t   last_beat_ms;
    int64_t   last_retry_ms;
} sup_client_t;

static inline int64_t sup_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000L);
}

static inline void sup_client_init(sup_client_t *c, proc_id_t id) {
    c->id            = id;
    c->coid          = name_open(SUP_CHANNEL_NAME, 0);
    c->last_beat_ms  = 0;
    c->last_retry_ms = 0;
}

/* Rate-limited (every 500ms) non-blocking pulse with auto-reconnect */
static inline void sup_beat(sup_client_t *c) {
    int64_t now = sup_monotonic_ms();
    if (now - c->last_beat_ms < 500) return;

    if (c->coid < 0) {
        if (now - c->last_retry_ms < 500) return;
        c->last_retry_ms = now;
        c->coid = name_open(SUP_CHANNEL_NAME, 0);
        if (c->coid < 0) return;
    }

    if (MsgSendPulse(c->coid, -1, PULSE_CODE_HEARTBEAT, (int)c->id) == -1) {
        /* Supervisor likely restarted; drop stale connection and retry next tick */
        name_close(c->coid);
        c->coid = -1;
    } else {
        c->last_beat_ms = now;
    }
}

static inline void sup_client_close(sup_client_t *c) {
    if (c->coid >= 0) {
        name_close(c->coid);
        c->coid = -1;
    }
}

#endif /* SUPERVISOR_PULSE_H */

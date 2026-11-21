#ifndef IPC_MSG_H
#define IPC_MSG_H

#include <stdint.h>
#include <sys/neutrino.h>
#include <sys/iomsg.h>

#define LCD_CHANNEL_NAME  "lcd_dashboard_srv"
#define MSG_TYPE_UPDATE   (_IO_MAX + 1)

/* Bitmask indicating which fields changed */
#define FLAG_SPEED  (1U << 0)
#define FLAG_H1     (1U << 1)
#define FLAG_H2     (1U << 2)
#define FLAG_H3     (1U << 3)

typedef struct {
    uint16_t type;        /* MSG_TYPE_UPDATE */
    uint16_t flags;       /* Bitmask of FLAG_* */
    int32_t  speed;       /* 0 - 240 KM/H */
    int32_t  h1;          /* Hazard 1 state */
    int32_t  h2;          /* Hazard 2 state */
    int32_t  h3;          /* Hazard 3 state */
    uint64_t timestamp;   /* ClockCycles() at sensor capture */
} sensor_msg_t;

typedef union {
    struct _pulse pulse;
    sensor_msg_t  sensor;
} recv_msg_t;

#endif /* IPC_MSG_H */

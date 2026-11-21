#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include "supervisor_pulse.h"
#include "ipc_msg.h"

/* ---------------- Hardware Configuration ---------------- */
#define TARGET_CORE       3   /* Off Core 3 so buzzer busy-wait doesn't starve polling */
#define UART_DEV_PATH     "/dev/ser1"

/* Raspberry Pi 5 (RP1) Physical Base Addresses */
#define RP1_IO_BANK0_BASE   0x1F000D0000UL
#define RP1_SYS_RIO0_BASE   0x1F000E0000UL
#define RP1_PADS_BANK0_BASE 0x1F000F0000UL
#define RP1_BLOCK_SIZE      0x4000

#define RP1_CLR_OFFSET      (0x3000 / 4)
#define RIO_OE_REG          1
#define RIO_IN_REG          2

/* ESP32 Hazard Input Pins */
#define PIN_HAZARD_1  17
#define PIN_HAZARD_2  27
#define PIN_HAZARD_3  22

static int pin_to_core(int core) {
    unsigned runmask = (1U << core);
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)runmask) == -1) {
        perror("ThreadCtl(_NTO_TCTL_RUNMASK)");
        return -1;
    }
    return 0;
}

/* ---------------- RP1 GPIO Input Helpers ---------------- */
static volatile uint32_t *rp1_io   = NULL;
static volatile uint32_t *rp1_rio  = NULL;
static volatile uint32_t *rp1_pads = NULL;

static int gpio_init(void) {
    if (ThreadCtl(_NTO_TCTL_IO, 0) == -1) {
        perror("ThreadCtl(_NTO_TCTL_IO)");
        return -1;
    }
    rp1_io = mmap_device_memory(NULL, RP1_BLOCK_SIZE,
                                PROT_READ | PROT_WRITE | PROT_NOCACHE, 0, RP1_IO_BANK0_BASE);
    rp1_rio = mmap_device_memory(NULL, RP1_BLOCK_SIZE,
                                 PROT_READ | PROT_WRITE | PROT_NOCACHE, 0, RP1_SYS_RIO0_BASE);
    rp1_pads = mmap_device_memory(NULL, RP1_BLOCK_SIZE,
                                  PROT_READ | PROT_WRITE | PROT_NOCACHE, 0, RP1_PADS_BANK0_BASE);

    if (rp1_io == MAP_FAILED || rp1_rio == MAP_FAILED || rp1_pads == MAP_FAILED) {
        perror("mmap_device_memory RP1 GPIO");
        return -1;
    }
    return 0;
}

static void gpio_set_input(int pin) {
    rp1_io[(pin * 2) + 1] = 5;
    rp1_pads[1 + pin] = 0xC6; /* OD + Input Enable + Pull-Down + Schmitt Trigger */
    rp1_rio[RIO_OE_REG + RP1_CLR_OFFSET] = (1U << pin);
}

static inline int gpio_read(int pin) {
    return (rp1_rio[RIO_IN_REG] >> pin) & 1;
}

static void gpio_cleanup(void) {
    if (rp1_io   && rp1_io   != MAP_FAILED) munmap_device_memory((void *)rp1_io,   RP1_BLOCK_SIZE);
    if (rp1_rio  && rp1_rio  != MAP_FAILED) munmap_device_memory((void *)rp1_rio,  RP1_BLOCK_SIZE);
    if (rp1_pads && rp1_pads != MAP_FAILED) munmap_device_memory((void *)rp1_pads, RP1_BLOCK_SIZE);
}

/* ---------------- QNX UART Helpers ---------------- */
static int uart_fd = -1;

static int uart_init(void) {
    uart_fd = open(UART_DEV_PATH, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (uart_fd < 0) {
        perror("UART not available (continuing with GPIOs only)");
        return -1;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(uart_fd, &tty) != 0) {
        perror("tcgetattr");
        close(uart_fd);
        uart_fd = -1;
        return -1;
    }

    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | CSTOPB | IHFLOW | OHFLOW);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    tcflush(uart_fd, TCIFLUSH);
    if (tcsetattr(uart_fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        close(uart_fd);
        uart_fd = -1;
        return -1;
    }

    printf("[SENSOR] UART opened on %s\n", UART_DEV_PATH);
    return 0;
}

static int uart_poll_speed(int *speed_out) {
    if (uart_fd < 0) return 0;

    static char line[16];
    static int pos = 0;
    static int in_frame = 0;
    char tmp[64];
    int updated = 0;

    ssize_t n;
    while ((n = read(uart_fd, tmp, sizeof(tmp))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char ch = tmp[i];
            if (ch == 'S') {
                pos = 0;
                in_frame = 1;
            } else if ((ch == '\n' || ch == '\r') && in_frame) {
                if (pos > 0) {
                    line[pos] = '\0';
                    int val = atoi(line);
                    if (val < 0) val = 0;
                    if (val > 240) val = 240;
                    *speed_out = val;
                    updated = 1;
                }
                pos = 0;
                in_frame = 0;
            } else if (in_frame && ch >= '0' && ch <= '9' && pos < (int)sizeof(line) - 1) {
                line[pos++] = ch;
            } else {
                pos = 0;
                in_frame = 0;
            }
        }
    }
    return updated;
}

/* ---------------- Main Client Loop ---------------- */
int main(void) {
    if (pin_to_core(TARGET_CORE) != 0) return EXIT_FAILURE;

    sup_client_t hb;
    sup_client_init(&hb, PROC_SENSOR);
    sup_beat(&hb);

    if (gpio_init() != 0) {
        sup_client_close(&hb);
        return EXIT_FAILURE;
    }
    uart_init();

    gpio_set_input(PIN_HAZARD_1);
    gpio_set_input(PIN_HAZARD_2);
    gpio_set_input(PIN_HAZARD_3);

    int server_coid = -1;
    while ((server_coid = name_open(LCD_CHANNEL_NAME, 0)) == -1) {
        sup_beat(&hb);
        printf("[SENSOR] Waiting for %s...\n", LCD_CHANNEL_NAME);
        usleep(200000);
    }
    printf("[SENSOR] Connected to %s (coid=%d)\n", LCD_CHANNEL_NAME, server_coid);

    int speed = 0, last_speed = -1;
    int last_h1 = -1, last_h2 = -1, last_h3 = -1;
    int sync_ticks = 0;

    while (1) {
        sup_beat(&hb);

        uint16_t flags = 0;
        uint64_t t_capture = ClockCycles();

        if (uart_poll_speed(&speed) && speed != last_speed) {
            flags |= FLAG_SPEED;
            last_speed = speed;
        }

        int h1 = gpio_read(PIN_HAZARD_1);
        int h2 = gpio_read(PIN_HAZARD_2);
        int h3 = gpio_read(PIN_HAZARD_3);

        if (h1 != last_h1) { flags |= FLAG_H1; last_h1 = h1; }
        if (h2 != last_h2) { flags |= FLAG_H2; last_h2 = h2; }
        if (h3 != last_h3) { flags |= FLAG_H3; last_h3 = h3; }

        int changed = (flags != 0);

        /* Periodic 500ms sync (100 * 5ms) so cleared icons self-heal if SPI glitches */
        if (++sync_ticks >= 100) {
            sync_ticks = 0;
            flags |= (FLAG_H1 | FLAG_H2 | FLAG_H3);
        }

        if (flags != 0) {
            if (changed) {
                printf("[SENSOR] Change: flags=0x%X spd=%d h=%d%d%d\n", flags, speed, h1, h2, h3);
                fflush(stdout);
            }

            sensor_msg_t msg = {
                .type      = MSG_TYPE_UPDATE,
                .flags     = flags,
                .speed     = speed,
                .h1        = h1,
                .h2        = h2,
                .h3        = h3,
                .timestamp = t_capture
            };

            if (MsgSend(server_coid, &msg, sizeof(msg), NULL, 0) == -1) {
                perror("[SENSOR] MsgSend failed");
                name_close(server_coid);
                while ((server_coid = name_open(LCD_CHANNEL_NAME, 0)) == -1) {
                    sup_beat(&hb);
                    usleep(200000);
                }
                /* Force full resend after reconnect */
                last_speed = last_h1 = last_h2 = last_h3 = -1;
            }
        }

        usleep(5000); /* 5 ms poll loop */
    }

    sup_client_close(&hb);
    name_close(server_coid);
    if (uart_fd >= 0) close(uart_fd);
    gpio_cleanup();
    return EXIT_SUCCESS;
}

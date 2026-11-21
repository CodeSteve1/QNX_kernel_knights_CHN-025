#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <devctl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/syspage.h>
#include <hw/io-spi.h>
#include "supervisor_pulse.h"
#include "ipc_msg.h"

/* ---------------- Hardware Configuration ---------------- */
#define TARGET_CORE       3
#define SPI_DEV_PATH      "/dev/io-spi/spi0/dev0"
#define SPI_CLOCK_HZ      12000000
#define BUZZER_IS_ACTIVE  0

/* Raspberry Pi 5 (RP1) Physical Base Addresses */
#define RP1_IO_BANK0_BASE   0x1F000D0000UL
#define RP1_SYS_RIO0_BASE   0x1F000E0000UL
#define RP1_PADS_BANK0_BASE 0x1F000F0000UL
#define RP1_BLOCK_SIZE      0x4000

#define RP1_SET_OFFSET      (0x2000 / 4)
#define RP1_CLR_OFFSET      (0x3000 / 4)
#define RIO_OUT_REG         0
#define RIO_OE_REG          1

/* Output Pins */
#define PIN_DC      26
#define PIN_RST     19
#define PIN_CS      13
#define PIN_BUZZER  18

#define LCD_WIDTH   240
#define LCD_HEIGHT  320

/* RGB565 Colors */
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_GREEN   0x07E0
#define COLOR_CYAN    0x07FF
#define COLOR_RED     0xF800
#define COLOR_YELLOW  0xFFE0
#define COLOR_ORANGE  0xFD20

/* ---------------- CPU Affinity & SysTick ---------------- */
static int pin_to_core(int core) {
    unsigned runmask = (1U << core);
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)runmask) == -1) {
        perror("ThreadCtl(_NTO_TCTL_RUNMASK)");
        return -1;
    }
    return 0;
}

static double cycles_per_ms = 1.0;
static double cycles_per_us = 1.0;

static void systick_init(void) {
    uint64_t cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    cycles_per_ms = (double)cps / 1000.0;
    cycles_per_us = (double)cps / 1000000.0;
}

static inline uint64_t systick_now(void) {
    return ClockCycles();
}

static inline double systick_elapsed_ms(uint64_t start, uint64_t end) {
    return (double)(end - start) / cycles_per_ms;
}

static inline void systick_delay_us(uint32_t us) {
    uint64_t start = ClockCycles();
    uint64_t target = (uint64_t)((double)us * cycles_per_us);
    while ((ClockCycles() - start) < target) {}
}

/* ---------------- RP1 GPIO Output Helpers ---------------- */
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

static void gpio_set_output(int pin) {
    rp1_io[(pin * 2) + 1] = 5;
    rp1_pads[1 + pin] = 0x30;
    rp1_rio[RIO_OE_REG + RP1_SET_OFFSET] = (1U << pin);
}

static inline void gpio_write(int pin, int val) {
    if (val) rp1_rio[RIO_OUT_REG + RP1_SET_OFFSET] = (1U << pin);
    else     rp1_rio[RIO_OUT_REG + RP1_CLR_OFFSET] = (1U << pin);
}

static void gpio_cleanup(void) {
    if (rp1_io   && rp1_io   != MAP_FAILED) munmap_device_memory((void *)rp1_io,   RP1_BLOCK_SIZE);
    if (rp1_rio  && rp1_rio  != MAP_FAILED) munmap_device_memory((void *)rp1_rio,  RP1_BLOCK_SIZE);
    if (rp1_pads && rp1_pads != MAP_FAILED) munmap_device_memory((void *)rp1_pads, RP1_BLOCK_SIZE);
}

/* ---------------- Buzzer Thread ---------------- */
static volatile int g_h1 = 0, g_h2 = 0, g_h3 = 0, g_speed = 0;
static volatile int g_running = 1;

static void buzzer_tone(int freq_hz, int duration_ms) {
    if (duration_ms <= 0) return;
#if BUZZER_IS_ACTIVE
    (void)freq_hz;
    gpio_write(PIN_BUZZER, 1);
    usleep((useconds_t)duration_ms * 1000);
    gpio_write(PIN_BUZZER, 0);
#else
    if (freq_hz <= 0) return;
    uint32_t half_period_us = (uint32_t)(500000 / freq_hz);
    uint64_t start = systick_now();
    while (g_running && systick_elapsed_ms(start, systick_now()) < (double)duration_ms) {
        gpio_write(PIN_BUZZER, 1);
        systick_delay_us(half_period_us);
        gpio_write(PIN_BUZZER, 0);
        systick_delay_us(half_period_us);
    }
    gpio_write(PIN_BUZZER, 0);
#endif
}

static void *buzzer_thread_fn(void *arg) {
    (void)arg;
    if (pin_to_core(TARGET_CORE) != 0) return NULL;
    if (ThreadCtl(_NTO_TCTL_IO, 0) == -1) {
        perror("Buzzer ThreadCtl");
        return NULL;
    }

    uint64_t last_speed_beep = 0;
    int was_overspeed = 0;

    while (g_running) {
        int h1 = g_h1, h2 = g_h2, h3 = g_h3, spd = g_speed;
        uint64_t now = systick_now();

        if (spd >= 100) {
            if (!was_overspeed || systick_elapsed_ms(last_speed_beep, now) >= 10000.0) {
                buzzer_tone(1200, 90);  usleep(30000);
                buzzer_tone(1600, 90);  usleep(30000);
                buzzer_tone(2200, 140);
                last_speed_beep = systick_now();
                was_overspeed = 1;
                usleep(150000);
            }
        } else {
            was_overspeed = 0;
        }

        if (h2) {
            buzzer_tone(2800, 110); buzzer_tone(2000, 110);
            buzzer_tone(2800, 110); buzzer_tone(2000, 110);
            usleep(350000);
        } else if (h1) {
            buzzer_tone(1400, 120); usleep(60000);
            buzzer_tone(1000, 180); usleep(700000);
        } else if (h3) {
            for (int i = 0; i < 3 && g_running && g_h3; i++) {
                buzzer_tone(650, 80); usleep(80000);
            }
            usleep(700000);
        } else {
            usleep(50000);
        }
    }
    gpio_write(PIN_BUZZER, 0);
    return NULL;
}

/* ---------------- SPI & ILI9341 Driver ---------------- */
static int spi_fd = -1;

static int spi_init(void) {
    spi_fd = open(SPI_DEV_PATH, O_RDWR);
    if (spi_fd < 0) {
        perror("open SPI device");
        return -1;
    }
    spi_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = SPI_MODE_WORD_WIDTH_8 | SPI_MODE_BODER_MSB;
    cfg.clock_rate = SPI_CLOCK_HZ;
    if (devctl(spi_fd, DCMD_SPI_SET_CONFIG, &cfg, sizeof(cfg), NULL) != EOK) {
        perror("DCMD_SPI_SET_CONFIG");
        close(spi_fd);
        return -1;
    }
    return 0;
}

static void ili9341_write_cmd(uint8_t cmd) {
    gpio_write(PIN_CS, 0); gpio_write(PIN_DC, 0);
    write(spi_fd, &cmd, 1);
    gpio_write(PIN_CS, 1);
}

static void ili9341_write_data(const uint8_t *data, size_t len) {
    gpio_write(PIN_CS, 0); gpio_write(PIN_DC, 1);
    const size_t chunk = 4096;
    size_t sent = 0;
    while (sent < len) {
        size_t n = (len - sent > chunk) ? chunk : (len - sent);
        write(spi_fd, data + sent, n);
        sent += n;
    }
    gpio_write(PIN_CS, 1);
}

static void ili9341_write_byte(uint8_t val) { ili9341_write_data(&val, 1); }

static void ili9341_init(void) {
    gpio_write(PIN_RST, 1); usleep(5000);
    gpio_write(PIN_RST, 0); usleep(20000);
    gpio_write(PIN_RST, 1); usleep(150000);

    ili9341_write_cmd(0x01); usleep(150000);
    ili9341_write_cmd(0x28);

    uint8_t pca[]   = {0x39, 0x2C, 0x00, 0x34, 0x02};
    uint8_t pcb[]   = {0x00, 0xC1, 0x30};
    uint8_t dta[]   = {0x85, 0x00, 0x78};
    uint8_t dtb[]   = {0x00, 0x00};
    uint8_t pos[]   = {0x64, 0x03, 0x12, 0x81};
    uint8_t vcom1[] = {0x3E, 0x28};
    uint8_t frc[]   = {0x00, 0x18};
    uint8_t dfc[]   = {0x08, 0x82, 0x27};

    ili9341_write_cmd(0xCB); ili9341_write_data(pca, sizeof(pca));
    ili9341_write_cmd(0xCF); ili9341_write_data(pcb, sizeof(pcb));
    ili9341_write_cmd(0xE8); ili9341_write_data(dta, sizeof(dta));
    ili9341_write_cmd(0xEA); ili9341_write_data(dtb, sizeof(dtb));
    ili9341_write_cmd(0xED); ili9341_write_data(pos, sizeof(pos));
    ili9341_write_cmd(0xF7); ili9341_write_byte(0x20);
    ili9341_write_cmd(0xC0); ili9341_write_byte(0x23);
    ili9341_write_cmd(0xC1); ili9341_write_byte(0x10);
    ili9341_write_cmd(0xC5); ili9341_write_data(vcom1, sizeof(vcom1));
    ili9341_write_cmd(0xC7); ili9341_write_byte(0x86);
    ili9341_write_cmd(0x36); ili9341_write_byte(0x28);
    ili9341_write_cmd(0x3A); ili9341_write_byte(0x55);
    ili9341_write_cmd(0xB1); ili9341_write_data(frc, sizeof(frc));
    ili9341_write_cmd(0xB6); ili9341_write_data(dfc, sizeof(dfc));

    ili9341_write_cmd(0x11); usleep(120000);
    ili9341_write_cmd(0x29); usleep(20000);
}

static void ili9341_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t col[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t row[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    ili9341_write_cmd(0x2A); ili9341_write_data(col, 4);
    ili9341_write_cmd(0x2B); ili9341_write_data(row, 4);
    ili9341_write_cmd(0x2C);
}

static void ili9341_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;

    ili9341_set_window(x, y, x + w - 1, y + h - 1);
    uint8_t hi = color >> 8, lo = color & 0xFF;
    uint8_t buf[1024];
    for (int i = 0; i < 1024; i += 2) { buf[i] = hi; buf[i + 1] = lo; }

    size_t total = (size_t)w * h * 2;
    gpio_write(PIN_CS, 0); gpio_write(PIN_DC, 1);
    while (total > 0) {
        size_t n = (total > sizeof(buf)) ? sizeof(buf) : total;
        write(spi_fd, buf, n);
        total -= n;
    }
    gpio_write(PIN_CS, 1);
}

static void ili9341_draw_bitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels) {
    size_t total_pixels = (size_t)w * h;
    uint8_t *buf = malloc(total_pixels * 2);
    if (!buf) return;
    for (size_t i = 0; i < total_pixels; i++) {
        buf[i * 2]     = pixels[i] >> 8;
        buf[i * 2 + 1] = pixels[i] & 0xFF;
    }
    ili9341_set_window(x, y, x + w - 1, y + h - 1);
    ili9341_write_data(buf, total_pixels * 2);
    free(buf);
}

/* ---------------- Icons & 5x7 Font ---------------- */
static uint16_t icon_hazard1[30 * 30];
static uint16_t icon_hazard2[30 * 30];
static uint16_t icon_hazard3[30 * 30];

static void build_placeholder_icons(void) {
    for (int y = 0; y < 30; y++) {
        for (int x = 0; x < 30; x++) {
            int idx = y * 30 + x;
            int half_w = (y * 14) / 28;
            if (y >= 2 && y <= 27 && abs(x - 15) <= half_w) {
                int is_bang = (x >= 14 && x <= 15) && ((y >= 10 && y <= 19) || (y >= 22 && y <= 24));
                icon_hazard1[idx] = is_bang ? COLOR_BLACK : COLOR_YELLOW;
            } else {
                icon_hazard1[idx] = COLOR_BLACK;
            }

            int dx = x - 15, dy = y - 15;
            if (dx * dx + dy * dy <= 13 * 13) {
                int is_bang = (x >= 14 && x <= 15) && ((y >= 7 && y <= 17) || (y >= 20 && y <= 22));
                icon_hazard2[idx] = is_bang ? COLOR_WHITE : COLOR_RED;
            } else {
                icon_hazard2[idx] = COLOR_BLACK;
            }

            if (x >= 2 && x <= 27 && y >= 2 && y <= 27) {
                int is_x = (abs(x - y) <= 2 || abs((29 - x) - y) <= 2) && (x >= 7 && x <= 22);
                icon_hazard3[idx] = is_x ? COLOR_BLACK : COLOR_ORANGE;
            } else {
                icon_hazard3[idx] = COLOR_BLACK;
            }
        }
    }
}

static const uint8_t* get_glyph(char c) {
    static const uint8_t g_space[5] = {0x00,0x00,0x00,0x00,0x00};
    static const uint8_t g_0[5]     = {0x3E,0x51,0x49,0x45,0x3E};
    static const uint8_t g_1[5]     = {0x00,0x42,0x7F,0x40,0x00};
    static const uint8_t g_2[5]     = {0x42,0x61,0x51,0x49,0x46};
    static const uint8_t g_3[5]     = {0x21,0x41,0x45,0x4B,0x31};
    static const uint8_t g_4[5]     = {0x18,0x14,0x12,0x7F,0x10};
    static const uint8_t g_5[5]     = {0x27,0x45,0x45,0x45,0x39};
    static const uint8_t g_6[5]     = {0x3C,0x4A,0x49,0x49,0x30};
    static const uint8_t g_7[5]     = {0x01,0x71,0x09,0x05,0x03};
    static const uint8_t g_8[5]     = {0x36,0x49,0x49,0x49,0x36};
    static const uint8_t g_9[5]     = {0x06,0x49,0x49,0x29,0x1E};
    static const uint8_t g_A[5]     = {0x7E,0x11,0x11,0x11,0x7E};
    static const uint8_t g_D[5]     = {0x7F,0x41,0x41,0x22,0x1C};
    static const uint8_t g_E[5]     = {0x7F,0x49,0x49,0x49,0x41};
    static const uint8_t g_H[5]     = {0x7F,0x08,0x08,0x08,0x7F};
    static const uint8_t g_K[5]     = {0x7F,0x08,0x14,0x22,0x41};
    static const uint8_t g_L[5]     = {0x7F,0x40,0x40,0x40,0x40};
    static const uint8_t g_M[5]     = {0x7F,0x02,0x0C,0x02,0x7F};
    static const uint8_t g_P[5]     = {0x7F,0x09,0x09,0x09,0x06};
    static const uint8_t g_S[5]     = {0x46,0x49,0x49,0x49,0x31};
    static const uint8_t g_T[5]     = {0x01,0x01,0x7F,0x01,0x01};
    static const uint8_t g_slash[5] = {0x20,0x10,0x08,0x04,0x02};
    static const uint8_t g_colon[5] = {0x00,0x36,0x36,0x00,0x00};
    static const uint8_t g_dot[5]   = {0x00,0x60,0x60,0x00,0x00};

    switch (c) {
        case '0': return g_0; case '1': return g_1; case '2': return g_2;
        case '3': return g_3; case '4': return g_4; case '5': return g_5;
        case '6': return g_6; case '7': return g_7; case '8': return g_8;
        case '9': return g_9; case 'A': return g_A; case 'D': return g_D;
        case 'E': return g_E; case 'H': return g_H; case 'K': return g_K;
        case 'L': return g_L; case 'M': return g_M; case 'P': return g_P;
        case 'S': return g_S; case 'T': return g_T; case '/': return g_slash;
        case ':': return g_colon; case '.': return g_dot;
        default:  return g_space;
    }
}

static void ili9341_draw_char(uint16_t x, uint16_t y, char c,
                              uint16_t fg, uint16_t bg, uint8_t scale) {
    const uint8_t *glyph = get_glyph(c);
    uint16_t char_w = 6 * scale, char_h = 8 * scale;
    size_t buf_size = (size_t)char_w * char_h * 2;
    uint8_t *buf = malloc(buf_size);
    if (!buf) return;

    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 6; col++) {
            uint16_t color = (col < 5 && row < 7 && (glyph[col] & (1 << row))) ? fg : bg;
            uint8_t hi = color >> 8, lo = color & 0xFF;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    size_t idx = ((size_t)(row * scale + sy) * char_w + (col * scale + sx)) * 2;
                    buf[idx] = hi; buf[idx + 1] = lo;
                }
            }
        }
    }
    ili9341_set_window(x, y, x + char_w - 1, y + char_h - 1);
    ili9341_write_data(buf, buf_size);
    free(buf);
}

static void ili9341_draw_string(uint16_t x, uint16_t y, const char *str,
                                uint16_t fg, uint16_t bg, uint8_t scale) {
    while (*str) {
        ili9341_draw_char(x, y, *str++, fg, bg, scale);
        x += 6 * scale;
    }
}

static void draw_latency(double ms) {
    char lat_buf[20];
    snprintf(lat_buf, sizeof(lat_buf), "LAT:%6.2f MS", ms);
    ili9341_draw_string(82, 215, lat_buf, COLOR_YELLOW, COLOR_BLACK, 2);
}

/* ---------------- Main Server Loop ---------------- */
int main(void) {
    if (pin_to_core(TARGET_CORE) != 0) return EXIT_FAILURE;
    systick_init();

    sup_client_t hb;
    sup_client_init(&hb, PROC_LCD);
    sup_beat(&hb);

    if (gpio_init() != 0 || spi_init() != 0) {
        sup_client_close(&hb);
        gpio_cleanup();
        return EXIT_FAILURE;
    }

    /* Register QNX IPC channel */
    name_attach_t *attach = name_attach(NULL, LCD_CHANNEL_NAME, 0);
    if (!attach) {
        perror("name_attach");
        sup_client_close(&hb);
        close(spi_fd);
        gpio_cleanup();
        return EXIT_FAILURE;
    }
    printf("[LCD] Server attached to /dev/name/local/%s\n", LCD_CHANNEL_NAME);

    gpio_set_output(PIN_DC);
    gpio_set_output(PIN_RST);
    gpio_set_output(PIN_CS);
    gpio_set_output(PIN_BUZZER);
    gpio_write(PIN_CS, 1);
    gpio_write(PIN_BUZZER, 0);

    pthread_t buzzer_tid;
    pthread_create(&buzzer_tid, NULL, buzzer_thread_fn, NULL);

    build_placeholder_icons();
    ili9341_init();
    ili9341_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_BLACK);

    ili9341_draw_string(95, 6, "SPEED", COLOR_CYAN, COLOR_BLACK, 4);
    ili9341_draw_string(124, 138, "KM/H", COLOR_WHITE, COLOR_BLACK, 3);
    draw_latency(0.0);

    recv_msg_t msg;
    char buf[8];

    while (1) {
        sup_beat(&hb);

        uint64_t timeout_ns = 1000000000ULL; /* 1 second */
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &timeout_ns, NULL);

        int rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1) {
            if (errno == ETIMEDOUT) {
                continue; /* Wake up to refresh heartbeat */
            }
            perror("MsgReceive");
            break;
        }

        /* Handle QNX system pulses (e.g., client disconnect) */
        if (rcvid == 0) {
            if (msg.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(msg.pulse.scoid);
            }
            continue;
        }

        if (msg.sensor.type != MSG_TYPE_UPDATE) {
            MsgError(rcvid, ENOSYS);
            continue;
        }

        /* Copy payload and reply immediately so sensor_proc isn't blocked during SPI I/O */
        sensor_msg_t data = msg.sensor;
        MsgReply(rcvid, EOK, NULL, 0);

        if (data.flags & FLAG_SPEED) {
            uint16_t color = (data.speed >= 100) ? COLOR_RED : COLOR_GREEN;
            snprintf(buf, sizeof(buf), "%3d", data.speed);
            ili9341_draw_string(52, 48, buf, color, COLOR_BLACK, 10);
            g_speed = data.speed;
        }
        if (data.flags & FLAG_H1) {
            if (data.h1) ili9341_draw_bitmap(75, 172, 30, 30, icon_hazard1);
            else         ili9341_fill_rect  (75, 172, 30, 30, COLOR_BLACK);
            g_h1 = data.h1;
        }
        if (data.flags & FLAG_H2) {
            if (data.h2) ili9341_draw_bitmap(145, 172, 30, 30, icon_hazard2);
            else         ili9341_fill_rect  (145, 172, 30, 30, COLOR_BLACK);
            g_h2 = data.h2;
        }
        if (data.flags & FLAG_H3) {
            if (data.h3) ili9341_draw_bitmap(215, 172, 30, 30, icon_hazard3);
            else         ili9341_fill_rect  (215, 172, 30, 30, COLOR_BLACK);
            g_h3 = data.h3;
        }

        double lat_ms = systick_elapsed_ms(data.timestamp, systick_now());
        draw_latency(lat_ms);
        printf("[LCD] Updated (flags=0x%X, spd=%d, h=%d%d%d) | E2E Latency: %.3f ms\n",
               data.flags, data.speed, data.h1, data.h2, data.h3, lat_ms);
    }

    g_running = 0;
    pthread_join(buzzer_tid, NULL);
    sup_client_close(&hb);
    name_detach(attach, 0);
    close(spi_fd);
    gpio_cleanup();
    return EXIT_SUCCESS;
}

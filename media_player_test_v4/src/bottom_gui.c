// src/bottom_gui.c
#include "bottom_gui.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cache of what is currently drawn in the bar texture. The status line carries
// page, instance count, off-page count, FPS, drops and CPU, so comparing it
// (plus the red/green colour choice) is enough to know if anything changed.
static char g_last_status[256];
static bool g_last_red = false;
static bool g_have_last = false;

// Minimal 5x7 font for digits, uppercase letters, and UI punctuation
static void get_glyph_5x7(char c, uint8_t rows[7]) {
    memset(rows, 0, 7);
    c = (char)toupper((unsigned char)c);
    static const uint8_t digits[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
        {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
        {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
        {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
        {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
        {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
    };
    if (c >= '0' && c <= '9') { memcpy(rows, digits[c - '0'], 7); return; }

    static const uint8_t letters[26][7] = {
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // A
        {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
        {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
        {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, // D
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F
        {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // G
        {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // H
        {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // I
        {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // J
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
        {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // M
        {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, // N
        {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
        {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P
        {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // Q
        {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // R
        {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // S
        {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // T
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // U
        {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // V
        {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, // W
        {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X
        {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // Y
        {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // Z
    };
    if (c >= 'A' && c <= 'Z') { memcpy(rows, letters[c - 'A'], 7); return; }

    switch (c) {
        case ':': { uint8_t g[7]={0x00,0x04,0x04,0x00,0x04,0x04,0x00}; memcpy(rows,g,7); break; }
        case '.': { uint8_t g[7]={0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}; memcpy(rows,g,7); break; }
        case '/': { uint8_t g[7]={0x01,0x01,0x02,0x04,0x08,0x10,0x10}; memcpy(rows,g,7); break; }
        case '-': { uint8_t g[7]={0x00,0x00,0x00,0x1F,0x00,0x00,0x00}; memcpy(rows,g,7); break; }
        case '_': { uint8_t g[7]={0x00,0x00,0x00,0x00,0x00,0x00,0x1F}; memcpy(rows,g,7); break; }
        case '|': { uint8_t g[7]={0x04,0x04,0x04,0x04,0x04,0x04,0x04}; memcpy(rows,g,7); break; }
        case '<': { uint8_t g[7]={0x02,0x04,0x08,0x10,0x08,0x04,0x02}; memcpy(rows,g,7); break; }
        case '>': { uint8_t g[7]={0x08,0x04,0x02,0x01,0x02,0x04,0x08}; memcpy(rows,g,7); break; }
        case '%': { uint8_t g[7]={0x19,0x19,0x02,0x04,0x08,0x13,0x13}; memcpy(rows,g,7); break; }
        default: break;
    }
}

// Clips once, then writes one 32-bit pixel at a time (bytes land as R,G,B,A in memory).
static void fill_rect(BottomGui *g, int x, int y, int w, int h, uint32_t rgba) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w > g->width)  ? g->width  : x + w;
    int y1 = (y + h > g->height) ? g->height : y + h;
    if (x0 >= x1 || y0 >= y1) return;

    uint8_t px[4] = {
        (uint8_t)((rgba >> 24) & 0xFF), (uint8_t)((rgba >> 16) & 0xFF),
        (uint8_t)((rgba >> 8) & 0xFF),  (uint8_t)(rgba & 0xFF)
    };
    uint32_t pixel;
    memcpy(&pixel, px, 4);

    for (int yy = y0; yy < y1; ++yy) {
        uint32_t *row = (uint32_t *)(g->pixels + ((size_t)yy * (size_t)g->width) * 4);
        for (int xx = x0; xx < x1; ++xx) {
            row[xx] = pixel;
        }
    }
}

static void draw_border(BottomGui *g, int x, int y, int w, int h, int t, uint32_t rgba) {
    fill_rect(g, x, y, w, t, rgba);
    fill_rect(g, x, y + h - t, w, t, rgba);
    fill_rect(g, x, y, t, h, rgba);
    fill_rect(g, x + w - t, y, t, h, rgba);
}

static void draw_text(BottomGui *g, int x, int y, int scale, const char *s, uint32_t rgba) {
    int cx = x;
    for (; *s; ++s) {
        uint8_t rows[7];
        get_glyph_5x7(*s, rows);
        for (int ry = 0; ry < 7; ++ry) {
            for (int rx = 0; rx < 5; ++rx) {
                if (rows[ry] & (1 << (4 - rx))) {
                    fill_rect(g, cx + rx * scale, y + ry * scale, scale, scale, rgba);
                }
            }
        }
        cx += 6 * scale;
    }
}

static bool pt_in_rect(int px, int py, GuiRect r) {
    return px >= r.x && px < (r.x + r.w) && py >= r.y && py < (r.y + r.h);
}

bool bottom_gui_init(BottomGui *gui, int screen_w, int screen_h, int bar_h) {
    memset(gui, 0, sizeof(*gui));
    g_have_last = false;   // force a first draw
    gui->width = screen_w;
    gui->height = bar_h;
    gui->screen_h = screen_h;
    gui->pixels = (uint8_t *)calloc((size_t)screen_w * (size_t)bar_h * 4, 1);
    if (!gui->pixels) return false;

    const int presets[GUI_PRESET_COUNT] = {1, 4, 6, 9, 10, 20, 25, 35, 45, 55,85};
    memcpy(gui->presets, presets, sizeof(presets));

    // Layout 11 checkboxes along Row 1 (wide enough for 3-digit "100")
    int start_x = 220;
    for (int i = 0; i < GUI_PRESET_COUNT; ++i) {
        gui->cb_rects[i] = (GuiRect){ start_x + i * 110, 16, 98, 32 };
    }

    // Layout pagination buttons on Row 2
    gui->prev_btn = (GuiRect){ 20, 62, 130, 34 };
    gui->next_btn = (GuiRect){ 165, 62, 130, 34 };
    gui->crash_btn = (GuiRect){ screen_w - 140, 16, 120, 34 };

    glGenTextures(1, &gui->tex_id);
    glBindTexture(GL_TEXTURE_2D, gui->tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gui->width, gui->height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, gui->pixels);
    return true;
}

void bottom_gui_shutdown(BottomGui *gui) {
    if (gui->tex_id) glDeleteTextures(1, &gui->tex_id);
    free(gui->pixels);
}

GuiAction bottom_gui_handle_click(const BottomGui *gui, int click_x, int click_y,
                                  int current_count, int total_pages) {
	GuiAction act = { false, current_count, 0, false };
	  int bar_top = gui->screen_h - gui->height;
	  if (click_y < bar_top) return act;

	  int local_y = click_y - bar_top;
	  if (pt_in_rect(click_x, local_y, gui->crash_btn)) {
	      act.trigger_crash = true;
	      return act;
	  }
    for (int i = 0; i < GUI_PRESET_COUNT; ++i) {
        if (pt_in_rect(click_x, local_y, gui->cb_rects[i])) {
            if (gui->presets[i] != current_count) {
                act.count_changed = true;
                act.new_count = gui->presets[i];
            }
            return act;
        }
    }

    if (total_pages > 1) {
        if (pt_in_rect(click_x, local_y, gui->prev_btn)) act.page_delta = -1;
        else if (pt_in_rect(click_x, local_y, gui->next_btn)) act.page_delta = +1;
    }
    return act;
}

void bottom_gui_update(BottomGui *gui, int selected_count, int current_page,
                       int total_pages, int offpage_count, float avg_fps,
                       float dropped_fps, float cpu_pct, const char *video_path) {
    (void)video_path;

    char status[256];
    snprintf(status, sizeof(status),
             "PAGE %d/%d | ACT:%d | OFF:%d | FPS:%.1f | DROP/S:%.1f | CPU:%.0f%%",
             current_page + 1, total_pages, selected_count, offpage_count,
             avg_fps, dropped_fps, cpu_pct);
    bool red = (dropped_fps > 1.0f);

    // Nothing visible changed since the last redraw: the texture already holds
    // the right picture, so skip the CPU redraw and the 1920x110 upload.
    if (g_have_last && red == g_last_red && strcmp(status, g_last_status) == 0) {
        return;
    }
    snprintf(g_last_status, sizeof(g_last_status), "%s", status);
    g_last_red = red;
    g_have_last = true;

    // Background + top divider
    fill_rect(gui, 0, 0, gui->width, gui->height, 0x181B22FF);
    fill_rect(gui, 0, 0, gui->width, 2, 0x3A4354FF);

    // Row 1: Checkboxes
    draw_text(gui, 20, 24, 2, "INSTANCES:", 0xDCE3F0FF);
    for (int i = 0; i < GUI_PRESET_COUNT; ++i) {
        GuiRect r = gui->cb_rects[i];
        bool checked = (gui->presets[i] == selected_count);
        draw_border(gui, r.x, r.y + 4, 24, 24, 2, checked ? 0x38BDF8FF : 0x64748BFF);
        if (checked) {
            fill_rect(gui, r.x + 5, r.y + 9, 14, 14, 0x38BDF8FF);
        }
        char num[8];
        snprintf(num, sizeof(num), "%d", gui->presets[i]);
        draw_text(gui, r.x + 32, r.y + 9, 2, num, checked ? 0xFFFFFFFF : 0xB0BAC9FF);
    }

    // Row 2: Pagination buttons + Telemetry (FPS, DROP/S, CPU %)
    uint32_t btn_bg = (total_pages > 1) ? 0x2A3447FF : 0x1E222BFF;
    uint32_t btn_fg = (total_pages > 1) ? 0xFFFFFFFF : 0x555E70FF;

    fill_rect(gui, gui->prev_btn.x, gui->prev_btn.y, gui->prev_btn.w, gui->prev_btn.h, btn_bg);
    draw_border(gui, gui->prev_btn.x, gui->prev_btn.y, gui->prev_btn.w, gui->prev_btn.h, 1, 0x4B5563FF);
    draw_text(gui, gui->prev_btn.x + 14, gui->prev_btn.y + 10, 2, "< PREV", btn_fg);

    fill_rect(gui, gui->next_btn.x, gui->next_btn.y, gui->next_btn.w, gui->next_btn.h, btn_bg);
    draw_border(gui, gui->next_btn.x, gui->next_btn.y, gui->next_btn.w, gui->next_btn.h, 1, 0x4B5563FF);
    draw_text(gui, gui->next_btn.x + 18, gui->next_btn.y + 10, 2, "NEXT >", btn_fg);

    draw_text(gui, 315, 72, 2, status, red ? 0xF87171FF : 0xA7F3D0FF);

    // Red CRASH button (top-right of bottom bar)
      fill_rect(gui, gui->crash_btn.x, gui->crash_btn.y, gui->crash_btn.w, gui->crash_btn.h, 0xB91C1CFF);
      draw_border(gui, gui->crash_btn.x, gui->crash_btn.y, gui->crash_btn.w, gui->crash_btn.h, 2, 0xFCA5A5FF);
      draw_text(gui, gui->crash_btn.x + 28, gui->crash_btn.y + 10, 2, "CRASH", 0xFFFFFFFF);

    glBindTexture(GL_TEXTURE_2D, gui->tex_id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, gui->width, gui->height,
                    GL_RGBA, GL_UNSIGNED_BYTE, gui->pixels);
}

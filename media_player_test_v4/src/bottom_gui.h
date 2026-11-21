// src/bottom_gui.h
#ifndef BOTTOM_GUI_H
#define BOTTOM_GUI_H

#include <GLES3/gl3.h>
#include <stdbool.h>
#include <stdint.h>

#define GUI_PRESET_COUNT 11

typedef struct {
    int x, y, w, h;
} GuiRect;

typedef struct {
    int width;
    int height;
    int screen_h;
    uint8_t *pixels;
    GLuint tex_id;

    int presets[GUI_PRESET_COUNT];
    GuiRect cb_rects[GUI_PRESET_COUNT];
    GuiRect prev_btn;
    GuiRect next_btn;
    GuiRect crash_btn;
} BottomGui;

typedef struct {
    bool count_changed;
    int new_count;
    int page_delta;
    bool trigger_crash;
} GuiAction;

bool bottom_gui_init(BottomGui *gui, int screen_w, int screen_h, int bar_h);
void bottom_gui_shutdown(BottomGui *gui);

GuiAction bottom_gui_handle_click(const BottomGui *gui, int click_x, int click_y,
                                  int current_count, int total_pages);

void bottom_gui_update(BottomGui *gui, int selected_count, int current_page,
                       int total_pages, int offpage_count, float avg_fps,
                       float dropped_fps, float cpu_pct, const char *video_path);

#endif

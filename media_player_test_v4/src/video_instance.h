// src/video_instance.h
#ifndef VIDEO_INSTANCE_H
#define VIDEO_INSTANCE_H

#include <GLES3/gl3.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int id;
    char filepath[256];

    pthread_t thread;
    atomic_bool running;
    atomic_bool frame_dirty;
    atomic_int fps_x10;     // Effective displayed FPS * 10
    atomic_int dropped_x10; // Dropped frames/sec * 10 (CPU + GPU bottleneck)

    pthread_mutex_t buf_mtx;
    uint8_t *rgba_buffer;
    size_t rgba_size;
    int width;
    int height;

    GLuint tex_id;
    bool tex_allocated;
} VideoInstance;

VideoInstance *video_instance_create(int id, const char *filepath);
void video_instance_destroy(VideoInstance *inst);
bool video_instance_start(VideoInstance *inst);
void video_instance_stop(VideoInstance *inst);

void video_instance_upload_texture(VideoInstance *inst);
float video_instance_get_fps(const VideoInstance *inst);
float video_instance_get_dropped_fps(const VideoInstance *inst);

#endif

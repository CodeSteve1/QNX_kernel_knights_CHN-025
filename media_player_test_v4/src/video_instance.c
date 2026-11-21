// src/video_instance.c
#include "video_instance.h"
#include <sys/neutrino.h>
#include <EGL/egl.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Core assignment: each decode thread goes on the least-loaded of cores 0-2.
 *      Core 3 is never used. ---- */
#define NUM_APP_CORES 3
/* Tie-break order: 1, 2 first so core 0 (where the render thread tends to run) fills last */
static const int core_order[NUM_APP_CORES] = { 1, 2, 0 };
/* The render thread lives on core 0 and costs more than a decode thread,
 * so it counts as RENDER_WEIGHT decode threads when balancing. */
#define RENDER_WEIGHT 2
static int core_load[NUM_APP_CORES] = { RENDER_WEIGHT, 0, 0 };
static pthread_mutex_t core_mtx = PTHREAD_MUTEX_INITIALIZER;

static int core_acquire(void) {
    pthread_mutex_lock(&core_mtx);
    int best = core_order[0];
    for (int i = 1; i < NUM_APP_CORES; ++i) {
        int c = core_order[i];
        if (core_load[c] < core_load[best]) best = c;
    }
    core_load[best]++;
    pthread_mutex_unlock(&core_mtx);
    return best;
}

static void core_release(int core) {
    pthread_mutex_lock(&core_mtx);
    if (core_load[core] > 0) core_load[core]--;
    pthread_mutex_unlock(&core_mtx);
}

/* ---- Shared-context texture upload ----
 * Each decode thread gets its own EGL context that shares textures with the
 * main (render) context. The decode thread uploads its frames itself, so the
 * glTexSubImage2D cost moves off the render thread (core 0) onto the cores the
 * decode threads are pinned to. The render thread only draws.
 * If anything fails, that instance silently falls back to the old path
 * (render thread uploads). */
#define MAX_TRACKED_IDS 256
static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_share_ctx = EGL_NO_CONTEXT;
static EGLConfig  g_main_cfg;
static EGLConfig  g_pbuf_cfg;
static bool       g_have_pbuf_cfg = false;
static atomic_int g_gl_upload_enabled = 0;
static atomic_bool g_worker_uploads[MAX_TRACKED_IDS];

/* Off-page "stress" draw, done by the decode thread instead of the render thread.
 * Program and VBO are shared objects; VAO and FBO are per-context, so each worker
 * makes its own. */
static atomic_bool g_offscreen[MAX_TRACKED_IDS];
static atomic_int  g_off_ready = 0;
static GLuint g_off_program = 0;
static GLuint g_off_vbo = 0;
static int    g_off_w = 0, g_off_h = 0;

/* Call once from the render thread after eglMakeCurrent, before creating instances. */
void video_instance_enable_gl_upload(EGLDisplay dpy, EGLConfig main_cfg, EGLContext main_ctx) {
    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint n = 0;
    g_have_pbuf_cfg = eglChooseConfig(dpy, attribs, &g_pbuf_cfg, 1, &n) && n > 0;
    g_dpy = dpy;
    g_main_cfg = main_cfg;
    g_share_ctx = main_ctx;
    atomic_store(&g_gl_upload_enabled, 1);
}

/* Call once from the render thread after gl_renderer_init(). */
void video_instance_set_offscreen_resources(GLuint program, GLuint vbo, int w, int h) {
    glFinish(); /* make sure the shared program/VBO are complete before workers use them */
    g_off_program = program;
    g_off_vbo = vbo;
    g_off_w = w;
    g_off_h = h;
    atomic_store(&g_off_ready, 1);
}

/* Render thread tells each instance whether it is currently off-page. */
void video_instance_set_offscreen(const VideoInstance *inst, bool offscreen) {
    if (inst && inst->id >= 0 && inst->id < MAX_TRACKED_IDS) {
        atomic_store(&g_offscreen[inst->id], offscreen);
    }
}

/* True when this instance's decode thread uploads (and draws off-page) itself. */
bool video_instance_has_worker(const VideoInstance *inst) {
    return inst && inst->id >= 0 && inst->id < MAX_TRACKED_IDS &&
           atomic_load(&g_worker_uploads[inst->id]);
}

typedef struct {
    EGLContext ctx;
    EGLSurface surf;
    GLuint vao, fbo, fbo_tex;
    bool off_init;
} WorkerGL;

static bool worker_gl_init(WorkerGL *w) {
    memset(w, 0, sizeof(*w));
    w->ctx = EGL_NO_CONTEXT;
    w->surf = EGL_NO_SURFACE;
    if (!atomic_load(&g_gl_upload_enabled)) return false;

    eglBindAPI(EGL_OPENGL_ES_API);
    EGLConfig cfg = g_have_pbuf_cfg ? g_pbuf_cfg : g_main_cfg;
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    w->ctx = eglCreateContext(g_dpy, cfg, g_share_ctx, ctx_attribs);
    if (w->ctx == EGL_NO_CONTEXT) return false;

    if (g_have_pbuf_cfg) {
        const EGLint pb[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        w->surf = eglCreatePbufferSurface(g_dpy, g_pbuf_cfg, pb);
    }
    /* If no pbuffer is available we try surfaceless (EGL_NO_SURFACE). */
    if (!eglMakeCurrent(g_dpy, w->surf, w->surf, w->ctx)) {
        if (w->surf != EGL_NO_SURFACE) eglDestroySurface(g_dpy, w->surf);
        eglDestroyContext(g_dpy, w->ctx);
        w->ctx = EGL_NO_CONTEXT;
        w->surf = EGL_NO_SURFACE;
        return false;
    }
    return true;
}

/* Draw this instance's texture into the worker's own 320x180 FBO. */
static void worker_offscreen_draw(WorkerGL *w, GLuint tex) {
    if (!atomic_load(&g_off_ready)) return;

    if (!w->off_init) {
        glGenVertexArrays(1, &w->vao);
        glBindVertexArray(w->vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_off_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
        glBindVertexArray(0);

        glGenFramebuffers(1, &w->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, w->fbo);
        glGenTextures(1, &w->fbo_tex);
        glBindTexture(GL_TEXTURE_2D, w->fbo_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_off_w, g_off_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, w->fbo_tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        w->off_init = true;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, w->fbo);
    glViewport(0, 0, g_off_w, g_off_h);
    glUseProgram(g_off_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(w->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFlush();
}

static void worker_gl_shutdown(WorkerGL *w) {
    if (w->ctx == EGL_NO_CONTEXT) return;
    if (w->off_init) {
        glDeleteFramebuffers(1, &w->fbo);
        glDeleteTextures(1, &w->fbo_tex);
        glDeleteVertexArrays(1, &w->vao);
        w->off_init = false;
    }
    glFinish();
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (w->surf != EGL_NO_SURFACE) eglDestroySurface(g_dpy, w->surf);
    eglDestroyContext(g_dpy, w->ctx);
    w->ctx = EGL_NO_CONTEXT;
    w->surf = EGL_NO_SURFACE;
}

static double monotonic_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

VideoInstance *video_instance_create(int id, const char *filepath) {
    av_log_set_level(AV_LOG_ERROR); // Silence swscaler warnings

    VideoInstance *inst = (VideoInstance *)calloc(1, sizeof(VideoInstance));
    if (!inst) return NULL;

    inst->id = id;
    snprintf(inst->filepath, sizeof(inst->filepath), "%s", filepath);
    atomic_init(&inst->running, false);
    atomic_init(&inst->frame_dirty, false);
    atomic_init(&inst->fps_x10, -1);
    atomic_init(&inst->dropped_x10, 0);
    pthread_mutex_init(&inst->buf_mtx, NULL);

    /* Create the texture name here (render thread, main context current) so the
     * decode thread's shared context can fill it in. Storage is allocated by the
     * decode thread on the first frame. */
    if (atomic_load(&g_gl_upload_enabled)) {
        glGenTextures(1, &inst->tex_id);
        glBindTexture(GL_TEXTURE_2D, inst->tex_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return inst;
}

void video_instance_destroy(VideoInstance *inst) {
    if (!inst) return;
    video_instance_stop(inst);
    if (inst->tex_id != 0) {
        glDeleteTextures(1, &inst->tex_id);
    }
    pthread_mutex_destroy(&inst->buf_mtx);
    free(inst->rgba_buffer);
    free(inst);
}

/* The actual decode/playback loop. Core pinning is handled by decode_thread_fn. */
static void *decode_loop(void *arg) {
    VideoInstance *inst = (VideoInstance *)arg;
    AVFormatContext *fmt_ctx = NULL;

    if (avformat_open_input(&fmt_ctx, inst->filepath, NULL, NULL) < 0) {
        fprintf(stderr, "[Instance %d] Failed to open %s\n", inst->id, inst->filepath);
        atomic_store(&inst->running, false);
        return NULL;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        atomic_store(&inst->running, false);
        return NULL;
    }

    int stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (stream_idx < 0) {
        avformat_close_input(&fmt_ctx);
        atomic_store(&inst->running, false);
        return NULL;
    }

    AVCodecParameters *codecpar = fmt_ctx->streams[stream_idx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "[Instance %d] Unsupported codec: %s\n",
                inst->id, avcodec_get_name(codecpar->codec_id));
        avformat_close_input(&fmt_ctx);
        atomic_store(&inst->running, false);
        return NULL;
    }

    AVCodecContext *dec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec_ctx, codecpar);
    dec_ctx->thread_count = 1;
    if (avcodec_open2(dec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "[Instance %d] Failed to open codec\n", inst->id);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        atomic_store(&inst->running, false);
        return NULL;
    }

    int w = dec_ctx->width;
    int h = dec_ctx->height;
    size_t frame_bytes = (size_t)w * (size_t)h * 4;

    pthread_mutex_lock(&inst->buf_mtx);
    inst->width = w;
    inst->height = h;
    inst->rgba_size = frame_bytes;
    inst->rgba_buffer = (uint8_t *)malloc(frame_bytes);
    pthread_mutex_unlock(&inst->buf_mtx);

    uint8_t *local_rgba = (uint8_t *)malloc(frame_bytes);
    struct SwsContext *sws_ctx = sws_getContext(
        w, h, dec_ctx->pix_fmt, w, h, AV_PIX_FMT_RGBA,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint8_t *dst_data[4] = { local_rgba, NULL, NULL, NULL };
    int dst_linesize[4]  = { w * 4, 0, 0, 0 };

    double stream_fps = av_q2d(fmt_ctx->streams[stream_idx]->avg_frame_rate);
    if (stream_fps <= 0.0 || stream_fps > 120.0) stream_fps = 30.0;

    double frame_duration = 1.0 / stream_fps;
    double next_frame_time = monotonic_sec();
    double t_start = next_frame_time;
    int frames = 0;
    int overwritten_frames = 0;

    /* Try to upload from this thread via a shared context. */
    WorkerGL wgl;
    bool gl_worker = (inst->tex_id != 0) && worker_gl_init(&wgl);
    bool tex_ready = false;
    if (gl_worker && inst->id >= 0 && inst->id < MAX_TRACKED_IDS) {
        atomic_store(&g_worker_uploads[inst->id], true);
    } else if (gl_worker) {
        worker_gl_shutdown(&wgl);
        gl_worker = false;
    }

    while (atomic_load(&inst->running)) {
        int ret = av_read_frame(fmt_ctx, pkt);
        if (ret == AVERROR_EOF) {
            av_seek_frame(fmt_ctx, stream_idx, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(dec_ctx);
            next_frame_time = monotonic_sec();
            continue;
        }
        if (ret < 0) break;

        if (pkt->stream_index == stream_idx) {
            if (avcodec_send_packet(dec_ctx, pkt) == 0) {
                while (avcodec_receive_frame(dec_ctx, frame) == 0 &&
                       atomic_load(&inst->running)) {
                    sws_scale(sws_ctx, (const uint8_t *const *)frame->data,
                              frame->linesize, 0, h, dst_data, dst_linesize);

                    if (gl_worker) {
                        /* Upload here, on this thread's core, into the shared texture. */
                        glBindTexture(GL_TEXTURE_2D, inst->tex_id);
                        if (!tex_ready) {
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                                         GL_RGBA, GL_UNSIGNED_BYTE, local_rgba);
                            tex_ready = true;
                        } else {
                            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                                            GL_RGBA, GL_UNSIGNED_BYTE, local_rgba);
                        }
                        glFlush(); /* make the update visible to the render context */

                        /* Off-page instance: do its stress draw here, not on the render thread. */
                        if (inst->id >= 0 && inst->id < MAX_TRACKED_IDS &&
                            atomic_load(&g_offscreen[inst->id])) {
                            worker_offscreen_draw(&wgl, inst->tex_id);
                        }
                    } else {
                        pthread_mutex_lock(&inst->buf_mtx);
                        uint8_t *tmp = inst->rgba_buffer;
                        inst->rgba_buffer = local_rgba;
                        local_rgba = tmp;
                        dst_data[0] = local_rgba;
                        pthread_mutex_unlock(&inst->buf_mtx);
                    }

                    // If frame_dirty was still true, the GPU never displayed the previous frame
                    if (atomic_exchange(&inst->frame_dirty, true)) {
                        overwritten_frames++;
                    }
                    frames++;

                    // Pace playback to native FPS
                    next_frame_time += frame_duration;
                    double now = monotonic_sec();
                    double sleep_sec = next_frame_time - now;
                    if (sleep_sec > 0.0) {
                        struct timespec req;
                        req.tv_sec = (time_t)sleep_sec;
                        req.tv_nsec = (long)((sleep_sec - (double)req.tv_sec) * 1e9);
                        nanosleep(&req, NULL);
                        now = monotonic_sec();
                    } else if (sleep_sec < -0.5) {
                        next_frame_time = now;
                    }

                    double dt = now - t_start;
                    if (dt >= 1.0) {
                        double displayed_fps = (double)(frames - overwritten_frames) / dt;
                        if (displayed_fps < 0.0) displayed_fps = 0.0;
                        double drop_fps = stream_fps - displayed_fps;
                        if (drop_fps < 0.0) drop_fps = 0.0;

                        atomic_store(&inst->fps_x10, (int)(displayed_fps * 10.0));
                        atomic_store(&inst->dropped_x10, (int)(drop_fps * 10.0));
                        frames = 0;
                        overwritten_frames = 0;
                        t_start = now;
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    if (gl_worker) {
        if (inst->id >= 0 && inst->id < MAX_TRACKED_IDS) {
            atomic_store(&g_worker_uploads[inst->id], false);
        }
        worker_gl_shutdown(&wgl);
    }

    free(local_rgba);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    return NULL;
}

/* Thread entry: pin to the least-loaded core in 0-2, run, then free the slot.
 * Wrapping decode_loop here means its many early `return NULL` paths can't leak a slot. */
static void *decode_thread_fn(void *arg) {
    int core = core_acquire();
    uintptr_t mask = (uintptr_t)1 << core;
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)mask) == -1) {
        perror("Error setting decode thread runmask");
    }
    void *ret = decode_loop(arg);
    core_release(core);
    return ret;
}

bool video_instance_start(VideoInstance *inst) {
    if (!inst || atomic_load(&inst->running)) return true;
    atomic_store(&inst->running, true);
    if (pthread_create(&inst->thread, NULL, decode_thread_fn, inst) != 0) {
        atomic_store(&inst->running, false);
        return false;
    }
    return true;
}

void video_instance_stop(VideoInstance *inst) {
    if (!inst) return;
    if (!atomic_exchange(&inst->running, false)) return;
    pthread_join(inst->thread, NULL);
}

void video_instance_upload_texture(VideoInstance *inst) {
    /* Decode thread uploads this instance itself: just consume the dirty flag so
     * the displayed/dropped FPS accounting still works. */
    if (inst && inst->id >= 0 && inst->id < MAX_TRACKED_IDS &&
        atomic_load(&g_worker_uploads[inst->id])) {
        atomic_store(&inst->frame_dirty, false);
        return;
    }

    if (!inst || !atomic_exchange(&inst->frame_dirty, false)) return;

    if (inst->tex_id == 0) {
        glGenTextures(1, &inst->tex_id);
        glBindTexture(GL_TEXTURE_2D, inst->tex_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, inst->tex_id);
    }

    pthread_mutex_lock(&inst->buf_mtx);
    if (inst->rgba_buffer && inst->width > 0 && inst->height > 0) {
        if (!inst->tex_allocated) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, inst->width, inst->height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, inst->rgba_buffer);
            inst->tex_allocated = true;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, inst->width, inst->height,
                            GL_RGBA, GL_UNSIGNED_BYTE, inst->rgba_buffer);
        }
    }
    pthread_mutex_unlock(&inst->buf_mtx);
}

float video_instance_get_fps(const VideoInstance *inst) {
    int val = atomic_load(&inst->fps_x10);
    return (val < 0) ? 0.0f : (float)val * 0.1f;
}

float video_instance_get_dropped_fps(const VideoInstance *inst) {
    return (float)atomic_load(&inst->dropped_x10) * 0.1f;
}

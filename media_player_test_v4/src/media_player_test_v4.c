// src/media_player_test_v4.c
#include "bottom_gui.h"
#include "gl_renderer.h"
#include "video_instance.h"
#include "supervisor_pulse.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <devctl.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <screen/screen.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/keycodes.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_PER_PAGE       10
#define MAX_INSTANCES      100
#define GUI_BAR_HEIGHT     110
#define HANG_TIMEOUT_MS    6000
#define ENABLE_WATCHDOG    0   // Set to 1 to re-enable internal watchdog
#define DEFAULT_VIDEO      "/data/home/qnxuser/qnx_vid.mp4"

/* Cores 0,1,2 are used by the app; core 3 is left idle. */
#define APP_CORE_MASK      0x07u
#define USED_CORES         3

/* Set to 1 to start one low-priority spinning thread on each of cores 0-2,
 * so all three cores rise to ~100% together the moment the process starts.
 * Core 3 is never touched. */
#define STRESS_BURN        0

/* Number of video instances created at startup. */
#define INITIAL_INSTANCES  1

/* Defined in video_instance.c: lets decode threads upload frames through their own
 * shared EGL contexts. */
void video_instance_enable_gl_upload(EGLDisplay dpy, EGLConfig cfg, EGLContext ctx);
void video_instance_set_offscreen_resources(GLuint program, GLuint vbo, int w, int h);
void video_instance_set_offscreen(const VideoInstance *inst, bool offscreen);
bool video_instance_has_worker(const VideoInstance *inst);

/* This kernel rejects multi-core subset masks (0x03/0x06/0x07 -> EINVAL),
 * so the render thread is pinned to core 0 only. */
static void restrict_to_app_cores(void) {
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)0x01) == -1) {
        perror("ThreadCtl(RUNMASK core 0) failed");
    }
}

#if STRESS_BURN
static void *burn_thread(void *arg) {
    int core = (int)(intptr_t)arg;
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)((uintptr_t)1 << core)) == -1) {
        perror("burn_thread runmask");
    }

    struct sched_param sp;
    int pol;
    if (pthread_getschedparam(pthread_self(), &pol, &sp) == 0 && sp.sched_priority > 1) {
        sp.sched_priority -= 1;
        pthread_setschedparam(pthread_self(), pol, &sp);
    }

    volatile uint64_t x = 0;
    for (;;) x++;
    return NULL;
}

static void start_burners(void) {
    for (int core = 0; core < USED_CORES; ++core) {
        pthread_t t;
        if (pthread_create(&t, NULL, burn_thread, (void *)(intptr_t)core) == 0) {
            pthread_detach(t);
        } else {
            perror("pthread_create burner");
        }
    }
}
#endif

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

// Reads QNX procnto (PID 1) idle thread times to compute CPU % of the
// cores this app is allowed to use (cores 0..USED_CORES-1).
// In procnto, idle thread for core N has tid N+1.
static float sample_qnx_cpu_usage(void) {
    static int proc_fd = -1;
    static uint64_t prev_idle_ns = 0;
    static long long prev_wall_ms = 0;
    static float cached_cpu = 0.0f;

    long long now_ms = monotonic_ms();
    if (prev_wall_ms != 0 && (now_ms - prev_wall_ms) < 1000) {
        return cached_cpu;
    }

    if (proc_fd < 0) {
        proc_fd = open("/proc/1/as", O_RDONLY);
    }
    if (proc_fd < 0) return 0.0f;

    uint64_t total_idle_ns = 0;
    for (int tid = 1; tid <= USED_CORES; ++tid) {
        procfs_status st;
        st.tid = tid;
        if (devctl(proc_fd, DCMD_PROC_TIDSTATUS, &st, sizeof(st), NULL) == EOK) {
            total_idle_ns += st.sutime;
        }
    }

    if (prev_wall_ms != 0 && now_ms > prev_wall_ms) {
        double wall_ns = (double)(now_ms - prev_wall_ms) * 1e6;
        double delta_idle = (double)(total_idle_ns - prev_idle_ns);
        double idle_ratio = delta_idle / (wall_ns * (double)USED_CORES);
        double usage = (1.0 - idle_ratio) * 100.0;
        if (usage < 0.0) usage = 0.0;
        if (usage > 100.0) usage = 100.0;
        cached_cpu = (float)usage;
    }

    prev_idle_ns = total_idle_ns;
    prev_wall_ms = now_ms;
    return cached_cpu;
}

static void rebuild_pool(VideoInstance **pool, int *pool_count,
                         int target_count, const char *video_path,
                         atomic_llong *heartbeat_ms, sup_client_t *hb) {
    for (int i = 0; i < *pool_count; ++i) {
        video_instance_destroy(pool[i]);
        pool[i] = NULL;
        atomic_store(heartbeat_ms, monotonic_ms());
        sup_beat(hb);
    }
    if (target_count > MAX_INSTANCES) target_count = MAX_INSTANCES;
    for (int i = 0; i < target_count; ++i) {
        pool[i] = video_instance_create(i, video_path);
        video_instance_start(pool[i]);
        atomic_store(heartbeat_ms, monotonic_ms());
        sup_beat(hb);
    }
    *pool_count = target_count;
}

static void trigger_intentional_crash(void) {
    fprintf(stderr, "[CrashBtn] Triggering intentional crash (SIGSEGV)...\n");
    fflush(stderr);
    volatile int *boom = NULL;
    *boom = 42;
}

static int run_media_player(const char *video_path, atomic_llong *heartbeat_ms) {
    atomic_store(heartbeat_ms, monotonic_ms());

    sup_client_t hb;
    sup_client_init(&hb, PROC_MEDIA);
    sup_beat(&hb);

    // 0. Keep this process (and everything it spawns) off core 3.
    restrict_to_app_cores();
#if STRESS_BURN
    start_burners();
#endif

    // 1. Initialize QNX Screen
    screen_context_t screen_ctx;
    screen_window_t screen_win;
    if (screen_create_context(&screen_ctx, SCREEN_APPLICATION_CONTEXT) != 0) {
        perror("screen_create_context failed");
        sup_client_close(&hb);
        return 1;
    }
    screen_create_window(&screen_win, screen_ctx);

    int format = SCREEN_FORMAT_RGBA8888;
    screen_set_window_property_iv(screen_win, SCREEN_PROPERTY_FORMAT, &format);

    int usage = SCREEN_USAGE_WRITE; // SCREEN_USAGE_OPENGL_ES3;
    screen_set_window_property_iv(screen_win, SCREEN_PROPERTY_USAGE, &usage);

    int win_size[2] = {1920, 1080};
    screen_set_window_property_iv(screen_win, SCREEN_PROPERTY_SIZE, win_size);
    if (screen_create_window_buffers(screen_win, 2) != 0) {
        perror("screen_create_window_buffers failed");
        sup_client_close(&hb);
        return 1;
    }

    // 2. Initialize EGL
    EGLDisplay egl_disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(egl_disp, NULL, NULL);

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig egl_cfg;
    EGLint num_cfg = 0;
    eglChooseConfig(egl_disp, cfg_attribs, &egl_cfg, 1, &num_cfg);
    if (num_cfg == 0) {
        fprintf(stderr, "eglChooseConfig returned 0 configs (EGL err: 0x%x)\n", eglGetError());
        sup_client_close(&hb);
        return 1;
    }

    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext egl_ctx = eglCreateContext(egl_disp, egl_cfg, EGL_NO_CONTEXT, ctx_attribs);
    EGLSurface egl_surf = eglCreateWindowSurface(
        egl_disp, egl_cfg, (EGLNativeWindowType)screen_win, NULL);
    eglMakeCurrent(egl_disp, egl_surf, egl_surf, egl_ctx);
    eglSwapInterval(egl_disp, 1);
    video_instance_enable_gl_upload(egl_disp, egl_cfg, egl_ctx);

    // 3. Initialize Renderer, Bottom GUI, and Default 1 Video Instance
    GLRenderer renderer;
    gl_renderer_init(&renderer, 320, 180);
    video_instance_set_offscreen_resources(renderer.program, renderer.vbo, 320, 180);

    BottomGui gui;
    bottom_gui_init(&gui, win_size[0], win_size[1], GUI_BAR_HEIGHT);

    VideoInstance *pool[MAX_INSTANCES] = {0};
    int pool_count = 0;
    int current_page = 0;
    rebuild_pool(pool, &pool_count, INITIAL_INSTANCES, video_path, heartbeat_ms, &hb);

    screen_event_t screen_ev;
    screen_create_event(&screen_ev);
    bool running = true;
    bool prev_mouse_down = false;

    while (running) {
        // Pet the watchdog & supervisor every frame
        atomic_store(heartbeat_ms, monotonic_ms());
        sup_beat(&hb);

        int total_pages = (pool_count + MAX_PER_PAGE - 1) / MAX_PER_PAGE;
        if (total_pages < 1) total_pages = 1;

        // 4. Poll QNX Screen events
        while (screen_get_event(screen_ctx, screen_ev, 0) == 0) {
            int type = SCREEN_EVENT_NONE;
            screen_get_event_property_iv(screen_ev, SCREEN_PROPERTY_TYPE, &type);
            if (type == SCREEN_EVENT_NONE) break;

            if (type == SCREEN_EVENT_CLOSE) {
                running = false;
            } else if (type == SCREEN_EVENT_POINTER) {
                int pos[2] = {0, 0}, buttons = 0;
                screen_get_event_property_iv(screen_ev, SCREEN_PROPERTY_SOURCE_POSITION, pos);
                screen_get_event_property_iv(screen_ev, SCREEN_PROPERTY_BUTTONS, &buttons);
                bool down = (buttons & SCREEN_LEFT_MOUSE_BUTTON) != 0;
                if (down && !prev_mouse_down) {
                    GuiAction act = bottom_gui_handle_click(&gui, pos[0], pos[1], pool_count, total_pages);
                    if (act.trigger_crash) {
                        trigger_intentional_crash();
                    } else if (act.count_changed) {
                        current_page = 0;
                        rebuild_pool(pool, &pool_count, act.new_count, video_path, heartbeat_ms, &hb);
                    } else if (act.page_delta != 0) {
                        current_page += act.page_delta;
                        if (current_page < 0) current_page = 0;
                        if (current_page >= total_pages) current_page = total_pages - 1;
                    }
                }
                prev_mouse_down = down;
            } else if (type == SCREEN_EVENT_MTOUCH_TOUCH) {
                int pos[2] = {0, 0};
                screen_get_event_property_iv(screen_ev, SCREEN_PROPERTY_SOURCE_POSITION, pos);
                GuiAction act = bottom_gui_handle_click(&gui, pos[0], pos[1], pool_count, total_pages);
                if (act.trigger_crash) {
                    trigger_intentional_crash();
                } else if (act.count_changed) {
                    current_page = 0;
                    rebuild_pool(pool, &pool_count, act.new_count, video_path, heartbeat_ms, &hb);
                } else if (act.page_delta != 0) {
                    current_page += act.page_delta;
                    if (current_page < 0) current_page = 0;
                    if (current_page >= total_pages) current_page = total_pages - 1;
                }
            } else if (type == SCREEN_EVENT_KEYBOARD) {
                int flags = 0, sym = 0;
                screen_get_event_property_iv(screen_ev, SCREEN_PROPERTY_FLAGS, &flags);
                screen_get_event_property_iv(screen_ev, SCREEN_PROPERTY_SYM, &sym);
                if (flags & KEY_DOWN) {
                    if (sym == KEYCODE_ESCAPE) running = false;
                    else if (sym == KEYCODE_LEFT && current_page > 0) current_page--;
                    else if (sym == KEYCODE_RIGHT && current_page + 1 < total_pages) current_page++;
                }
            }
        }

        // 5. Upload decoded frames for ALL instances
        float sum_fps = 0.0f;
        float sum_drop = 0.0f;
        for (int i = 0; i < pool_count; ++i) {
            video_instance_upload_texture(pool[i]);
            sum_fps  += video_instance_get_fps(pool[i]);
            sum_drop += video_instance_get_dropped_fps(pool[i]);
        }
        float avg_fps  = (pool_count > 0) ? (sum_fps  / (float)pool_count) : 0.0f;
        float avg_drop = (pool_count > 0) ? (sum_drop / (float)pool_count) : 0.0f;
        float cpu_pct  = sample_qnx_cpu_usage();

        total_pages = (pool_count + MAX_PER_PAGE - 1) / MAX_PER_PAGE;
        if (total_pages < 1) total_pages = 1;
        if (current_page >= total_pages) current_page = total_pages - 1;

        int page_start = current_page * MAX_PER_PAGE;
        int page_end = page_start + MAX_PER_PAGE;
        if (page_end > pool_count) page_end = pool_count;
        int visible_count = page_end - page_start;
        int offpage_count = pool_count - visible_count;

        // 6. Offscreen GPU stress pass for OFF-PAGE instances.
        bool main_offscreen_needed = false;
        for (int i = 0; i < pool_count; ++i) {
            bool off = !(i >= page_start && i < page_end);
            video_instance_set_offscreen(pool[i], off);
            if (off && !video_instance_has_worker(pool[i])) main_offscreen_needed = true;
        }
        if (main_offscreen_needed) {
            gl_renderer_begin_offscreen(&renderer);
            for (int i = 0; i < pool_count; ++i) {
                if (i >= page_start && i < page_end) continue;
                if (video_instance_has_worker(pool[i])) continue;
                gl_renderer_draw_quad(&renderer, pool[i]->tex_id, 0, 0, 320, 180);
            }
            gl_renderer_end_offscreen();
        }

        // 7. On-screen split grid pass for VISIBLE instances
        glViewport(0, 0, win_size[0], win_size[1]);
        glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        int cols = 1, rows = 1;
        gl_renderer_compute_grid(visible_count, &cols, &rows);
        int video_area_h = win_size[1] - GUI_BAR_HEIGHT;
        int cell_w = win_size[0] / cols;
        int cell_h = video_area_h / rows;

        for (int idx = 0; idx < visible_count; ++idx) {
            int c = idx % cols;
            int r = idx / cols;
            int x = c * cell_w;
            int y = GUI_BAR_HEIGHT + (rows - 1 - r) * cell_h;
            gl_renderer_draw_quad(&renderer, pool[page_start + idx]->tex_id, x, y, cell_w, cell_h);
        }

        // 8. Draw Bottom GUI Bar
        bottom_gui_update(&gui, pool_count, current_page, total_pages,
                          offpage_count, avg_fps, avg_drop, cpu_pct, video_path);
        gl_renderer_draw_quad(&renderer, gui.tex_id, 0, 0, win_size[0], GUI_BAR_HEIGHT);

        eglSwapBuffers(egl_disp, egl_surf);
    }

    // Cleanup
    rebuild_pool(pool, &pool_count, 0, video_path, heartbeat_ms, &hb);
    bottom_gui_shutdown(&gui);
    gl_renderer_shutdown(&renderer);
    screen_destroy_event(screen_ev);
    eglDestroySurface(egl_disp, egl_surf);
    eglDestroyContext(egl_disp, egl_ctx);
    eglTerminate(egl_disp);
    screen_destroy_window(screen_win);
    screen_destroy_context(screen_ctx);
    sup_client_close(&hb);
    return 0;
}

int main(int argc, char **argv) {
    /* Render thread on core 0; decode threads on 0-2; core 3 left idle. */
    restrict_to_app_cores();

    const char *video_path = (argc > 1) ? argv[1] : DEFAULT_VIDEO;

#if !ENABLE_WATCHDOG
    atomic_llong dummy_hb;
    atomic_init(&dummy_hb, 0);
    return run_media_player(video_path, &dummy_hb);
#else
    atomic_llong *heartbeat_ms = mmap(NULL, sizeof(atomic_llong),
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (heartbeat_ms == MAP_FAILED) {
        perror("mmap heartbeat failed");
        return 1;
    }

    while (1) {
        atomic_store(heartbeat_ms, monotonic_ms());
        pid_t pid = fork();

        if (pid == 0) {
            int rc = run_media_player(video_path, heartbeat_ms);
            _exit(rc);
        } else if (pid < 0) {
            perror("fork failed");
            return 1;
        }

        // Parent: Watchdog monitoring loop
        while (1) {
            int status = 0;
            pid_t res = waitpid(pid, &status, WNOHANG);

            if (res == pid) {
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    munmap(heartbeat_ms, sizeof(atomic_llong));
                    return 0;
                }
                fprintf(stderr, "[Watchdog] Crash detected (status=0x%x). Restarting with 1 instance...\n", status);
                usleep(500000);
                break;
            }

            long long elapsed = monotonic_ms() - atomic_load(heartbeat_ms);
            if (elapsed > HANG_TIMEOUT_MS) {
                fprintf(stderr, "[Watchdog] Hang detected (%lld ms unresponsive). Killing and restarting with 1 instance...\n", elapsed);
                kill(pid, SIGKILL);
                waitpid(pid, NULL, 0);
                usleep(500000);
                break;
            }

            usleep(250000);
        }
    }
#endif
}

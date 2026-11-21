#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include "supervisor_pulse.h"

/* ---------------- Configuration ---------------- */
/* ---------------- Configuration ---------------- */
/* ---------------- Configuration ---------------- */
#define BIN_PATH_LCD      "/data/home/qnxuser/bin/lcd_proc_v1/build/aarch64le-debug/lcd_proc_v1"
#define BIN_PATH_SENSOR   "/data/home/qnxuser/bin/sensor_msg_v1/build/aarch64le-debug/sensor_msg_v1"
#define BIN_PATH_MEDIA    "/data/home/qnxuser/bin/media_player_test_v4/build/aarch64le-debug/media_player_test_v4"

#define PULSE_CODE_SUP_HB       (_PULSE_CODE_MINAVAIL + 2)
#define PULSE_CODE_CHILD_PID    (_PULSE_CODE_MINAVAIL + 10) /* + proc_id */

#define SUPERVISOR_TIMEOUT_MS   4000
#define RESTART_BACKOFF_US      300000

typedef struct {
    proc_id_t   id;
    const char *name;
    const char *path;
    int64_t     hang_timeout_ms;
    int         startup_delay_us;
} proc_spec_t;

static const proc_spec_t PROCS[PROC_COUNT] = {
    { PROC_LCD,    "lcd_proc",     BIN_PATH_LCD,    4000, 300000 },
    { PROC_SENSOR, "sensor_proc",  BIN_PATH_SENSOR, 4000, 100000 },
    { PROC_MEDIA,  "media_player", BIN_PATH_MEDIA,  7000, 0      },
};

static volatile sig_atomic_t g_stop = 0;

static void handle_sig(int sig) {
    (void)sig;
    g_stop = 1;
}

static int is_pid_alive(pid_t pid) {
    if (pid <= 0) return 0;
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) return 0;
    if (r < 0 && errno == ECHILD) {
        return (kill(pid, 0) == 0 || errno == EPERM);
    }
    return 1;
}

static void kill_and_reap(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static pid_t spawn_process(const proc_spec_t *spec, int guardian_coid, int64_t *last_hb) {
    last_hb[spec->id] = sup_monotonic_ms();

    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)spec->path, NULL };
        execv(spec->path, argv);
        fprintf(stderr, "[SUP] execv(%s) failed: %s\n", spec->path, strerror(errno));
        _exit(127);
    } else if (pid < 0) {
        perror("[SUP] fork child failed");
        return -1;
    }

    /* Report new child PID to Guardian via pulse */
    MsgSendPulse(guardian_coid, -1, PULSE_CODE_CHILD_PID + spec->id, (int)pid);
    printf("[SUP] Started %s (pid=%d)\n", spec->name, (int)pid);
    fflush(stdout);

    if (spec->startup_delay_us > 0) {
        usleep(spec->startup_delay_us);
    }
    return pid;
}

/* ---------------- Supervisor Worker Loop ---------------- */
static int run_supervisor_worker(pid_t guardian_pid, int guardian_chid, pid_t child_pids[PROC_COUNT]) {
    unsigned runmask = (1U << 3);
    ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)runmask);

    int guardian_coid = ConnectAttach(0, guardian_pid, guardian_chid, _NTO_SIDE_CHANNEL, 0);
    if (guardian_coid == -1) {
        perror("[SUP] ConnectAttach to guardian failed");
        return 1;
    }

    name_attach_t *attach = NULL;
    for (int tries = 0; tries < 10; tries++) {
        attach = name_attach(NULL, SUP_CHANNEL_NAME, 0);
        if (attach) break;
        if (errno != EEXIST) break;
        usleep(100000); /* Wait 100ms for old channel to clear */
    }
    if (!attach) {
        perror("[SUP] name_attach failed");
        ConnectDetach(guardian_coid);
        return 1;
    }

    int64_t last_hb[PROC_COUNT];
    int64_t now = sup_monotonic_ms();

    /* Adopt already-running children or spawn missing ones */
    for (int i = 0; i < PROC_COUNT; i++) {
        last_hb[i] = now;
        if (!is_pid_alive(child_pids[i])) {
            child_pids[i] = spawn_process(&PROCS[i], guardian_coid, last_hb);
        } else {
            printf("[SUP] Adopted existing %s (pid=%d)\n", PROCS[i].name, (int)child_pids[i]);
        }
    }

    struct _pulse pulse;

    while (!g_stop) {
        /* Pet the Guardian */
        MsgSendPulse(guardian_coid, -1, PULSE_CODE_SUP_HB, 0);

        /* Wait up to 250ms for heartbeat pulses from children */
        uint64_t timeout_ns = 250000000ULL;
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &timeout_ns, NULL);

        int r = MsgReceivePulse(attach->chid, &pulse, sizeof(pulse), NULL);
        if (r == 0) {
            if (pulse.code == PULSE_CODE_HEARTBEAT) {
                int id = pulse.value.sival_int;
                if (id >= 0 && id < PROC_COUNT) {
                    last_hb[id] = sup_monotonic_ms();
                }
            } else if (pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(pulse.scoid);
            }
        }

        /* Check all children for crashes or hangs */
        now = sup_monotonic_ms();
        for (int i = 0; i < PROC_COUNT; i++) {
            if (!is_pid_alive(child_pids[i])) {
                fprintf(stderr, "[SUP] %s (pid=%d) is DOWN. Restarting...\n",
                        PROCS[i].name, (int)child_pids[i]);
                usleep(RESTART_BACKOFF_US);
                child_pids[i] = spawn_process(&PROCS[i], guardian_coid, last_hb);
                continue;
            }

            if ((now - last_hb[i]) > PROCS[i].hang_timeout_ms) {
                fprintf(stderr, "[SUP] %s (pid=%d) HUNG (%lld ms). Killing & restarting...\n",
                        PROCS[i].name, (int)child_pids[i], (long long)(now - last_hb[i]));
                kill_and_reap(child_pids[i]);
                usleep(RESTART_BACKOFF_US);
                child_pids[i] = spawn_process(&PROCS[i], guardian_coid, last_hb);
            }
        }
    }

    name_detach(attach, 0);
    ConnectDetach(guardian_coid);
    return 0;
}

/* ---------------- Guardian (Watches the Supervisor) ---------------- */
int main(void) {
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    int guardian_chid = ChannelCreate(0);
    if (guardian_chid == -1) {
        perror("[GUARDIAN] ChannelCreate");
        return EXIT_FAILURE;
    }

    pid_t guardian_pid = getpid();
    pid_t child_pids[PROC_COUNT] = {0};

    printf("[GUARDIAN] Starting pulse-based self-healing supervisor...\n");
    fflush(stdout);

    while (!g_stop) {
        pid_t sup_pid = fork();
        if (sup_pid == 0) {
            int rc = run_supervisor_worker(guardian_pid, guardian_chid, child_pids);
            _exit(rc);
        } else if (sup_pid < 0) {
            perror("[GUARDIAN] fork supervisor failed");
            sleep(1);
            continue;
        }

        printf("[GUARDIAN] Supervisor worker running (pid=%d)\n", (int)sup_pid);
        fflush(stdout);

        int64_t last_sup_hb = sup_monotonic_ms();
        struct _pulse pulse;

        while (!g_stop) {
            uint64_t timeout_ns = 500000000ULL; /* 500ms */
            TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &timeout_ns, NULL);

            int r = MsgReceivePulse(guardian_chid, &pulse, sizeof(pulse), NULL);
            if (r == 0) {
                if (pulse.code == PULSE_CODE_SUP_HB) {
                    last_sup_hb = sup_monotonic_ms();
                } else if (pulse.code >= PULSE_CODE_CHILD_PID &&
                           pulse.code < PULSE_CODE_CHILD_PID + PROC_COUNT) {
                    int idx = pulse.code - PULSE_CODE_CHILD_PID;
                    child_pids[idx] = (pid_t)pulse.value.sival_int;
                }
            }

            /* Check if Supervisor crashed/exited */
            int status = 0;
            if (waitpid(sup_pid, &status, WNOHANG) == sup_pid) {
                if (g_stop) break;
                fprintf(stderr, "[GUARDIAN] Supervisor exited/crashed (status=0x%x). Restarting...\n", status);
                usleep(RESTART_BACKOFF_US);
                break;
            }

            /* Check if Supervisor hung */
            int64_t elapsed = sup_monotonic_ms() - last_sup_hb;
            if (elapsed > SUPERVISOR_TIMEOUT_MS) {
                fprintf(stderr, "[GUARDIAN] Supervisor HUNG (%lld ms). Killing & restarting...\n",
                        (long long)elapsed);
                kill(sup_pid, SIGKILL);
                waitpid(sup_pid, NULL, 0);
                usleep(RESTART_BACKOFF_US);
                break;
            }
        }
    }

    printf("[GUARDIAN] Shutting down managed processes...\n");
    for (int i = 0; i < PROC_COUNT; i++) {
        if (child_pids[i] > 0) kill(child_pids[i], SIGTERM);
    }
    ChannelDestroy(guardian_chid);
    return EXIT_SUCCESS;
}

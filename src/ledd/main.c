// brickos-ledd: the BrickOS LED daemon. docs/ledd.md describes the modes, the configuration, the
// health and applied files, and the driver notes.
//
//   brickos-ledd [--root DIR]                 run until SIGTERM, which ends its own effects
//   brickos-ledd [--root DIR] --end-effects   end the pattern a dead daemon left, and exit
//
// --root puts every path under DIR, for host runs against a fixture tree; keymon's shared memory
// is then not used, and ledpolicy comes from DIR's system.json.
#define _POSIX_C_SOURCE 200809L
#include "daemon.h"
#include "../common/version.h"
BRICKOS_DECLARE_COMPONENT("ledd", BRICKOS_VERSION_LEDD);

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>

#define KEYMON_SHM_KEY 0x4e4f4d4b
#define KEYMON_SHM_VALUES 23
#define KEYMON_LEDPOLICY 19

static volatile sig_atomic_t stop_requested;
static void on_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

// keymon may start after the daemon, so the segment is looked for until it is found.
static const int *keymon_values;
static int keymon_policy(void *context, int *value) {
    (void)context;
    if (!keymon_values) {
        int id = shmget(KEYMON_SHM_KEY, KEYMON_SHM_VALUES * sizeof(int), 0);
        if (id < 0) return -1;
        void *memory = shmat(id, NULL, SHM_RDONLY);
        if (memory == (void *)-1) return -1;
        keymon_values = memory;
    }
    *value = keymon_values[KEYMON_LEDPOLICY];
    return 0;
}

// xorshift32, for the random colour of each cycle.
static unsigned random_state;
static unsigned next_random(void *context) {
    (void)context;
    unsigned x = random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return random_state = x;
}

int main(int argc, char **argv) {
    BRICKOS_HANDLE_VERSION(argc, argv);
    const char *root = "";
    int end_effects = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--end-effects")) end_effects = 1;
        else {
            fprintf(stderr, "usage: brickos-ledd [--root DIR] [--end-effects]\n");
            return 2;
        }
    }
    if (end_effects) return ledd_end_effects(root) == 0 ? 0 : 1;

    struct sigaction action = {.sa_handler = on_stop};
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    random_state = (unsigned)time(NULL) ^ (unsigned)getpid() << 16 ^ (unsigned)now_ms();
    if (!random_state) random_state = 1;
    const struct ledd_random random = {next_random, NULL};
    static struct ledd_daemon daemon;
    ledd_daemon_init(&daemon, root, (long)getpid(), now_ms(), &random, root[0] ? NULL : keymon_policy, NULL);
    while (!stop_requested) {
        uint64_t now = now_ms(), next = ledd_daemon_tick(&daemon, now);
        uint64_t wait = next > now ? next - now : 0;
        if (wait > LEDD_POLL_MS) wait = LEDD_POLL_MS;
        // A signal ends the sleep early.
        struct timespec pause = {(time_t)(wait / 1000), (long)(wait % 1000) * 1000000L};
        if (wait) nanosleep(&pause, NULL);
    }
    ledd_daemon_stop(&daemon, now_ms());
    return 0;
}

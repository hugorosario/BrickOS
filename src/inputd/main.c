// BrickOS replacement for the TrimUI Brick's trimui_inputd. It reads the physical controls
// from GPIO and publishes them as the stock TRIMUI Player1 controller.
// Physical POWER is read separately from the PMIC evdev node axp2202-pek (EV_KEY/KEY_POWER).
// The baseline grabs it while MainUI runs. An opt-in GameSwitcher build may also
// claim a validated live game lease. Other games/apps retain stock keymon POWER.
// POWER is never added to the uinput controller.
//
//   trimui_inputd [--run] [--config FILE] [--health FILE] [--run-for-ms MS] [--fail-write-after-ms MS]
//   trimui_inputd --check-config [FILE]   validate a profile and print the resolved map
//   trimui_inputd --replay FILE [--config FILE]  run a scripted input trace through the engine
//   trimui_inputd --read-gpio MS          passive, read-only pin trace while another producer runs
//   trimui_inputd --version
//
// --check-config and --replay open neither /dev/mem nor /dev/uinput. --fail-write-after-ms is a
// test hook: after MS it treats the next event write as failed, as a real failure would be.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "engine.h"
#include "gpio.h"
#include "profile.h"
#include "power.h"
#include "power_source.h"
#include "uinput.h"
#ifdef BRICKOS_GAMESWITCHER_POWER
#include "../gameswitcher/power_control.h"
#endif

#include "../common/version.h"
BRICKOS_DECLARE_COMPONENT("inputd", BRICKOS_VERSION_INPUTD);
#define INPUTD_VERSION BRICKOS_VERSION_INPUTD
#define SCAN_US 8333
#define HEALTH_PATH "/tmp/brickos-inputd.health"
#define FN_EXPORT "/sys/class/gpio/export"
#define FN_DIRECTION "/sys/class/gpio/gpio243/direction"
#define FN_VALUE "/sys/class/gpio/gpio243/value"
#define DISP_LCD_GET_BRIGHTNESS 0x103
#define POWER_OWNER_POLL_MS 50

enum { EXIT_USAGE = 2, EXIT_CONFIG = 3, EXIT_GPIO = 4, EXIT_UINPUT = 5, EXIT_WRITE = 6 };

static volatile sig_atomic_t stop_requested, reload_requested;
static void on_stop(int sig) { (void)sig; stop_requested = 1; }
static void on_reload(int sig) { (void)sig; reload_requested = 1; }

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int exists(const char *path) { return access(path, F_OK) == 0; }

static const char *dest_name(const struct inputd_dest *d, char *buf, size_t size) {
    switch (d->type) {
    case INPUTD_EV_KEY: snprintf(buf, size, "key %d", d->code); break;
    case INPUTD_EV_ABS: snprintf(buf, size, "abs %d %+d", d->code, d->value); break;
    case INPUTD_EV_SW: snprintf(buf, size, "switch %d", d->code); break;
    default: snprintf(buf, size, "disabled"); break;
    }
    return buf;
}

// The stock destination of each profile source, for the resolved map.
static struct inputd_dest stock_dest(int source) {
    int bit = inputd_sources[source].bit;
    if (bit < 0) return (struct inputd_dest){INPUTD_EV_SW, INPUTD_SW_FN, 1};
    for (int i = 0; i < INPUTD_KEYS; ++i)
        if (inputd_keys[i].bit == bit) return (struct inputd_dest){INPUTD_EV_KEY, inputd_keys[i].code, 1};
    for (int i = 0; i < INPUTD_DPAD; ++i)
        if (inputd_dpad_hat[i].bit == bit) return (struct inputd_dest){INPUTD_EV_ABS, inputd_dpad_hat[i].axis, inputd_dpad_hat[i].value};
    return (struct inputd_dest){0, 0, 0};
}

static int check_config(const char *path) {
    struct inputd_config config;
    char error[256];
    int loaded = inputd_config_load(&config, path, error, sizeof(error));
    if (loaded < 0) {
        fprintf(stderr, "invalid profile: %s\n", error);
        return EXIT_CONFIG;
    }
    printf("profile %s: %s\n", path, loaded ? "loaded" : "absent, stock mapping");
    printf("active_profile %s\n", config.active_profile);
    for (int s = 0; s < INPUTD_SOURCES; ++s) {
        char buf[48];
        struct inputd_dest d = config.bound[s] ? config.dest[s] : stock_dest(s);
        printf("%-12s %-8s %s\n", inputd_sources[s].name, config.bound[s] ? "binding" : "stock", dest_name(&d, buf, sizeof(buf)));
    }
    return 0;
}

static void print_events(const struct inputd_events *events, long long t) {
    for (int i = 0; i < events->count; ++i)
        printf("%lld %d %d %d\n", t, events->ev[i].type, events->ev[i].code, events->ev[i].value);
}

// The 1 s tick's profile check: reloads when SIGHUP asked for it or when the file's inode, size
// or modification time changed since the last load, including its removal (the stock mapping).
// Returns 1 after a reload, -1 when the new file was rejected and the active mapping stays,
// and 0 when nothing changed.
static int reload_profile(const char *path, struct inputd_config_stamp *stamp, int requested,
                          struct inputd_engine *e, struct inputd_config *config, int *loaded,
                          uint32_t pressed, struct inputd_events *out, char *error, size_t size) {
    struct inputd_config_stamp now;
    inputd_config_stamp(path, &now);
    if (!requested && inputd_config_stamp_same(&now, stamp)) return 0;
    *stamp = now;
    struct inputd_config next;
    int ok = inputd_config_load(&next, path, error, size);
    if (ok < 0) return -1;
    *config = next;
    *loaded = ok;
    inputd_engine_reload(e, ok > 0 ? config : NULL, pressed, out);
    return 1;
}

// Replay's stand-in for another writer: puts FILE at path by rename, or in place, with a fixed
// modification time, as two writes within one second on a card with coarse timestamps look.
static int replay_write(const char *file, const char *path, int rename_into_place) {
    char tmp[512], data[65536];
    FILE *in = fopen(file, "rb");
    if (!in) return -1;
    size_t length = fread(data, 1, sizeof(data), in);
    fclose(in);
    snprintf(tmp, sizeof(tmp), "%s.replay-tmp", path);
    const char *target = rename_into_place ? tmp : path;
    FILE *out = fopen(target, "wb");
    if (!out) return -1;
    int failed = fwrite(data, 1, length, out) != length;
    failed |= fclose(out) != 0;
    struct timespec times[2] = {{1700000000, 0}, {1700000000, 0}};
    failed |= utimensat(AT_FDCWD, target, times, 0) != 0;
    if (!failed && rename_into_place) failed = rename(tmp, path) != 0;
    return failed ? -1 : 0;
}

static int source_bit(const char *name) {
    int s = inputd_config_source(name);
    return s >= 0 ? inputd_sources[s].bit : -2;
}

// A replay script: one command per line, in scan passes of 8.333 ms.
//   press NAME... | release NAME... | pass [N] | marker FILE on|off | power press|release | advance MS
//   fn 0|1 | grab | suspend on|off
//   reload [FILE]
//   replace FILE | write FILE | remove | hup | tick   (need --config)
// NAME is a profile source name (A, B, X, Y, L1, R1, L2, R2, SELECT, START, GUIDE, F1, F2,
// VOLUME_UP, VOLUME_DOWN, DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT). Each output line is
// "PASS TYPE CODE VALUE". replace, write and remove change the --config file as another
// writer would (replace by rename, write in place); hup stands for SIGHUP, and tick runs the
// daemon's 1 s profile check, printing "PASS profile NAME" when it reloads.
static void replay_power(struct inputd_power_engine *power,uint64_t at,int held,
                         int suppressed,int pending,const char *session) {
    if (*session) {
        enum inputd_game_power_action action=inputd_power_engine_update_game(power,at,held,suppressed,pending);
        char transaction[33];
        if (action && !inputd_power_transaction(power,session,transaction))
            printf("GAME_POWER %s t=%llu transaction=%s\n",action==INPUTD_GAME_POWER_SHORT?"short":"long",
                   (unsigned long long)at,transaction);
    } else if (inputd_power_engine_update(power,at,held,suppressed,pending))
        printf("POWER_REQUEST t=%llu\n",(unsigned long long)at);
}
static int replay(const char *path, const char *config_path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return EXIT_USAGE; }
    struct inputd_config config;
    char error[256];
    struct inputd_config_stamp stamp;
    if (config_path) inputd_config_stamp(config_path, &stamp);
    int loaded = config_path ? inputd_config_load(&config, config_path, error, sizeof(error)) : 0;
    if (loaded < 0) { fprintf(stderr, "invalid profile: %s\n", error); fclose(f); return EXIT_CONFIG; }
    int hup = 0;
    static struct inputd_engine e;
    inputd_engine_init(&e, loaded > 0 ? &config : NULL);
    struct inputd_power_engine power;
    inputd_power_engine_init(&power, 0);
    int power_held = 0, power_marker = 0, keytest_marker = 0;
    uint64_t power_time_ms = 0;
    char game_session[33]={0};
    uint32_t pressed = 0;
    int suspended = 0;
    long long pass = 0;
    char line[256];
    int lineno = 0, status = 0;
    while (fgets(line, sizeof(line), f)) {
        ++lineno;
        char *word = strtok(line, " \t\r\n");
        if (!word || word[0] == '#') continue;
        struct inputd_events out = {0};
        if (!strcmp(word,"game-session")) {
            char *session=strtok(NULL," \t\r\n"),probe[33];
            struct inputd_power_engine validator={.sequence=1};
            if (!session || inputd_power_transaction(&validator,session,probe)) { status=EXIT_USAGE; goto done; }
            strcpy(game_session,session);
            inputd_power_engine_init(&power,power_held);
        } else if (!strcmp(word, "power")) {
            char *action = strtok(NULL, " \t\r\n");
            if (!action || (strcmp(action, "press") && strcmp(action, "release"))) {
                fprintf(stderr, "%s:%d: expected power press|release\n", path, lineno);
                status = EXIT_USAGE; goto done;
            }
            power_held = !strcmp(action, "press");
            replay_power(&power,power_time_ms,power_held,keytest_marker,power_marker,game_session);
        } else if (!strcmp(word, "advance")) {
            char *duration = strtok(NULL, " \t\r\n"), *end = NULL;
            unsigned long long delta = duration ? strtoull(duration, &end, 10) : 0;
            if (!duration || *duration == '-' || end == duration || *end != 0 || delta > UINT64_MAX - power_time_ms) {
                fprintf(stderr, "%s:%d: invalid advance duration\n", path, lineno);
                status = EXIT_USAGE; goto done;
            }
            power_time_ms += delta;
            replay_power(&power,power_time_ms,power_held,keytest_marker,power_marker,game_session);
        } else if (!strcmp(word, "press") || !strcmp(word, "release")) {
            int press = word[0] == 'p';
            for (char *name; (name = strtok(NULL, " \t\r\n"));) {
                int bit = !strcmp(name, "L2") ? INPUTD_BIT_L2 : !strcmp(name, "R2") ? INPUTD_BIT_R2 : source_bit(name);
                if (bit < 0) { fprintf(stderr, "%s:%d: unknown control %s\n", path, lineno, name); status = EXIT_USAGE; goto done; }
                if (press) pressed |= 1u << bit; else pressed &= ~(1u << bit);
            }
            inputd_engine_scan(&e, pressed, suspended, &out);
            print_events(&out, pass++);
        } else if (!strcmp(word, "pass")) {
            char *n = strtok(NULL, " \t\r\n");
            long count = n ? strtol(n, NULL, 10) : 1;
            for (long i = 0; i < count; ++i) {
                out.count = 0;
                inputd_engine_scan(&e, pressed, suspended, &out);
                print_events(&out, pass++);
            }
        } else if (!strcmp(word, "marker")) {
            char *name = strtok(NULL, " \t\r\n"), *state = strtok(NULL, " \t\r\n");
            int on = state && !strcmp(state, "on");
            if (!name || !state) { status = EXIT_USAGE; goto done; }
            int found = 0;
            for (int i = 0; i < INPUTD_TURBOS; ++i)
                if (!strcmp(strrchr(inputd_turbo_markers[i].path, '/') + 1, name)) {
                    found = 1;
                    if (on) e.markers.turbo |= 1u << inputd_turbo_markers[i].bit; else e.markers.turbo &= ~(1u << inputd_turbo_markers[i].bit);
                }
            for (int i = 0; i < INPUTD_ALTS; ++i)
                if (!strcmp(strrchr(inputd_alt_markers[i].path, '/') + 1, name)) {
                    found = 1;
                    if (on) e.markers.alt |= 1u << inputd_alt_markers[i].bit; else e.markers.alt &= ~(1u << inputd_alt_markers[i].bit);
                }
            if (!strcmp(name, "power-hold")) { found = 1; power_marker = on; }
            if (!strcmp(name, "keytestmode")) { found = 1; keytest_marker = on; }
            if (!strcmp(name, "input_no_dpad")) { found = 1; e.markers.no_dpad = on; }
            if (!strcmp(name, "input_dpad_to_joystick")) { found = 1; e.markers.dpad2axis = on; }
            if (!strcmp(name, "rotate_270")) { found = 1; e.markers.rotate = on; }
            if (!found) { fprintf(stderr, "%s:%d: unknown marker %s\n", path, lineno, name); status = EXIT_USAGE; goto done; }
            if (!strcmp(name,"power-hold") || !strcmp(name,"keytestmode"))
                replay_power(&power,power_time_ms,power_held,keytest_marker,power_marker,game_session);
        } else if (!strcmp(word, "fn")) {
            char *v = strtok(NULL, " \t\r\n");
            inputd_engine_fn(&e, v ? atoi(v) : 0, &out);
            print_events(&out, pass);
        } else if (!strcmp(word, "grab")) {
            inputd_engine_grab(&e, &out);
            print_events(&out, pass);
        } else if (!strcmp(word, "suspend")) {
            char *v = strtok(NULL, " \t\r\n");
            suspended = v && !strcmp(v, "on");
        } else if (!strcmp(word, "reload")) {
            char *file = strtok(NULL, " \t\r\n");
            struct inputd_config next;
            int ok = file ? inputd_config_load(&next, file, error, sizeof(error)) : 0;
            if (ok < 0) {
                printf("%lld reload rejected: %s\n", pass, error);
                continue;
            }
            inputd_engine_reload(&e, ok > 0 ? &next : NULL, pressed, &out);
            print_events(&out, pass);
        } else if (!strcmp(word, "replace") || !strcmp(word, "write") || !strcmp(word, "remove") ||
                   !strcmp(word, "hup") || !strcmp(word, "tick")) {
            if (!config_path) { fprintf(stderr, "%s:%d: %s needs --config\n", path, lineno, word); status = EXIT_USAGE; goto done; }
            if (!strcmp(word, "hup")) {
                hup = 1;
            } else if (!strcmp(word, "tick")) {
                int reloaded = reload_profile(config_path, &stamp, hup, &e, &config, &loaded, pressed, &out, error, sizeof(error));
                hup = 0;
                if (reloaded < 0) printf("%lld reload rejected: %s\n", pass, error);
                print_events(&out, pass);
                if (reloaded > 0) printf("%lld profile %s\n", pass, loaded > 0 ? config.active_profile : "stock");
            } else if (!strcmp(word, "remove")) {
                if (unlink(config_path) != 0 && errno != ENOENT) {
                    fprintf(stderr, "%s:%d: cannot remove %s\n", path, lineno, config_path); status = EXIT_USAGE; goto done;
                }
            } else {
                char *file = strtok(NULL, " \t\r\n");
                if (!file || replay_write(file, config_path, !strcmp(word, "replace")) < 0) {
                    fprintf(stderr, "%s:%d: cannot %s %s\n", path, lineno, word, file ? file : "(no file)"); status = EXIT_USAGE; goto done;
                }
            }
        } else if (!strcmp(word, "stop")) {
            inputd_engine_neutral(&e, &out);
            print_events(&out, pass);
        } else {
            fprintf(stderr, "%s:%d: unknown command %s\n", path, lineno, word);
            status = EXIT_USAGE;
            goto done;
        }
    }
done:
    fclose(f);
    return status;
}

static uint32_t sample(struct inputd_gpio *g, int *valid) {
    uint32_t pressed = 0;
    *valid = 1;
    for (int bit = 0; bit < INPUTD_BITS; ++bit) {
        int gpio = inputd_key_io[bit];
        if (gpio < 0) continue;
        int level = inputd_gpio_level(g, gpio);
        if (level < 0) *valid = 0;
        else if (level == 0) pressed |= 1u << bit;
    }
    return pressed;
}

static int read_gpio(long duration_ms) {
    struct inputd_gpio g;
    if (inputd_gpio_open(&g, 1) < 0) { perror("/dev/mem"); return EXIT_GPIO; }
    printf("mux:");
    for (int bit = 0; bit < INPUTD_BITS; ++bit)
        if (inputd_key_io[bit] >= 0) printf(" %d=%d", inputd_key_io[bit], inputd_gpio_mux(&g, inputd_key_io[bit]));
    printf("\n");
    long long start = now_ms();
    int valid;
    uint32_t last = sample(&g, &valid);
    printf("0 pressed=%08x valid=%d\n", last, valid);
    fflush(stdout);
    while (now_ms() - start < duration_ms) {
        usleep(SCAN_US);
        uint32_t pressed = sample(&g, &valid);
        if (pressed == last) continue;
        for (int bit = 0; bit < INPUTD_BITS; ++bit) {
            uint32_t b = 1u << bit;
            if ((pressed ^ last) & b)
                printf("%lld gpio %d bit %d %s\n", now_ms() - start, inputd_key_io[bit], bit, pressed & b ? "pressed" : "released");
        }
        fflush(stdout);
        last = pressed;
    }
    inputd_gpio_close(&g);
    return 0;
}

static int write_text(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

// FN is a sysfs GPIO, exported as the stock daemon does. Returns 0, 1, or -1.
static int read_fn(void) {
    FILE *f = fopen(FN_VALUE, "rb");
    if (!f) return -1;
    int value = -1;
    if (fscanf(f, "%d", &value) != 1) value = -1;
    fclose(f);
    return value;
}

static void read_markers(struct inputd_markers *m) {
    m->turbo = m->alt = 0;
    for (int i = 0; i < INPUTD_TURBOS; ++i)
        if (exists(inputd_turbo_markers[i].path)) m->turbo |= 1u << inputd_turbo_markers[i].bit;
    for (int i = 0; i < INPUTD_ALTS; ++i)
        if (exists(inputd_alt_markers[i].path)) m->alt |= 1u << inputd_alt_markers[i].bit;
    m->no_dpad = exists(INPUTD_NO_DPAD);
    m->dpad2axis = exists(INPUTD_DPAD2AXIS);
    m->rotate = exists(INPUTD_ROTATE_270);
}

// The stock backlight workaround: a suspend marker left while the screen is lit is removed.
static void suspend_workaround(void) {
    if (!exists(INPUTD_SUSPEND)) return;
    int fd = open("/dev/disp", O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    unsigned long args[4] = {0, 0, 0, 0};
    int brightness = ioctl(fd, DISP_LCD_GET_BRIGHTNESS, args);
    close(fd);
    if (brightness > 0) {
        puts("inputd: suspend marker with the backlight on, removing it");
        unlink(INPUTD_SUSPEND);
    }
}

struct health {
    const char *path;
    const char *power_state;
    const char *power_source;
    long long started_ms;
    unsigned long long scans, suspended_scans, events, write_errors, mux_fixes;
    // game_power: 1 when a GameSwitcher endpoint was given, so a session may claim POWER.
    int ready, power_marker_pending, power_grabbed, game_power;
};

static void write_health(const struct health *h, const char *state) {
    char tmp[300], text[768];
    snprintf(tmp, sizeof(tmp), "%s.tmp", h->path);
    snprintf(text, sizeof(text),
             "state=%s\npid=%d\nuptime_ms=%lld\nscans=%llu\nsuspended_scans=%llu\nevents=%llu\nwrite_errors=%llu\nmux_fixes=%llu\npower_state=%s\npower_source=%s\npower_marker_pending=%d\npower_grabbed=%d\ngame_power=%d\n",
             state, (int)getpid(), now_ms() - h->started_ms, h->scans, h->suspended_scans, h->events, h->write_errors, h->mux_fixes,
             h->power_state ? h->power_state : "unknown", h->power_source ? h->power_source : "waiting",
             h->power_marker_pending, h->power_grabbed, h->game_power);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
    rename(tmp, h->path);
}

static int set_mux(struct inputd_gpio *g) {
    int fixed = 0;
    for (int bit = 0; bit < INPUTD_BITS; ++bit) {
        int gpio = inputd_key_io[bit];
        if (gpio < 0 || inputd_gpio_mux(g, gpio) == 0) continue;
        inputd_gpio_input(g, gpio);
        ++fixed;
    }
    return fixed;
}

static int run(const char *config_path, const char *health_path, long run_for_ms, long fail_write_after_ms,
               const char *game_power_directory) {
#ifndef BRICKOS_GAMESWITCHER_POWER
    if (game_power_directory) { fputs("inputd: game POWER is unavailable in this build\n",stderr); return EXIT_USAGE; }
#else
    struct gs_power_client game_client={.fd=-1};
    char power_nonce[33]={0};
    if (game_power_directory) {
        if (gs_power_client_init(&game_client,game_power_directory)) return EXIT_USAGE;
        unsigned char random[16]; int random_fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
        if (random_fd<0) return EXIT_USAGE;
        ssize_t bytes=read(random_fd,random,sizeof(random)); close(random_fd);
        if (bytes!=sizeof(random)) return EXIT_USAGE;
        for (int i=0;i<16;++i) snprintf(power_nonce+i*2,3,"%02x",random[i]);
    }
    int game_valid=0,game_press=0;
#endif
    struct inputd_config config;
    char error[256];
    // Taken before the load, so a write between the two is reloaded at the next tick.
    struct inputd_config_stamp config_stamp;
    inputd_config_stamp(config_path, &config_stamp);
    int loaded = inputd_config_load(&config, config_path, error, sizeof(error));
    if (loaded < 0) {
        fprintf(stderr, "inputd: invalid profile, not starting: %s\n", error);
        return EXIT_CONFIG;
    }
    struct inputd_gpio g;
    if (inputd_gpio_open(&g, 0) < 0) { perror("inputd: /dev/mem"); return EXIT_GPIO; }
    set_mux(&g);
    int valid;
    uint32_t pressed = sample(&g, &valid);
    if (!valid) { fputs("inputd: GPIO sample failed\n", stderr); inputd_gpio_close(&g); return EXIT_GPIO; }
    if (!exists("/sys/class/gpio/gpio243")) write_text(FN_EXPORT, "243");
    write_text(FN_DIRECTION, "in");
    int fd = inputd_uinput_create(error, sizeof(error));
    if (fd < 0) { fprintf(stderr, "inputd: %s\n", error); inputd_gpio_close(&g); return EXIT_UINPUT; }
    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);
    signal(SIGHUP, on_reload);
    signal(SIGPIPE, SIG_IGN);
    static struct inputd_engine e;
    inputd_engine_init(&e, loaded > 0 ? &config : NULL);
    struct inputd_power_source power_source;
    inputd_power_source_init(&power_source);
    struct inputd_power_engine power_engine;
    inputd_power_engine_init(&power_engine, 0);
    int power_held = 0, power_source_ready = 0;
    int mainui_active = 0, power_owner = 0, proc_error_reported = 0;
    long long power_retry_at = now_ms();
    long long power_owner_poll_at = 0;
    struct health h = {
        .path = health_path,
        .power_state = inputd_power_state_name(power_engine.state),
        .power_source = "waiting",
        .started_ms = now_ms(),
        .game_power = game_power_directory != NULL
    };
    printf("BrickOS trimui_inputd %s, profile %s\n", INPUTD_VERSION, loaded > 0 ? config.active_profile : "stock");
    fflush(stdout);
    long long next_tick = now_ms() + 1000;
    int fn_last = -1, grab_last = 0, ticks = 0, was_suspended = 0, status = 0;
    while (!stop_requested) {
        if (run_for_ms > 0 && now_ms() - h.started_ms >= run_for_ms) break;
        pressed = sample(&g, &valid);
        int suspended = exists(INPUTD_SUSPEND);
        long long power_now = now_ms();
        int io_test = exists(INPUTD_IO_TEST_MARKER);
        if (power_now >= power_owner_poll_at) {
#ifdef BRICKOS_GAMESWITCHER_POWER
            int previous=game_valid;
            game_valid=game_power_directory && gs_power_client_poll(&game_client,(uint64_t)power_now);
            if (game_valid!=previous) {
                printf("inputd: supported-game POWER %s\n",game_valid?"claimed":"released"); fflush(stdout);
                uint64_t sequence=power_engine.sequence;
                inputd_power_engine_init(&power_engine,power_held);
                power_engine.sequence=sequence;
            }
#endif
            int mainui = inputd_power_mainui_active("/proc");
            // Only BrickOS's OSD consumes the power-hold marker. With the official OSD,
            // or a fallback to it, MainUI keeps POWER for its own dialog.
            int osd = mainui > 0 ? inputd_power_brickos_osd_active("/proc") : 0;
            if (mainui >= 0 && osd >= 0) {
                // 0: stock keymon (no MainUI), 1: BrickOS, 2: MainUI's own dialog.
                int owner = !mainui ? 0 : osd ? 1 : 2;
                if (owner != power_owner) {
                    printf("inputd: MainUI %s; POWER %s\n", mainui ? "active" : "inactive",
                           owner == 1 ? "claimed by BrickOS" :
                           owner == 2 ? "left to MainUI (no BrickOS OSD)" : "yielded to stock keymon");
                    fflush(stdout);
                    power_owner = owner;
                }
                mainui_active = owner == 1;
                proc_error_reported = 0;
            } else if (!proc_error_reported) {
                fprintf(stderr, "inputd: cannot inspect /proc for MainUI; leaving POWER to stock input: %s\n",
                        strerror(errno));
                proc_error_reported = 1;
                mainui_active = 0;
                power_owner = 0;
            }
            power_owner_poll_at = power_now + POWER_OWNER_POLL_MS;
        }
#ifdef BRICKOS_GAMESWITCHER_POWER
#define POWER_CLAIM() inputd_game_power_should_grab(io_test,mainui_active,game_valid && game_client.lease.standby!=1,power_held,power_source.grabbed,game_press)
#else
#define POWER_CLAIM() inputd_power_should_grab(io_test,mainui_active,power_held,power_source.grabbed)
#endif
        if (!power_source_ready && power_now >= power_retry_at) {
            int initially_held = 0;
            // Open passively first: an in-progress press must stay with keymon, even
            // when MainUI appears between scans.
            int found = inputd_power_source_open(&power_source, &initially_held, 0,
                                                 error, sizeof(error));
            power_retry_at = power_now + 1000;
            if (found > 0) {
                power_source_ready = 1;
                power_held = initially_held;
                uint64_t sequence=power_engine.sequence;
                inputd_power_engine_init(&power_engine, initially_held);
                power_engine.sequence=sequence;
                h.power_source = power_source.path;
                h.power_state = inputd_power_state_name(power_engine.state);
                printf("inputd: POWER source %s (KEY_POWER)\n", power_source.path);
                fflush(stdout);
            } else if (found < 0) {
                fprintf(stderr, "inputd: POWER source scan: %s\n", error);
            }
        }
        if (power_source_ready &&
            POWER_CLAIM() != power_source.grabbed &&
            inputd_power_source_set_grab(&power_source,
                                         POWER_CLAIM(),
                                         error, sizeof(error)) < 0) {
            fprintf(stderr, "inputd: POWER source ownership failed: %s\n", error);
            inputd_power_source_close(&power_source);
            power_source_ready = 0;
            power_held = 0;
            h.power_source = "waiting";
            power_retry_at = power_now + 1000;
            inputd_power_engine_update(&power_engine, (uint64_t)power_now, 0, io_test, 0);
        }
        if (power_source_ready && inputd_power_source_read(&power_source, &power_held, error, sizeof(error)) < 0) {
            fprintf(stderr, "inputd: POWER source lost: %s\n", error);
            power_source_ready = 0;
            power_held = 0;
            h.power_source = "waiting";
            power_retry_at = power_now + 1000;
            inputd_power_engine_update(&power_engine, (uint64_t)power_now, 0, io_test, 0);
        }
        // If IO-test ownership just ended, the fresh read above lets us reclaim
        // the device on the release edge without splitting a press from release.
        if (power_source_ready &&
            POWER_CLAIM() != power_source.grabbed &&
            inputd_power_source_set_grab(&power_source,
                                         POWER_CLAIM(),
                                         error, sizeof(error)) < 0) {
            fprintf(stderr, "inputd: POWER source ownership failed: %s\n", error);
            inputd_power_source_close(&power_source);
            power_source_ready = 0;
            power_held = 0;
            h.power_source = "waiting";
            power_retry_at = power_now + 1000;
            inputd_power_engine_update(&power_engine, (uint64_t)power_now, 0, io_test, 0);
        }
        if (power_source_ready) {
            int pending = exists(INPUTD_POWER_MARKER);
#ifdef BRICKOS_GAMESWITCHER_POWER
            if (game_valid || game_press) {
                if (game_valid && power_held && !power_engine.held && power_source.grabbed) game_press=1;
                enum inputd_game_power_action action=inputd_power_engine_update_game(&power_engine,(uint64_t)power_now,
                    power_held,io_test || !game_valid || !power_source.grabbed || game_client.lease.standby,
                    pending || game_client.pending || game_client.lease.busy);
                if (action) {
                    char transaction[33]={0};
                    int result=inputd_power_transaction(&power_engine,power_nonce,transaction);
                    if (!result) result=gs_power_client_request(&game_client,
                        action==INPUTD_GAME_POWER_SHORT?GS_POWER_SHORT:GS_POWER_LONG,transaction,(uint64_t)power_now);
                    inputd_power_engine_publish_result(&power_engine,result? -1:1);
                    fprintf(stderr,"inputd: game POWER %s transaction=%s queued=%d\n",
                        action==INPUTD_GAME_POWER_SHORT?"short":"long",transaction,!result);
                }
                if (!power_held) game_press=0;
            } else
#endif
            if (inputd_power_engine_update(&power_engine, (uint64_t)power_now,
                                           power_held, io_test || !mainui_active, pending)) {
                int published = inputd_power_publish_marker(INPUTD_POWER_DIR);
                inputd_power_engine_publish_result(&power_engine, published);
                if (published < 0)
                    fprintf(stderr, "inputd: cannot publish POWER request: %s\n", strerror(errno));
                else if (published > 0)
                    fprintf(stderr, "inputd: POWER hold exceeded %d ms; request published\n", INPUTD_POWER_HOLD_MS);
            }
        }
#ifdef BRICKOS_GAMESWITCHER_POWER
        if (game_valid && power_source_ready && !power_source.grabbed && !power_held)
            (void)gs_power_client_yielded(&game_client,(uint64_t)power_now);
        if (game_valid && power_source_ready && power_source.grabbed && !power_held)
            (void)gs_power_client_reclaimed(&game_client,(uint64_t)power_now);
#endif
        h.power_state = inputd_power_state_name(power_engine.state);
        h.power_marker_pending = exists(INPUTD_POWER_MARKER);
        h.power_grabbed = power_source_ready && power_source.grabbed;
        struct inputd_events out = {0};
        inputd_engine_scan(&e, pressed, suspended, &out);
        int injected = fail_write_after_ms > 0 && now_ms() - h.started_ms >= fail_write_after_ms;
        if (out.count && (injected || inputd_uinput_write(fd, &out) < 0)) {
            ++h.write_errors;
            fprintf(stderr, "inputd: event write failed: %s\n", injected ? "injected by --fail-write-after-ms" : strerror(errno));
            status = EXIT_WRITE;
            break;
        }
        h.events += (unsigned long long)out.count;
        ++h.scans;
        if (suspended) ++h.suspended_scans;
        if (was_suspended && !suspended) h.mux_fixes += (unsigned long long)set_mux(&g);
        was_suspended = suspended;
        if (!h.ready) {
            h.ready = 1;
            write_health(&h, "ready");
        }
        if (now_ms() >= next_tick) {
            next_tick += 1000;
            out.count = 0;
            int fn = read_fn();
            if (fn != fn_last && fn >= 0) inputd_engine_fn(&e, fn, &out);
            fn_last = fn;
            read_markers(&e.markers);
            int grab = exists(INPUTD_GRAB);
            if (grab != grab_last) inputd_engine_grab(&e, &out);
            grab_last = grab;
            if (++ticks % 4 == 0) suspend_workaround();
            h.mux_fixes += (unsigned long long)set_mux(&g);
            int requested = reload_requested;
            reload_requested = 0;
            int reloaded = reload_profile(config_path, &config_stamp, requested, &e, &config, &loaded, pressed, &out, error, sizeof(error));
            if (reloaded < 0)
                fprintf(stderr, "inputd: reload rejected, keeping profile %s: %s\n", loaded > 0 ? config.active_profile : "stock", error);
            else if (reloaded > 0)
                printf("inputd: profile %s\n", loaded > 0 ? config.active_profile : "stock");
            if (reloaded) fflush(stdout);
            injected = fail_write_after_ms > 0 && now_ms() - h.started_ms >= fail_write_after_ms;
            if (out.count && (injected || inputd_uinput_write(fd, &out) < 0)) {
                ++h.write_errors;
                fprintf(stderr, "inputd: event write failed: %s\n", injected ? "injected by --fail-write-after-ms" : strerror(errno));
                status = EXIT_WRITE;
                break;
            }
            h.events += (unsigned long long)out.count;
            write_health(&h, "ready");
        }
        usleep(SCAN_US);
    }
    struct inputd_events out = {0};
    inputd_engine_neutral(&e, &out);
    if (status == 0) inputd_uinput_write(fd, &out);
    inputd_uinput_destroy(fd);
    inputd_power_source_close(&power_source);
    h.power_grabbed=0;
#ifdef BRICKOS_GAMESWITCHER_POWER
    if (game_power_directory) gs_power_client_close(&game_client);
#endif
    inputd_gpio_close(&g);
    write_health(&h, status ? "failed" : "stopped");
    return status;
}

static int usage(void) {
    fputs("usage: trimui_inputd [--run] [--config FILE] [--health FILE] [--run-for-ms MS] [--fail-write-after-ms MS]\n"
          "       opt-in GameSwitcher build: --game-power PRIVATE_DIRECTORY\n"
          "       trimui_inputd --check-config [FILE]\n"
          "       trimui_inputd --replay FILE [--config FILE]\n"
          "       trimui_inputd --read-gpio MS\n"
          "       trimui_inputd --version\n", stderr);
    return EXIT_USAGE;
}

int main(int argc, char **argv) {
    BRICKOS_HANDLE_VERSION(argc, argv);
    const char *config_path = INPUTD_CONFIG_PATH, *health_path = HEALTH_PATH, *replay_path = NULL,*game_power_directory=NULL;
    long run_for_ms = 0, read_ms = 0, fail_write_after_ms = 0;
    int check = 0;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--version")) { printf("BrickOS trimui_inputd %s\n", INPUTD_VERSION); return 0; }
        else if (!strcmp(a, "--run")) continue;
        else if (!strcmp(a, "--check-config")) { check = 1; if (next && next[0] != '-') { config_path = next; ++i; } }
        else if (!strcmp(a, "--config") && next) { config_path = next; ++i; }
        else if (!strcmp(a, "--health") && next) { health_path = next; ++i; }
        else if (!strcmp(a,"--game-power") && next) { game_power_directory=next; ++i; }
        else if (!strcmp(a, "--run-for-ms") && next) { run_for_ms = strtol(next, NULL, 10); ++i; }
        else if (!strcmp(a, "--fail-write-after-ms") && next) { fail_write_after_ms = strtol(next, NULL, 10); ++i; }
        else if (!strcmp(a, "--replay") && next) { replay_path = next; ++i; }
        else if (!strcmp(a, "--read-gpio") && next) { read_ms = strtol(next, NULL, 10); ++i; }
        else return usage();
    }
    if (check) return check_config(config_path);
    if (replay_path) return replay(replay_path, strcmp(config_path, INPUTD_CONFIG_PATH) ? config_path : NULL);
    if (read_ms > 0) return read_gpio(read_ms);
    return run(config_path, health_path, run_for_ms, fail_write_after_ms,game_power_directory);
}

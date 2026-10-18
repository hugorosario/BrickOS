#define _POSIX_C_SOURCE 200809L
#include "daemon.h"
#include "display.h"
#include "input.h"
#include "music.h"
#include "panel.h"
#include "panel_render.h"
#include "render.h"
#include "state.h"
#include "theme.h"
#include "toast.h"
#include "widgets.h"
#include "../common/version.h"
BRICKOS_DECLARE_COMPONENT("osdd", BRICKOS_VERSION_OSDD);

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;
static void stop_on_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static uint64_t monotonic_us(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static uint64_t monotonic_ms(void) {
    return monotonic_us() / 1000;
}

// --run-for-ms ends a bounded run; --health names the file the launcher's supervisor watches.
static int parse_options(int argc, char **argv, uint64_t *run_ms, const char **health_path) {
    *run_ms = 0;
    *health_path = NULL;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) return -1;
        const char *value = argv[index + 1];
        if (strcmp(argv[index], "--run-for-ms") == 0 && *run_ms == 0) {
            char *end = NULL;
            unsigned long long ms = strtoull(value, &end, 10);
            if (end == value || *end != 0 || ms < 1000 || ms > 300000) return -1;
            *run_ms = (uint64_t)ms;
        } else if (strcmp(argv[index], "--health") == 0 && *health_path == NULL && value[0] != 0) {
            *health_path = value;
        } else {
            return -1;
        }
    }
    return 0;
}

#define HEALTH_INTERVAL_MS 1000

// The health file: key=value lines, replaced by rename once a second while the loop runs.
// loops= counts the loop's iterations, so a stopped or stuck loop leaves it unchanged, and the
// supervisor then ends the daemon.
struct health {
    const char *path;
    uint64_t start_ms;
    uint64_t next_ms;
    unsigned long long loops;
    int failed;                 // A failed write is logged once.
};

static void write_health(struct health *health, const char *state, uint64_t now_ms) {
    if (health->path == NULL) return;
    char temporary[512];
    FILE *file = NULL;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", health->path) < (int)sizeof(temporary))
        file = fopen(temporary, "w");
    int written = file != NULL &&
        fprintf(file, "state=%s\npid=%ld\nuptime_ms=%llu\nloops=%llu\n", state, (long)getpid(),
                (unsigned long long)(now_ms - health->start_ms), health->loops) > 0;
    if (file != NULL && fclose(file) != 0) written = 0;
    if (written && rename(temporary, health->path) == 0) return;
    if (file != NULL) unlink(temporary);
    if (!health->failed) fprintf(stderr, "health %s: %s\n", health->path, strerror(errno));
    health->failed = 1;
}

// The devices behind the daemon's loop.
struct devices {
    struct osdd_renderer renderer;
    struct osdd_display display;
    struct osdd_toast_reader reader;
    struct osdd_input input;
    struct osdd_music_reader music;
    struct osdd_music_sender music_commands;
    struct osdd_widget_runner widgets;
    struct osdd_theme theme;    // The frontend's optional toast theme profile.
    uint64_t start_ms;
    int trace;
    int want_grab;              // The panel's grab, reapplied when the input node returns.
    int modal_grab;              // The power dialog owns input independently of the panel.
    int input_missing;          // Logged once until the node is found again.
    int keymon_memory;          // keymon's shared memory exists.
    uint64_t next_input_open_ms;
    pid_t poweroff_pid;
    // The command the power worker runs: poweroff or reboot.
    const char *power_command;
    int poweroff_started;
    int poweroff_finished;
    int poweroff_result;
};

// Draws the visible slots in index order, as the official osd_msg_update does.
static int render_toasts(struct osdd_renderer *renderer,
                         const struct osdd_toast_state *state,
                         char *error, size_t error_size) {
    if (osdd_renderer_clear(renderer, error, error_size) < 0) return -1;
    for (int index = 0; index < OSDD_TOAST_SLOTS; ++index) {
        const struct osdd_toast_slot *slot = &state->slots[index];
        if (osdd_toast_slot_visible(slot) &&
            osdd_renderer_toast(renderer, &slot->value, error, error_size) < 0)
            fprintf(stderr, "toast render slot=%d id=%s: %s\n", index, slot->value.id, error);
    }
    return 0;
}

static int read_status(const char *path, char *text, size_t capacity) {
    FILE *file = fopen(path, "r");
    if (file == NULL) return -1;
    size_t count = fread(text, 1, capacity - 1, file);
    int finished = feof(file);
    fclose(file);
    if (count == 0 || !finished) return -1;
    text[count] = 0;
    while (count > 0 && isspace((unsigned char)text[count - 1])) text[--count] = 0;
    return count > 0 ? 0 : -1;
}

static int read_number(const char *path, int *value) {
    char text[32], extra;
    return read_status(path, text, sizeof(text)) == 0 &&
           sscanf(text, "%d%c", value, &extra) == 1 ? 0 : -1;
}

static int read_fraction(const char *path, int *value, int *maximum) {
    char text[32], extra;
    return read_status(path, text, sizeof(text)) == 0 &&
           sscanf(text, "%d/%d%c", value, maximum, &extra) == 2 &&
           *maximum > 0 && *value >= 0 && *value <= *maximum ? 0 : -1;
}

static int read_panel_status(struct osdd_panel_status *status) {
    memset(status, 0, sizeof(*status));
    return read_number("/tmp/trimui_osd/toggle_led/status", &status->led) == 0 &&
           read_number("/tmp/trimui_osd/toggle_rumble/status", &status->motor) == 0 &&
           read_number("/tmp/trimui_osd/toggle_wifi/status", &status->wifi) == 0 &&
           read_number("/tmp/trimui_osd/toggle_bt/status", &status->bluetooth) == 0 &&
           read_fraction("/tmp/trimui_osd/slider_volume/status", &status->volume,
                         &status->volume_max) == 0 &&
           read_fraction("/tmp/trimui_osd/slider_backlight/status", &status->backlight,
                         &status->backlight_max) == 0 &&
           read_status("/tmp/trimui_osd/toggle_cpu/curfreq", status->cpu,
                       sizeof(status->cpu)) == 0 &&
           read_status("/tmp/trimui_osd/toggle_temperature/temperature",
                       status->temperature, sizeof(status->temperature)) == 0 ? 0 : -1;
}

// With music_only, only the music tile is drawn again; the rest of the renderer's frame
// must still be the previous panel frame.
// look is the BrickOS look while BrickOS is selected, or NULL for the stock panel.
static int render_panel(struct osdd_renderer *renderer,
                        const struct osdd_panel_state *panel,
                        const struct osdd_panel_status *status,
                        const struct osdd_music_reader *music, int music_only,
                        const struct osdd_look *look, char *error, size_t error_size) {
    struct osdd_panel_visual values = {
        .led_on = status->led, .motor_on = status->motor,
        .wifi_on = status->wifi, .bluetooth_on = status->bluetooth,
        .volume = status->volume, .volume_max = status->volume_max,
        .backlight = status->backlight, .backlight_max = status->backlight_max,
        .cpu_text = status->cpu, .temperature_text = status->temperature,
        .music_frame = music->has_frame ? music->frame : NULL,
        .look = look,
    };
    SDL_Rect tile = osdd_panel_music_rect();
    if (music_only) SDL_SetClipRect(renderer->frame, &tile);
    int result = osdd_renderer_panel(renderer, panel, &values,
                                     "/usr/trimui/osd", error, error_size);
    if (music_only) SDL_SetClipRect(renderer->frame, NULL);
    return result;
}

#define PANEL_SHOW_REQUEST "/tmp/show_osdd"
#define PANEL_HIDE_REQUEST "/tmp/hide_osdd"
#define PANEL_SIGNAL "/tmp/trimui_osd/osdd_show_up"

// Consumes one of keymon's empty panel request files, as the official daemon does.
static int consume_request(const char *path) {
    if (access(path, F_OK) != 0) return 0;
    if (unlink(path) < 0 && errno != ENOENT) fprintf(stderr, "remove %s: %s\n", path, strerror(errno));
    return 1;
}

// The official daemon keeps an empty, world-writable osdd_show_up while its panel is up.
static void set_panel_signal(int enabled) {
    if (!enabled) {
        if (unlink(PANEL_SIGNAL) < 0 && errno != ENOENT)
            fprintf(stderr, "remove %s: %s\n", PANEL_SIGNAL, strerror(errno));
        return;
    }
    int fd = open(PANEL_SIGNAL, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0666);
    if (fd < 0 || fchmod(fd, 0666) < 0)
        fprintf(stderr, "create %s: %s\n", PANEL_SIGNAL, strerror(errno));
    if (fd >= 0) close(fd);
}

static uint64_t io_now_us(void *context) {
    (void)context;
    return monotonic_us();
}

// TRIMUI Player1 comes from trimui_inputd, which starts after an early boot start and
// is restarted before some emulators, so a missing node is retried every 500 ms.
static int io_poll_input(void *context, enum osdd_panel_button *buttons, size_t capacity,
                         size_t *count, char *error, size_t error_size) {
    struct devices *devices = context;
    *count = 0;
    if (devices->input.fd < 0) {
        uint64_t now = monotonic_ms();
        if (now < devices->next_input_open_ms) return 0;
        devices->next_input_open_ms = now + 500;
        if (osdd_input_open(&devices->input, error, error_size) < 0) {
            if (!devices->input_missing) fprintf(stderr, "OSD input: %s; waiting for it\n", error);
            devices->input_missing = 1;
            return 0;
        }
        fprintf(stderr, "OSD input: TRIMUI Player1 opened\n");
        devices->input_missing = 0;
        if ((devices->want_grab || devices->modal_grab) &&
            osdd_input_grab(&devices->input, 1, error, error_size) < 0)
            fprintf(stderr, "OSD input grab: %s\n", error);
    }
    int result = osdd_input_poll(&devices->input, buttons, capacity, count, error, error_size);
    if (result == 0 && devices->input.fd < 0) {
        fprintf(stderr, "OSD input: TRIMUI Player1 disappeared; reopening\n");
        devices->next_input_open_ms = 0;
    }
    return result;
}

static void io_reset_keys(void *context) {
    struct devices *devices = context;
    osdd_input_reset_keys(&devices->input);
}

static int io_consume_request(void *context, int hide) {
    (void)context;
    return consume_request(hide ? PANEL_HIDE_REQUEST : PANEL_SHOW_REQUEST);
}

#define POWER_HOLD_REQUEST "/tmp/brickos/power-hold"
#define POWEROFF_FLAG "/tmp/poweroff_flag"

static int io_consume_power_request(void *context, char *error, size_t error_size) {
    (void)context;
    struct stat st;
    if (lstat(POWER_HOLD_REQUEST, &st) < 0) {
        if (errno == ENOENT) return 0;
        snprintf(error, error_size, "inspect %s: %s", POWER_HOLD_REQUEST, strerror(errno));
        return -1;
    }
    if (unlink(POWER_HOLD_REQUEST) < 0) {
        if (errno == ENOENT) return 0;
        snprintf(error, error_size, "remove %s: %s", POWER_HOLD_REQUEST, strerror(errno));
        return -1;
    }
    return 1;
}

static int io_poweroff_flag(void *context) {
    (void)context;
    struct stat st;
    if (lstat(POWEROFF_FLAG, &st) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

// One worker for the chosen action, whichever it is: the firmware's reboot or poweroff.
static int io_start_power(void *context, enum osdd_power_action action, char *error, size_t error_size) {
    struct devices *devices = context;
    if (devices->poweroff_started) return 0;
    devices->poweroff_started = 1;
    devices->power_command = action == OSDD_POWER_ACTION_REBOOT ? "reboot" : "poweroff";
    fprintf(stderr, "power command: %s\n", devices->power_command);
    pid_t child = fork();
    if (child < 0) {
        devices->poweroff_finished = 1;
        devices->poweroff_result = -1;
        snprintf(error, error_size, "fork %s: %s", devices->power_command, strerror(errno));
        return -1;
    }
    if (child == 0) {
        execlp(devices->power_command, devices->power_command, (char *)NULL);
        _exit(127);
    }
    devices->poweroff_pid = child;
    return 0;
}

static int io_poll_poweroff(void *context, char *error, size_t error_size) {
    struct devices *devices = context;
    if (devices->poweroff_finished) {
        if (devices->poweroff_result < 0) {
            snprintf(error, error_size, "%s worker failed", devices->power_command);
            return -1;
        }
        return 1;
    }
    if (devices->poweroff_pid <= 0) {
        snprintf(error, error_size, "%s worker has no child process", devices->power_command);
        return -1;
    }
    int status;
    pid_t result = waitpid(devices->poweroff_pid, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR)) return 0;
    devices->poweroff_finished = 1;
    devices->poweroff_pid = -1;
    if (result < 0) {
        devices->poweroff_result = -1;
        snprintf(error, error_size, "wait for %s: %s", devices->power_command, strerror(errno));
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        devices->poweroff_result = 1;
        return 1;
    }
    devices->poweroff_result = -1;
    if (WIFEXITED(status)) snprintf(error, error_size, "%s exited with status %d", devices->power_command, WEXITSTATUS(status));
    else if (WIFSIGNALED(status)) snprintf(error, error_size, "%s killed by signal %d", devices->power_command, WTERMSIG(status));
    else snprintf(error, error_size, "%s ended unexpectedly", devices->power_command);
    return -1;
}

// Looks at the profile and the frontend, at most once per second, and logs a profile change.
static void tick_theme(struct devices *devices, uint64_t now_ms) {
    if (osdd_theme_tick(&devices->theme, now_ms)) {
        if (devices->theme.loader.current != NULL) fputs("OSD theme profile: in use\n", stderr);
        else fprintf(stderr, "OSD theme profile: not in use: %s\n", devices->theme.loader.error);
    }
}

static enum osdd_poll_result io_poll_toast(void *context, struct osdd_toast *toast,
                                           char *error, size_t error_size) {
    struct devices *devices = context;
    // The profile is looked at, at most once per second, before each record is consumed.
    tick_theme(devices, monotonic_ms());
    enum osdd_poll_result result = osdd_toast_reader_poll(&devices->reader, toast, error, error_size);
    if (result == OSDD_POLL_UPDATE) osdd_theme_attach(&devices->theme, toast);
    return result;
}

static enum osdd_music_result io_poll_music(void *context, char *error, size_t error_size) {
    struct devices *devices = context;
    return osdd_music_reader_poll(&devices->music, error, error_size);
}

static int io_read_status(void *context, struct osdd_panel_status *status) {
    (void)context;
    return read_panel_status(status);
}

static int io_publish_panel(void *context, const struct osdd_panel_state *panel,
                            const struct osdd_panel_status *status, int music_only,
                            unsigned char alpha, uint64_t now_ms, char *error, size_t error_size) {
    struct devices *devices = context;
    // The panel draws the current frontend's look. A look that changed since the last frame
    // redraws the whole panel, not only the music tile.
    int material = devices->theme.material;
    struct osdd_look look = devices->theme.look;
    tick_theme(devices, now_ms);
    if (music_only && (material != devices->theme.material ||
                       (material && !osdd_look_same(&look, &devices->theme.look)))) music_only = 0;
    uint64_t render_start = monotonic_us();
    if (render_panel(&devices->renderer, panel, status, &devices->music, music_only,
                     devices->theme.material ? &devices->theme.look : NULL, error, error_size) < 0) return -1;
    uint64_t rendered = monotonic_us();
    if (osdd_display_present_panel(&devices->display, devices->renderer.frame->pixels,
                                   OSDD_FRAME_BYTES, now_ms, alpha, error, error_size) < 0)
        return -1;
    if (devices->trace)
        fprintf(stderr, "trace t=%llu panel music_only=%d render_us=%llu present_us=%llu\n",
                (unsigned long long)(now_ms - devices->start_ms), music_only,
                (unsigned long long)(rendered - render_start),
                (unsigned long long)(monotonic_us() - rendered));
    return 0;
}

static int io_panel_alpha(void *context, unsigned char alpha, char *error, size_t error_size) {
    struct devices *devices = context;
    return osdd_display_panel_alpha(&devices->display, alpha, error, error_size);
}

static int io_publish_overlay(void *context, const struct osdd_toast_state *toasts,
                              const struct osdd_power_dialog *power, int toast_visible,
                              uint64_t now_ms, char *error, size_t error_size) {
    struct devices *devices = context;
    tick_theme(devices, now_ms);
    int rendered = toast_visible ? render_toasts(&devices->renderer, toasts, error, error_size)
                                 : osdd_renderer_clear(&devices->renderer, error, error_size);
    // With BrickOS selected, the dialog takes the BrickOS look; in Stock++ it takes the profile
    // only while EmulationStation runs, as the toasts do.
    if (rendered < 0 || osdd_renderer_power(&devices->renderer, power, osdd_theme_profile(&devices->theme),
                                            devices->theme.material ? &devices->theme.look : NULL, devices->theme.portuguese,
                                            error, error_size) < 0 ||
        osdd_display_present(&devices->display, devices->renderer.frame->pixels,
                             OSDD_FRAME_BYTES, now_ms, error, error_size) < 0) return -1;
    if (devices->trace)
        fprintf(stderr, "trace t=%llu published done=%llu\n",
                (unsigned long long)(now_ms - devices->start_ms),
                (unsigned long long)(monotonic_ms() - devices->start_ms));
    return 0;
}

static int io_hide_overlay(void *context, char *error, size_t error_size) {
    struct devices *devices = context;
    return osdd_display_hide(&devices->display, error, error_size);
}

static int sync_input_grab(struct devices *devices, char *error, size_t error_size) {
    int enabled = devices->want_grab || devices->modal_grab;
    if (devices->input.fd < 0) {
        if (!enabled && devices->input.grab_path != NULL) unlink(devices->input.grab_path);
        return 0;
    }
    return osdd_input_grab(&devices->input, enabled, error, error_size);
}

static int io_grab(void *context, int enabled, char *error, size_t error_size) {
    struct devices *devices = context;
    devices->want_grab = enabled;
    return sync_input_grab(devices, error, error_size);
}

static int io_grab_modal(void *context, int enabled, char *error, size_t error_size) {
    struct devices *devices = context;
    devices->modal_grab = enabled;
    return sync_input_grab(devices, error, error_size);
}

static void io_panel_signal(void *context, int enabled) {
    (void)context;
    set_panel_signal(enabled);
}

static int io_music_command(void *context, enum osdd_music_command command,
                            char *error, size_t error_size) {
    struct devices *devices = context;
    return osdd_music_send(&devices->music_commands, command, error, error_size);
}

static int io_widget_queue(void *context, const char *directory, const char *script,
                           const char *argument) {
    struct devices *devices = context;
    return osdd_widget_queue(&devices->widgets, directory, script, argument);
}

static int io_widget_run(void *context, uint64_t now_ms) {
    struct devices *devices = context;
    return osdd_widget_run(&devices->widgets, now_ms);
}

static int io_widget_idle(void *context) {
    struct devices *devices = context;
    return osdd_widget_idle(&devices->widgets);
}

// keymon's System V shared memory, key 0x4e4f4d4b, which the widget scripts read with
// shmvar. Found once, it stays until reboot.
static int io_gameswitcher_display(void *context) {
    (void)context;
    return osdd_display_gameswitcher_marker(OSDD_GAMESWITCHER_DISPLAY_MARKER);
}

static int io_widgets_ready(void *context) {
    struct devices *devices = context;
    if (devices->keymon_memory) return 1;
    FILE *list = fopen("/proc/sysvipc/shm", "r");
    if (list == NULL) return 0;
    char line[256];
    while (fgets(line, sizeof(line), list) != NULL) {
        long long key;
        if (sscanf(line, "%lld", &key) == 1 && key == 0x4e4f4d4bLL) devices->keymon_memory = 1;
    }
    fclose(list);
    if (devices->keymon_memory) fputs("OSD widgets: keymon shared memory found\n", stderr);
    return devices->keymon_memory;
}

int main(int argc, char **argv) {
    BRICKOS_HANDLE_VERSION(argc, argv);
    uint64_t run_ms;
    const char *health_path;
    if (parse_options(argc, argv, &run_ms, &health_path) < 0) {
        fputs("usage: trimui_osdd [--run-for-ms 1000..300000] [--health FILE]\n", stderr);
        return 2;
    }
    struct sigaction action = {.sa_handler = stop_on_signal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    // A closed music FIFO then fails the write with EPIPE instead of ending the daemon.
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, NULL);

    char error[256];
    struct devices *devices = calloc(1, sizeof(*devices));
    struct osdd_daemon *daemon = calloc(1, sizeof(*daemon));
    if (devices == NULL || daemon == NULL) {
        perror("OSD allocation");
        free(devices);
        free(daemon);
        return 1;
    }
    devices->power_command = "poweroff";
    if (osdd_renderer_open(&devices->renderer, error, sizeof(error)) < 0) {
        fprintf(stderr, "OSD renderer: %s\n", error);
        free(devices);
        free(daemon);
        return 1;
    }
    if (osdd_display_open(&devices->display, error, sizeof(error)) < 0) {
        fprintf(stderr, "OSD display: %s\n", error);
        osdd_renderer_close(&devices->renderer);
        free(devices);
        free(daemon);
        return 1;
    }
    if (devices->display.profile.left_shown)
        fputs("OSD display: an earlier OSD left its layers shown; set them idle\n", stderr);
    devices->reader.path = "/tmp/trimui_osd/osd_toast_msg";
    // The official daemon creates its shared directory; an early boot start comes first.
    if (mkdir("/tmp/trimui_osd", 0755) < 0 && errno != EEXIST)
        fprintf(stderr, "create /tmp/trimui_osd: %s\n", strerror(errno));
    osdd_input_init(&devices->input);
    devices->input.grab_path = OSDD_INPUTD_GRAB_PATH;
    if (osdd_input_open(&devices->input, error, sizeof(error)) < 0) {
        fprintf(stderr, "OSD input: %s; waiting for it\n", error);
        devices->input_missing = 1;
        devices->next_input_open_ms = monotonic_ms() + 500;
    }
    if (osdd_music_reader_init(&devices->music, "/tmp/trimui_music/vfb_osd") < 0) {
        fputs("OSD music reader allocation failed\n", stderr);
        osdd_input_close(&devices->input);
        osdd_display_close(&devices->display);
        osdd_renderer_close(&devices->renderer);
        free(devices);
        free(daemon);
        return 1;
    }
    // Like the official daemon, keep musicserver's command FIFO open from startup.
    osdd_music_sender_init(&devices->music_commands, "/tmp/trimui_music/widget_cmd");
    if (osdd_music_sender_open(&devices->music_commands, error, sizeof(error)) < 0)
        fprintf(stderr, "music commands: %s; retrying on the next command\n", error);
    osdd_widget_runner_init(&devices->widgets, OSDD_WIDGET_ROOT);
    // Diagnostic timing for comparisons with the official daemon; not a stock behavior.
    devices->trace = getenv("BRICKOS_OSDD_TRACE") != NULL;
    static const struct osdd_daemon_io io_functions = {
        .now_us = io_now_us, .poll_input = io_poll_input, .reset_keys = io_reset_keys,
        .consume_request = io_consume_request,
        .consume_power_request = io_consume_power_request, .poweroff_flag = io_poweroff_flag,
        .start_power = io_start_power, .poll_poweroff = io_poll_poweroff,
        .poll_toast = io_poll_toast,
        .poll_music = io_poll_music, .read_status = io_read_status,
        .publish_panel = io_publish_panel, .panel_alpha = io_panel_alpha,
        .publish_overlay = io_publish_overlay, .hide_overlay = io_hide_overlay,
        .grab = io_grab, .grab_modal = io_grab_modal, .panel_signal = io_panel_signal,
        .music_command = io_music_command, .widget_queue = io_widget_queue,
        .widget_run = io_widget_run, .widget_idle = io_widget_idle,
        .widgets_ready = io_widgets_ready, .gameswitcher_display = io_gameswitcher_display,
    };
    struct osdd_daemon_io io = io_functions;
    io.context = devices;
    osdd_daemon_init(daemon, &io, devices->trace);
    osdd_theme_init(&devices->theme, OSDD_PROFILE_PATH, &osdd_renderer_profile_assets, &daemon->toasts);
    osdd_theme_frontend(&devices->theme, "/mnt/SDCARD/BrickOS/mainui/brickos/preferences.json",
                        "/mnt/SDCARD/BrickOS/mainui/current", "/mnt/UDISK/system.json",
                        getenv("BRICKOS_OSDD_PRESENTATION"));
    uint64_t start = monotonic_ms();
    devices->start_ms = daemon->start_ms;
    struct health health = {.path = health_path, .start_ms = start};
    int exit_code = 0;
    fprintf(stderr, "BrickOS OSD started pid=%ld run_ms=%llu\n",
            (long)getpid(), (unsigned long long)run_ms);
    while (!stop_requested) {
        uint64_t now = monotonic_ms();
        if (now == 0 || (run_ms != 0 && now - start >= run_ms)) break;
        uint64_t wait_us;
        if (osdd_daemon_step(daemon, &wait_us) < 0) {
            exit_code = 1;
            break;
        }
        ++health.loops;
        if (now >= health.next_ms) {
            write_health(&health, "running", now);
            health.next_ms = now + HEALTH_INTERVAL_MS;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = (long)wait_us * 1000};
        while (!stop_requested && nanosleep(&pause, &pause) < 0 && errno == EINTR) {}
    }
    // Leave no panel signal behind; the input close removes the grab file.
    if (daemon->presence.signal) set_panel_signal(0);
    osdd_widget_runner_close(&devices->widgets);
    osdd_music_reader_close(&devices->music);
    osdd_music_sender_close(&devices->music_commands);
    fprintf(stderr, "BrickOS OSD layer SET retries=%u\n", devices->display.set_retries);
    osdd_display_close(&devices->display);
    osdd_input_close(&devices->input);
    osdd_theme_close(&devices->theme);
    osdd_renderer_close(&devices->renderer);
    write_health(&health, exit_code ? "failed" : "stopped", monotonic_ms());
    free(daemon);
    free(devices);
    fprintf(stderr, "BrickOS OSD exit=%d\n", exit_code);
    return exit_code;
}

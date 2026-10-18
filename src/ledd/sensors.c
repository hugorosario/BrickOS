#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "sensors.h"

#include "../mainui/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int read_file(const char *path, char *text, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, text + used, size - 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
        if (used == size - 1) break;
    }
    close(fd);
    text[used] = 0;
    return used ? (int)used : -1;
}

static int read_long(const char *path, long *value) {
    char text[64], *end;
    if (read_file(path, text, sizeof(text)) < 0) return -1;
    errno = 0;
    long v = strtol(text, &end, 10);
    if (end == text || errno) return -1;
    *value = v;
    return 0;
}

static void trim(char *text) {
    size_t n = strlen(text);
    while (n && (text[n - 1] == '\n' || text[n - 1] == ' ' || text[n - 1] == '\r')) text[--n] = 0;
}

// The first supply, in name order, whose type is one of types; its folder in out.
static int supply(const char *root, const char *const *types, int type_count, char *out, size_t size) {
    char folder[512], path[1024], type[64];
    snprintf(folder, sizeof(folder), "%s" LEDD_POWER_DIR, root);
    struct dirent **entries;
    int n = scandir(folder, &entries, NULL, alphasort), found = -1;
    if (n < 0) return -1;
    for (int i = 0; i < n; ++i) {
        if (found < 0 && entries[i]->d_name[0] != '.') {
            snprintf(path, sizeof(path), "%s/%s/type", folder, entries[i]->d_name);
            if (read_file(path, type, sizeof(type)) > 0) {
                trim(type);
                for (int t = 0; t < type_count && found < 0; ++t)
                    if (strcmp(type, types[t]) == 0) {
                        snprintf(out, size, "%s/%s", folder, entries[i]->d_name);
                        found = 0;
                    }
            }
        }
        free(entries[i]);
    }
    free(entries);
    return found;
}

void ledd_sensors_battery(const char *root, struct ledd_readings *r, int capacity) {
    static const char *const battery[] = {"Battery"};
    char folder[1024], path[1100], status[64];
    long value;
    if (supply(root, battery, 1, folder, sizeof(folder)) < 0) {
        r->has_status = 0;
        if (capacity) r->has_capacity = 0;
        return;
    }
    if (capacity) {
        snprintf(path, sizeof(path), "%s/capacity", folder);
        r->has_capacity = read_long(path, &value) == 0 && value >= 0 && value <= 100;
        if (r->has_capacity) r->capacity = (int)value;
    }
    snprintf(path, sizeof(path), "%s/status", folder);
    r->has_status = read_file(path, status, sizeof(status)) > 0;
    if (!r->has_status) return;
    trim(status);
    r->status = !strcmp(status, "Charging") ? LEDD_BATTERY_CHARGING : !strcmp(status, "Full") ? LEDD_BATTERY_FULL :
                !strcmp(status, "Discharging") ? LEDD_BATTERY_DISCHARGING :
                !strcmp(status, "Not charging") ? LEDD_BATTERY_NOT_CHARGING : LEDD_BATTERY_UNKNOWN;
}

void ledd_sensors_charger(const char *root, struct ledd_readings *r) {
    static const char *const chargers[] = {"USB", "Mains"};
    char folder[1024], path[1100];
    long value;
    r->has_usb = supply(root, chargers, 2, folder, sizeof(folder)) == 0 &&
                 (snprintf(path, sizeof(path), "%s/online", folder), read_long(path, &value) == 0);
    if (r->has_usb) r->usb_online = value != 0;
}

void ledd_sensors_cpu(const char *root, struct ledd_readings *r) {
    char path[512];
    long value;
    snprintf(path, sizeof(path), "%s" LEDD_CPU_FREQ, root);
    r->has_cpu = read_long(path, &value) == 0 && value > 0;
    if (r->has_cpu) r->cpu_khz = value;
}

void ledd_sensors_temperature(const char *root, struct ledd_readings *r) {
    char path[512];
    long value;
    snprintf(path, sizeof(path), "%s" LEDD_TEMPERATURE, root);
    r->has_temperature = read_long(path, &value) == 0;
    if (r->has_temperature) r->temperature_mc = value;
}

int ledd_sensors_standby(const char *root) {
    char path[512], text[8192];
    struct stat st;
    snprintf(path, sizeof(path), "%s" LEDD_SUSPEND_MARKER, root);
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    snprintf(path, sizeof(path), "%s" LEDD_DISPLAY_STATE, root);
    if (read_file(path, text, sizeof(text)) < 0) return 0;
    // "backlight( 21)" in the display's state.
    char *p = strstr(text, "backlight("), *end;
    if (!p) return 0;
    long value = strtol(p + 10, &end, 10);
    return end != p + 10 && *end == ')' && value == 0;
}

int ledd_sensors_bricktools(const char *root) {
    char folder[512], path[1024], cmdline[4096];
    snprintf(folder, sizeof(folder), "%s/proc", root);
    DIR *proc = opendir(folder);
    if (!proc) return 0;
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(proc));) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9' || strspn(e->d_name, "0123456789") != strlen(e->d_name)) continue;
        snprintf(path, sizeof(path), "%s/%s/cmdline", folder, e->d_name);
        int n = read_file(path, cmdline, sizeof(cmdline));
        // Each argument ends with a NUL byte; one of them runs the script.
        for (int at = 0; !found && n > 0 && at < n; at += (int)strlen(cmdline + at) + 1) {
            const char *arg = cmdline + at, *base = strrchr(arg, '/');
            found = strcmp(base ? base + 1 : arg, LEDD_BRICKTOOLS_DAEMON) == 0;
        }
    }
    closedir(proc);
    return found;
}

int ledd_sensors_system_policy(const char *root) {
    static char text[16384];
    static struct mainui_json json;
    char path[512];
    snprintf(path, sizeof(path), "%s" LEDD_SYSTEM_JSON, root);
    int n = read_file(path, text, sizeof(text));
    if (n < 0 || mainui_json_parse_fitting(&json, text, (size_t)n) < 0) return -1;
    const struct mainui_json_member *policy = mainui_json_get(&json, "ledpolicy");
    if (!policy || policy->type != MAINUI_JSON_NUMBER) return -1;
    return policy->number == 1 ? 1 : 0;
}

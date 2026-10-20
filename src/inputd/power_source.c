#define _GNU_SOURCE
#include "power_source.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int process_id_name(const char *name) {
    if (name == NULL || name[0] == 0) return 0;
    for (const char *p = name; *p; ++p)
        if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

static int live_process(const char *proc_root, const char *pid, const char *name) {
    char path[PATH_MAX], comm[64] = {0}, statline[256];
    if (snprintf(path, sizeof(path), "%s/%s/comm", proc_root, pid) >= (int)sizeof(path)) return 0;
    FILE *file = fopen(path, "r");
    if (file == NULL) return 0; // The process may exit during a /proc scan.
    if (fgets(comm, sizeof(comm), file) == NULL) { fclose(file); return 0; }
    fclose(file);
    comm[strcspn(comm, "\r\n")] = 0;
    if (strcmp(comm, name) != 0) return 0;

    if (snprintf(path, sizeof(path), "%s/%s/stat", proc_root, pid) >= (int)sizeof(path)) return 0;
    file = fopen(path, "r");
    if (file == NULL) return 0;
    if (fgets(statline, sizeof(statline), file) == NULL) { fclose(file); return 0; }
    fclose(file);
    char *end = strrchr(statline, ')');
    if (end == NULL || end[1] != ' ' || end[2] == 0) return 0;
    return end[2] != 'Z' && end[2] != 'X';
}

static int live_mainui(const char *proc_root, const char *pid) {
    return live_process(proc_root, pid, "MainUI");
}

// Only BrickOS's OSD consumes the power-hold marker: the installed daemon, or the private
// copy of a development or end-to-end session. The official OSD runs from the firmware's own
// paths.
static int live_brickos_osd(const char *proc_root, const char *pid) {
    if (!live_process(proc_root, pid, "trimui_osdd")) return 0;
    char path[PATH_MAX], target[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s/exe", proc_root, pid) >= (int)sizeof(path)) return 0;
    ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length <= 0) return 0;
    target[length] = 0;
    static const char installed[] = "/mnt/SDCARD/BrickOS/osd/", session[] = "/tmp/brickos-osdd-session-",
                      e2e[] = "/tmp/brickos-e2e-";
    return strncmp(target, installed, sizeof(installed) - 1) == 0 ||
           strncmp(target, session, sizeof(session) - 1) == 0 ||
           strncmp(target, e2e, sizeof(e2e) - 1) == 0;
}

// found, when given, gets the matching process's PID.
static int any_process(const char *proc_root, int (*match)(const char *, const char *), int *found) {
    if (proc_root == NULL || proc_root[0] != '/') { errno = EINVAL; return -1; }
    DIR *directory = opendir(proc_root);
    if (directory == NULL) return -1;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            int saved = errno;
            closedir(directory);
            if (saved != 0) { errno = saved; return -1; }
            return 0;
        }
        if (process_id_name(entry->d_name) && match(proc_root, entry->d_name)) {
            if (found != NULL) *found = atoi(entry->d_name);
            closedir(directory);
            return 1;
        }
    }
}

int inputd_power_mainui_active(const char *proc_root) {
    return any_process(proc_root, live_mainui, NULL);
}

int inputd_power_brickos_osd_active(const char *proc_root) {
    return any_process(proc_root, live_brickos_osd, NULL);
}

static int watch_process(const char *proc_root, struct inputd_process_watch *watch, long long now, int rescan_ms,
                         int (*match)(const char *, const char *)) {
    if (proc_root == NULL || proc_root[0] != '/') { errno = EINVAL; return -1; }
    if (watch->pid > 0) {
        char pid[16];
        snprintf(pid, sizeof(pid), "%d", watch->pid);
        if (match(proc_root, pid)) return 1;
        // Gone or no longer a match: another one may run (MainUI's wrapper and its program).
        watch->pid = 0;
        watch->rescan_at = now;
    }
    if (now < watch->rescan_at) return 0;
    int found = 0, result = any_process(proc_root, match, &found);
    if (result == 1) watch->pid = found;
    else watch->rescan_at = now + rescan_ms;
    return result;
}

int inputd_power_mainui_watch(const char *proc_root, struct inputd_process_watch *watch, long long now,
                              int rescan_ms) {
    return watch_process(proc_root, watch, now, rescan_ms, live_mainui);
}

int inputd_power_brickos_osd_watch(const char *proc_root, struct inputd_process_watch *watch, long long now,
                                   int rescan_ms) {
    return watch_process(proc_root, watch, now, rescan_ms, live_brickos_osd);
}

#define INPUTD_KEY_WORDS ((KEY_MAX + 1 + (8 * sizeof(unsigned long) - 1)) / (8 * sizeof(unsigned long)))

static int bit_set(const unsigned long *bits, int bit) {
    return (bits[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1UL;
}

void inputd_power_source_init(struct inputd_power_source *source) {
    if (source == NULL) return;
    memset(source, 0, sizeof(*source));
    source->fd = -1;
}

static int event_name(const char *name) {
    if (strncmp(name, "event", 5) != 0 || name[5] == 0) return 0;
    for (const char *p = name + 5; *p != 0; ++p) if (*p < '0' || *p > '9') return 0;
    return 1;
}

int inputd_power_source_open(struct inputd_power_source *source, int *initially_held, int grab,
                             char *error, size_t error_size) {
    if (source == NULL || initially_held == NULL) return -1;
    if (source->fd >= 0) return 1;
    DIR *directory = opendir("/dev/input");
    if (directory == NULL) {
        if (error != NULL && error_size > 0) snprintf(error, error_size, "/dev/input: %s", strerror(errno));
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!event_name(entry->d_name)) continue;
        char path[sizeof(source->path)], name[128] = {0};
        if (snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name) >= (int)sizeof(path)) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long keys[INPUTD_KEY_WORDS] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) <= 0 || strcmp(name, "axp2202-pek") != 0 ||
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 || !bit_set(keys, KEY_POWER)) {
            close(fd);
            continue;
        }
        if (grab && ioctl(fd, EVIOCGRAB, 1) < 0) {
            int saved = errno;
            if (error != NULL && error_size > 0)
                snprintf(error, error_size, "%s EVIOCGRAB: %s", path, strerror(saved));
            close(fd);
            closedir(directory);
            errno = saved;
            return -1;
        }
        unsigned long held[INPUTD_KEY_WORDS] = {0};
        // Failure to read the initial key state disarms detection until a release arrives.
        int known = ioctl(fd, EVIOCGKEY(sizeof(held)), held) >= 0;
        *initially_held = known ? bit_set(held, KEY_POWER) : 1;
        source->fd = fd;
        source->grabbed = grab != 0;
        snprintf(source->path, sizeof(source->path), "%s", path);
        closedir(directory);
        if (error != NULL && error_size > 0) error[0] = 0;
        return 1;
    }
    closedir(directory);
    if (error != NULL && error_size > 0)
        snprintf(error, error_size, "axp2202-pek with KEY_POWER not found");
    return 0;
}

int inputd_power_source_set_grab(struct inputd_power_source *source, int grab,
                                 char *error, size_t error_size) {
    if (source == NULL || source->fd < 0) { errno = EINVAL; return -1; }
    grab = grab != 0;
    if (source->grabbed == grab) return 0;
    if (ioctl(source->fd, EVIOCGRAB, grab) < 0) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "%s EVIOCGRAB %s: %s", source->path,
                     grab ? "claim" : "release", strerror(errno));
        return -1;
    }
    source->grabbed = grab;
    if (error != NULL && error_size > 0) error[0] = 0;
    return 0;
}

static int read_state(struct inputd_power_source *source, int *held) {
    unsigned long keys[INPUTD_KEY_WORDS] = {0};
    if (ioctl(source->fd, EVIOCGKEY(sizeof(keys)), keys) < 0) return -1;
    *held = bit_set(keys, KEY_POWER);
    return 0;
}

int inputd_power_source_read(struct inputd_power_source *source, int *held,
                             char *error, size_t error_size) {
    if (source == NULL || source->fd < 0 || held == NULL) return -1;
    int changed = 0, dropped = 0;
    struct input_event events[32];
    for (;;) {
        ssize_t received = read(source->fd, events, sizeof(events));
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (received == 0 || (received < 0 && (errno == ENODEV || errno == EIO))) {
            if (error != NULL && error_size > 0) snprintf(error, error_size, "%s disappeared", source->path);
            inputd_power_source_close(source);
            return -1;
        }
        if (received < 0 || received % (ssize_t)sizeof(events[0]) != 0) {
            if (error != NULL && error_size > 0)
                snprintf(error, error_size, "%s read: %s", source->path,
                         received < 0 ? strerror(errno) : "short event");
            inputd_power_source_close(source);
            return -1;
        }
        size_t count = (size_t)received / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            const struct input_event *event = &events[i];
            if (event->type == EV_SYN && event->code == SYN_DROPPED) dropped = 1;
            if (event->type == EV_KEY && event->code == KEY_POWER &&
                (event->value == 0 || event->value == 1)) {
                int next = event->value == 1;
                if (next != *held) { *held = next; changed = 1; }
            }
        }
    }
    if (dropped) {
        int next;
        if (read_state(source, &next) < 0) {
            if (error != NULL && error_size > 0)
                snprintf(error, error_size, "%s key state: %s", source->path, strerror(errno));
            inputd_power_source_close(source);
            return -1;
        }
        if (next != *held) { *held = next; changed = 1; }
    }
    return changed;
}

void inputd_power_source_close(struct inputd_power_source *source) {
    if (source == NULL) return;
    if (source->fd >= 0) close(source->fd);
    inputd_power_source_init(source);
}

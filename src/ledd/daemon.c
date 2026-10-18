#define _POSIX_C_SOURCE 200809L
#include "daemon.h"

#include "driver.h"
#include "health.h"
#include "sensors.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void path_of(char *out, size_t size, const char *root, const char *path) { snprintf(out, size, "%s%s", root, path); }

// Replaces path with text through a temporary file and a rename.
static int replace_file(const char *path, const char *text) {
    char temporary[600];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    size_t length = strlen(text);
    ssize_t n;
    do n = write(fd, text, length);
    while (n < 0 && errno == EINTR);
    int ok = n == (ssize_t)length;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static void load_config(struct ledd_daemon *d) {
    char path[512];
    path_of(path, sizeof(path), d->root, LEDD_CONFIG_PATH);
    d->config_status = ledd_config_load(&d->config, path, NULL, d->config_error, sizeof(d->config_error));
    if (d->config_status == LEDD_CONFIG_MISSING) d->config_error[0] = 0;
}

void ledd_daemon_init(struct ledd_daemon *d, const char *root, long pid, uint64_t now, const struct ledd_random *random,
                      ledd_policy_reader keymon, void *keymon_context) {
    memset(d, 0, sizeof(*d));
    snprintf(d->root, sizeof(d->root), "%s", root ? root : "");
    d->pid = pid;
    d->start = now;
    if (random) d->random = *random;
    d->keymon = keymon;
    d->keymon_context = keymon_context;
    d->system_policy = -1;
    ledd_reconcile_init(&d->reconciler);
    ledd_config_defaults(&d->config);
    d->next_config = d->next_sensors = d->next_battery = d->next_scan = d->next_poll = d->next_health = now;
}

static int follow_theme(struct ledd_daemon *d) {
    int value;
    if (d->keymon && d->keymon(d->keymon_context, &value) == 0) return value == 1;
    // Without keymon's segment, system.json, read again only when it changes.
    char path[512];
    path_of(path, sizeof(path), d->root, LEDD_SYSTEM_JSON);
    if (ledd_config_changed(&d->system_stamp, path)) d->system_policy = ledd_sensors_system_policy(d->root);
    return d->system_policy == 1;
}

static void write_applied(struct ledd_daemon *d) {
    char path[512], text[512];
    path_of(path, sizeof(path), d->root, LEDD_APPLIED_PATH);
    if (!d->reconciler.applied) {
        if (unlink(path) != 0 && errno != ENOENT) ++d->errors;
        return;
    }
    size_t used = 0;
    for (int g = 0; g < LEDD_GROUPS; ++g) {
        const struct ledd_written *w = &d->reconciler.written[g];
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%s %d %06X %ld %ld\n", ledd_group_names[g], w->effect,
                                 w->rgb & 0xFFFFFFu, w->cycles, w->duration);
    }
    if (replace_file(path, text) != 0) ++d->errors;
}

static void apply(struct ledd_daemon *d, const struct ledd_actions *a) {
    if (a->engine_on && ledd_driver_engine_on(d->root) != 0) ++d->errors;
    for (int g = 0; g < LEDD_GROUPS; ++g) {
        if (a->write[g]) {
            if (ledd_driver_write(d->root, g, &a->values[g]) != 0) ++d->errors;
            else ++d->writes;
        }
        if (a->dark[g]) {
            if (ledd_driver_dark(d->root, g) != 0) ++d->errors;
            else ++d->writes;
        }
    }
    if (a->restore) ++d->restores;
    if (a->applied_changed) write_applied(d);
}

static void write_health(struct ledd_daemon *d, uint64_t now) {
    char path[512], text[1024];
    const char *detail = d->reconciler.reason == LEDD_WHY_INVALID_CONFIG ? d->config_error :
                         d->reconciler.reason == LEDD_WHY_NO_READING && d->missing ? d->missing : "";
    snprintf(text, sizeof(text),
             "state=%s\nmode=%s\nreason=%s\ndetail=%s\npid=%ld\nuptime_ms=%llu\nwrites=%lu\nrestores=%lu\nerrors=%lu\n",
             ledd_state_name(d->reconciler.state), ledd_mode_name(d->config.mode), ledd_reason_name(d->reconciler.reason),
             detail, d->pid, (unsigned long long)(now - d->start), d->writes, d->restores, d->errors);
    path_of(path, sizeof(path), d->root, LEDD_HEALTH_PATH);
    if (replace_file(path, text) != 0) ++d->errors;
    d->health_written = 1;
    d->health_state = d->reconciler.state;
    d->health_reason = d->reconciler.reason;
    d->health_mode = d->config.mode;
    d->next_health = now + LEDD_HEALTH_MS;
}

static uint64_t earliest(uint64_t a, uint64_t b) { return !b || (a && a < b) ? a : b; }

uint64_t ledd_daemon_tick(struct ledd_daemon *d, uint64_t now) {
    if (now >= d->next_config) {
        char path[512];
        path_of(path, sizeof(path), d->root, LEDD_CONFIG_PATH);
        if (ledd_config_changed(&d->config_stamp, path)) load_config(d);
        d->next_config = now + LEDD_CONFIG_CHECK_MS;
    }
    if (now >= d->next_battery) {
        ledd_sensors_battery(d->root, &d->readings, 1);
        d->next_battery = now + LEDD_BATTERY_MS;
    }
    if (now >= d->next_sensors) {
        // The charger state follows the battery's status too, so that is read with it.
        ledd_sensors_battery(d->root, &d->readings, 0);
        ledd_sensors_charger(d->root, &d->readings);
        ledd_sensors_cpu(d->root, &d->readings);
        ledd_sensors_temperature(d->root, &d->readings);
        d->next_sensors = now + LEDD_SENSORS_MS;
    }
    if (now >= d->next_scan) {
        d->bricktools = ledd_sensors_bricktools(d->root);
        d->next_scan = now + LEDD_SCAN_MS;
    }
    uint64_t cycle = ledd_reconcile_deadline(&d->reconciler);
    if (now >= d->next_poll || (cycle && now >= cycle)) {
        struct ledd_inputs in;
        memset(&in, 0, sizeof(in));
        if (ledd_driver_read(d->root, &in.leds) != 0 && in.leds.enable < 0) ++d->errors;
        in.follow_theme = follow_theme(d);
        in.standby = ledd_sensors_standby(d->root);
        in.bricktools = d->bricktools;
        in.config_invalid = d->config_status == LEDD_CONFIG_INVALID;
        in.mode = d->config.mode;
        d->missing = NULL;
        in.outcome = ledd_policy_pattern(&d->config, &d->readings, &d->random, &in.pattern, &d->missing);
        struct ledd_actions actions;
        ledd_reconcile_step(&d->reconciler, &in, now, &d->random, &actions);
        apply(d, &actions);
        if (now >= d->next_poll) d->next_poll = now + LEDD_POLL_MS;
    }
    if (!d->health_written || now >= d->next_health || d->health_state != d->reconciler.state ||
        d->health_reason != d->reconciler.reason || d->health_mode != d->config.mode)
        write_health(d, now);
    uint64_t next = d->next_poll;
    next = earliest(next, d->next_config);
    next = earliest(next, d->next_sensors);
    next = earliest(next, d->next_battery);
    next = earliest(next, d->next_scan);
    next = earliest(next, d->next_health);
    return earliest(next, ledd_reconcile_deadline(&d->reconciler));
}

void ledd_daemon_stop(struct ledd_daemon *d, uint64_t now) {
    struct ledd_led_state leds;
    struct ledd_actions actions;
    memset(&actions, 0, sizeof(actions));
    ledd_driver_read(d->root, &leds);
    ledd_reconcile_end(&d->reconciler, &leds, &actions);
    apply(d, &actions);
    d->reconciler.state = LEDD_STOPPED;
    d->reconciler.reason = LEDD_WHY_NONE;
    write_health(d, now);
}

int ledd_end_effects(const char *root) {
    char path[512], line[128], name[8], rgb[16];
    snprintf(path, sizeof(path), "%s" LEDD_APPLIED_PATH, root ? root : "");
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    struct ledd_led_state leds;
    ledd_driver_read(root ? root : "", &leds);
    int failed = 0;
    while (fgets(line, sizeof(line), file)) {
        struct ledd_written w;
        if (sscanf(line, "%7s %d %15s %ld %ld", name, &w.effect, rgb, &w.cycles, &w.duration) != 5) continue;
        w.rgb = (unsigned)strtoul(rgb, NULL, 16);
        for (int g = 0; g < LEDD_GROUPS; ++g)
            if (!strcmp(name, ledd_group_names[g]) && ledd_driver_shows(&leds.groups[g], &w))
                failed |= ledd_driver_dark(root ? root : "", g);
    }
    fclose(file);
    unlink(path);
    return failed ? -1 : 0;
}

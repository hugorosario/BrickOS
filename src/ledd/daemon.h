#ifndef BRICKOS_LEDD_DAEMON_H
#define BRICKOS_LEDD_DAEMON_H

#include "config.h"
#include "policy.h"
#include "reconcile.h"

#include <stdint.h>

// The LED daemon's work at each moment: the configuration, the sensors, the yield conditions,
// one reconcile step, its writes, and the health and applied-pattern records. Every path is under
// root ("" on the device); the clock is a monotonic millisecond count passed in.

#define LEDD_CONFIG_CHECK_MS 1000
#define LEDD_SENSORS_MS 2000
#define LEDD_BATTERY_MS 30000
#define LEDD_SCAN_MS 2000
#define LEDD_HEALTH_MS 5000

// keymon's shared memory, index 19 (ledpolicy): 0 with the value, -1 while the segment is absent.
typedef int (*ledd_policy_reader)(void *context, int *value);

struct ledd_daemon {
    char root[256];
    long pid;
    struct ledd_config config;
    enum ledd_config_status config_status;
    char config_error[128];
    struct ledd_config_stamp config_stamp, system_stamp;
    int system_policy;  // ledpolicy from system.json, -1 when unreadable
    struct ledd_readings readings;
    struct ledd_reconciler reconciler;
    struct ledd_random random;
    ledd_policy_reader keymon;
    void *keymon_context;
    int bricktools;
    const char *missing;  // the reading the mode lacks
    uint64_t start, next_config, next_sensors, next_battery, next_scan, next_poll, next_health;
    unsigned long writes, restores, errors;
    int health_written;
    enum ledd_state health_state;
    enum ledd_reason health_reason;
    enum ledd_mode health_mode;
};

void ledd_daemon_init(struct ledd_daemon *d, const char *root, long pid, uint64_t now, const struct ledd_random *random,
                      ledd_policy_reader keymon, void *keymon_context);
// Runs what is due at now and returns when it next needs to run.
uint64_t ledd_daemon_tick(struct ledd_daemon *d, uint64_t now);
// A stop request: the daemon's own effect ends where it still shows, the applied record goes,
// and the health says stopped.
void ledd_daemon_stop(struct ledd_daemon *d, uint64_t now);
// --end-effects, for the launcher after the daemon died: ends the pattern the applied record
// names where it still shows, and removes the record. Returns 0, or -1 on a failed write.
int ledd_end_effects(const char *root);

#endif

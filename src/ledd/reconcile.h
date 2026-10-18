#ifndef BRICKOS_LEDD_RECONCILE_H
#define BRICKOS_LEDD_RECONCILE_H

#include "driver.h"
#include "policy.h"

#include <stdint.h>

// One step of the LED daemon: from what the driver reads back, the conditions that make it
// yield, and the pattern the mode wants, the writes to make now. Pure; the clock is a
// millisecond count passed in.

#define LEDD_POLL_MS 500
#define LEDD_SETTLE_MS 1500
// keymon's low-battery warning on the shoulder lights: blink 2, red, 30000 cycles. The daemon
// never writes 30000 cycles, so its own red blink is not mistaken for it.
#define LEDD_WARNING_EFFECT 6
#define LEDD_WARNING_RGB 0xFF0000u
#define LEDD_WARNING_CYCLES 30000

enum ledd_state { LEDD_ACTIVE, LEDD_IDLE, LEDD_YIELDING, LEDD_STOPPED };

enum ledd_reason {
    LEDD_WHY_NONE,
    LEDD_WHY_DEFAULT,
    LEDD_WHY_FOLLOW_THEME,
    LEDD_WHY_LED_OFF,
    LEDD_WHY_STANDBY,
    LEDD_WHY_LOW_BATTERY,
    LEDD_WHY_BRICKTOOLS,
    LEDD_WHY_NO_CHARGER,
    LEDD_WHY_NO_READING,
    LEDD_WHY_INVALID_CONFIG,
};

struct ledd_inputs {
    struct ledd_led_state leds;  // read back now
    int follow_theme;            // ledpolicy is 1
    int standby;
    int bricktools;              // BrickTools' LED daemon runs
    int config_invalid;          // the configuration did not load; the mode is then default
    enum ledd_mode mode;
    enum ledd_policy_outcome outcome;
    struct ledd_pattern pattern;  // when outcome is LEDD_POLICY_PATTERN
};

struct ledd_reconciler {
    int applied;                            // groups may show the daemon's writes, which a stop ends
    struct ledd_pattern pattern;            // the pattern in place (random: the current colour);
                                            // effect 0 after a yield, so it is written again
    struct ledd_written written[LEDD_GROUPS];
    uint64_t foreign_since;                 // when another writer's change was first seen, or 0
    uint64_t cycle_end;                     // random colour: when the current cycle ends
    enum ledd_state state;
    enum ledd_reason reason;
};

struct ledd_actions {
    int engine_on;                     // effect_enable 1 first
    int write[LEDD_GROUPS];            // write values[g]
    struct ledd_written values[LEDD_GROUPS];
    int dark[LEDD_GROUPS];             // end the daemon's own effect: one static black cycle
    int restore;                       // the writes put the pattern back after another writer
    int applied_changed;               // the record of the applied pattern changed
};

void ledd_reconcile_init(struct ledd_reconciler *r);
void ledd_reconcile_step(struct ledd_reconciler *r, const struct ledd_inputs *in, uint64_t now,
                         const struct ledd_random *random, struct ledd_actions *out);
// Ends the daemon's own effect on the groups that still show it, and forgets it (a stop).
void ledd_reconcile_end(struct ledd_reconciler *r, const struct ledd_led_state *leds, struct ledd_actions *out);
// The next time a step must run for a random colour's next cycle, or 0.
uint64_t ledd_reconcile_deadline(const struct ledd_reconciler *r);
const char *ledd_state_name(enum ledd_state state);
const char *ledd_reason_name(enum ledd_reason reason);

#endif

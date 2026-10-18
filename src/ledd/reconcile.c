#include "reconcile.h"

#include "health.h"

#include <string.h>

void ledd_reconcile_init(struct ledd_reconciler *r) {
    memset(r, 0, sizeof(*r));
    r->state = LEDD_IDLE;
    r->reason = LEDD_WHY_DEFAULT;
}

static int same_pattern(const struct ledd_pattern *a, const struct ledd_pattern *b) {
    return a->effect == b->effect && a->duration_ms == b->duration_ms && a->random == b->random &&
           (a->random || a->rgb == b->rgb);
}

// keymon's low-battery warning, read back on the shoulder lights.
static int warning(const struct ledd_led_state *leds) {
    const struct ledd_group_state *lr = &leds->groups[LEDD_GROUP_LR];
    return lr->readable && lr->effect == LEDD_WARNING_EFFECT && lr->rgb == LEDD_WARNING_RGB &&
           lr->cycles == LEDD_WARNING_CYCLES;
}

void ledd_reconcile_end(struct ledd_reconciler *r, const struct ledd_led_state *leds, struct ledd_actions *out) {
    if (r->applied) {
        // Groups another writer has since taken are left to it.
        for (int g = 0; g < LEDD_GROUPS; ++g)
            if (ledd_driver_shows(&leds->groups[g], &r->written[g])) out->dark[g] = 1;
        out->applied_changed = 1;
    }
    r->applied = 0;
    r->foreign_since = 0;
    r->cycle_end = 0;
    memset(&r->pattern, 0, sizeof(r->pattern));
}

// One pattern on every group, in the colour given: endless cycles, or one cycle for a random
// colour, whose next cycle the deadline schedules.
static void write_all(struct ledd_reconciler *r, const struct ledd_pattern *pattern, unsigned rgb, uint64_t now,
                      struct ledd_actions *out) {
    r->pattern = *pattern;
    r->pattern.rgb = rgb;
    for (int g = 0; g < LEDD_GROUPS; ++g) {
        struct ledd_written w = {pattern->effect, rgb, pattern->random ? 1 : LEDD_CYCLES_ENDLESS, pattern->duration_ms};
        r->written[g] = out->values[g] = w;
        out->write[g] = 1;
    }
    r->applied = 1;
    r->foreign_since = 0;
    r->cycle_end = pattern->random ? now + (uint64_t)pattern->duration_ms : 0;
    out->applied_changed = 1;
}

void ledd_reconcile_step(struct ledd_reconciler *r, const struct ledd_inputs *in, uint64_t now,
                         const struct ledd_random *random, struct ledd_actions *out) {
    memset(out, 0, sizeof(*out));
    // The yield order: Follow Theme, the LED switch, standby, keymon's warning, BrickTools.
    enum ledd_reason yield = in->follow_theme ? LEDD_WHY_FOLLOW_THEME :
                             in->leds.enable == 0 ? LEDD_WHY_LED_OFF :
                             in->standby ? LEDD_WHY_STANDBY :
                             warning(&in->leds) ? LEDD_WHY_LOW_BATTERY :
                             in->bricktools ? LEDD_WHY_BRICKTOOLS : LEDD_WHY_NONE;
    if (yield != LEDD_WHY_NONE) {
        // Follow Theme hands the LEDs back to the firmware, so the daemon's effect ends first.
        // Under the others nothing is written; the pattern is written again once they end, and
        // a stop still ends it where it shows.
        if (yield == LEDD_WHY_FOLLOW_THEME) ledd_reconcile_end(r, &in->leds, out);
        else r->pattern.effect = 0;
        r->foreign_since = 0;
        r->state = LEDD_YIELDING;
        r->reason = yield;
        return;
    }
    if (in->config_invalid || in->outcome == LEDD_POLICY_NONE) {
        ledd_reconcile_end(r, &in->leds, out);
        r->state = LEDD_IDLE;
        r->reason = in->config_invalid ? LEDD_WHY_INVALID_CONFIG :
                    in->mode == LEDD_MODE_CHARGING ? LEDD_WHY_NO_CHARGER : LEDD_WHY_DEFAULT;
        return;
    }
    if (in->outcome == LEDD_POLICY_NO_READING) {
        // Nothing is written until the reading returns; the pattern is written again then.
        r->pattern.effect = 0;
        r->foreign_since = 0;
        r->state = LEDD_IDLE;
        r->reason = LEDD_WHY_NO_READING;
        return;
    }
    r->state = LEDD_ACTIVE;
    r->reason = LEDD_WHY_NONE;
    int engine_off = in->leds.effect_enable == 0;
    // A new pattern, or the first after a yield, is written at once.
    if (!r->applied || !r->pattern.effect || !same_pattern(&r->pattern, &in->pattern)) {
        out->engine_on = engine_off;
        write_all(r, &in->pattern, in->pattern.rgb, now, out);
        return;
    }
    // Another writer: its one-shot effect plays for the settle time, then the pattern returns.
    int differs = engine_off;
    for (int g = 0; g < LEDD_GROUPS; ++g) differs |= !ledd_driver_shows(&in->leds.groups[g], &r->written[g]);
    if (differs) {
        if (!r->foreign_since) r->foreign_since = now ? now : 1;
        if (now - r->foreign_since < LEDD_SETTLE_MS) return;
        out->engine_on = engine_off;
        out->restore = 1;
        write_all(r, &r->pattern, r->pattern.random ? ledd_random_rgb(random) : r->pattern.rgb, now, out);
        return;
    }
    r->foreign_since = 0;
    if (r->pattern.random && now >= r->cycle_end) write_all(r, &r->pattern, ledd_random_rgb(random), now, out);
}

uint64_t ledd_reconcile_deadline(const struct ledd_reconciler *r) {
    // While another writer's change settles, the next cycle waits for the settle time.
    return r->applied && r->pattern.effect && r->pattern.random && !r->foreign_since ? r->cycle_end : 0;
}

const char *ledd_state_name(enum ledd_state state) {
    switch (state) {
    case LEDD_ACTIVE: return LEDD_STATE_ACTIVE;
    case LEDD_IDLE: return LEDD_STATE_IDLE;
    case LEDD_YIELDING: return LEDD_STATE_YIELDING;
    case LEDD_STOPPED: return LEDD_STATE_STOPPED;
    }
    return "";
}

const char *ledd_reason_name(enum ledd_reason reason) {
    switch (reason) {
    case LEDD_WHY_NONE: return "";
    case LEDD_WHY_DEFAULT: return LEDD_REASON_DEFAULT;
    case LEDD_WHY_FOLLOW_THEME: return LEDD_REASON_FOLLOW_THEME;
    case LEDD_WHY_LED_OFF: return LEDD_REASON_LED_OFF;
    case LEDD_WHY_STANDBY: return LEDD_REASON_STANDBY;
    case LEDD_WHY_LOW_BATTERY: return LEDD_REASON_LOW_BATTERY;
    case LEDD_WHY_BRICKTOOLS: return LEDD_REASON_BRICKTOOLS;
    case LEDD_WHY_NO_CHARGER: return LEDD_REASON_NO_CHARGER;
    case LEDD_WHY_NO_READING: return LEDD_REASON_NO_READING;
    case LEDD_WHY_INVALID_CONFIG: return LEDD_REASON_INVALID_CONFIG;
    }
    return "";
}

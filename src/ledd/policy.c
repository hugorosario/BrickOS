#include "policy.h"

#include <stddef.h>

struct step {
    long bound;
    unsigned rgb;
};

// The first step whose bound the value reaches (at least) or stays within (at most).
static unsigned at_least(const struct step *steps, size_t count, long value, unsigned below) {
    for (size_t i = 0; i < count; ++i)
        if (value >= steps[i].bound) return steps[i].rgb;
    return below;
}

static unsigned at_most(const struct step *steps, size_t count, long value, unsigned above) {
    for (size_t i = 0; i < count; ++i)
        if (value <= steps[i].bound) return steps[i].rgb;
    return above;
}

unsigned ledd_battery_rgb(int capacity) {
    static const struct step steps[] = {
        {90, LEDD_RGB_GREEN}, {70, LEDD_RGB_CHARTREUSE}, {60, LEDD_RGB_YELLOW}, {50, LEDD_RGB_ORANGE},
        {40, LEDD_RGB_DARK_ORANGE}, {30, LEDD_RGB_RED_ORANGE}, {20, LEDD_RGB_VERMILION},
    };
    return at_least(steps, sizeof(steps) / sizeof(steps[0]), capacity, LEDD_RGB_RED);
}

// Whole MHz and °C, truncated as BrickTools' shell arithmetic does.
unsigned ledd_cpu_rgb(long khz) {
    static const struct step steps[] = {
        {1200, LEDD_RGB_GREEN}, {1300, LEDD_RGB_CHARTREUSE}, {1900, LEDD_RGB_DARK_ORANGE}, {1991, LEDD_RGB_VERMILION},
    };
    return at_most(steps, sizeof(steps) / sizeof(steps[0]), khz / 1000, LEDD_RGB_RED);
}

unsigned ledd_temperature_rgb(long millicelsius) {
    static const struct step steps[] = {
        {40, LEDD_RGB_GREEN}, {45, LEDD_RGB_CHARTREUSE}, {50, LEDD_RGB_YELLOW}, {55, LEDD_RGB_ORANGE},
        {60, LEDD_RGB_DARK_ORANGE}, {65, LEDD_RGB_RED_ORANGE}, {70, LEDD_RGB_VERMILION},
    };
    return at_most(steps, sizeof(steps) / sizeof(steps[0]), millicelsius / 1000, LEDD_RGB_RED);
}

unsigned ledd_colour_rgb(enum ledd_colour colour) {
    static const unsigned rgb[LEDD_COLOUR_COUNT] = {
        0, 0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFF00u, 0xFF00FFu, 0x00FFFFu, 0xFFFFFFu,
    };
    return (unsigned)colour < LEDD_COLOUR_COUNT ? rgb[colour] : 0;
}

unsigned ledd_random_rgb(const struct ledd_random *random) {
    unsigned value = random && random->next ? random->next(random->context) : 0;
    return ledd_colour_rgb((enum ledd_colour)(value % 7 + 1));
}

int ledd_charger_connected(const struct ledd_readings *r, int *known) {
    int charging = r->has_status && (r->status == LEDD_BATTERY_CHARGING || r->status == LEDD_BATTERY_FULL);
    int online = r->has_usb && r->usb_online;
    *known = r->has_status || r->has_usb;
    return charging || online;
}

static enum ledd_policy_outcome play(struct ledd_pattern *pattern, int effect, unsigned rgb, int duration_ms,
                                     int random) {
    pattern->effect = effect;
    pattern->rgb = rgb;
    pattern->duration_ms = duration_ms;
    pattern->random = random;
    return LEDD_POLICY_PATTERN;
}

static enum ledd_policy_outcome lacking(const char **missing, const char *name) {
    if (missing) *missing = name;
    return LEDD_POLICY_NO_READING;
}

enum ledd_policy_outcome ledd_policy_pattern(const struct ledd_config *config, const struct ledd_readings *r,
                                             const struct ledd_random *random, struct ledd_pattern *pattern,
                                             const char **missing) {
    if (missing) *missing = NULL;
    switch (config->mode) {
    case LEDD_MODE_BATTERY:
        if (!r->has_capacity) return lacking(missing, "battery");
        return play(pattern, LEDD_DRIVER_STATIC, ledd_battery_rgb(r->capacity), LEDD_STATUS_DURATION_MS, 0);
    case LEDD_MODE_CPU:
        if (!r->has_cpu) return lacking(missing, "cpu");
        return play(pattern, LEDD_DRIVER_STATIC, ledd_cpu_rgb(r->cpu_khz), LEDD_STATUS_DURATION_MS, 0);
    case LEDD_MODE_TEMPERATURE:
        if (!r->has_temperature) return lacking(missing, "temperature");
        return play(pattern, LEDD_DRIVER_STATIC, ledd_temperature_rgb(r->temperature_mc), LEDD_STATUS_DURATION_MS, 0);
    case LEDD_MODE_CHARGING: {
        int known;
        if (!ledd_charger_connected(r, &known)) return known ? LEDD_POLICY_NONE : lacking(missing, "charger");
        if (r->has_status && r->status == LEDD_BATTERY_FULL)
            return play(pattern, LEDD_DRIVER_STATIC, LEDD_RGB_GREEN, LEDD_STATUS_DURATION_MS, 0);
        if (!r->has_capacity) return lacking(missing, "battery");
        return play(pattern, LEDD_DRIVER_BREATH, ledd_battery_rgb(r->capacity), LEDD_CHARGING_DURATION_MS, 0);
    }
    case LEDD_MODE_CUSTOM: {
        int duration = config->time * 1000;
        if (config->effect == LEDD_EFFECT_OFF) return play(pattern, LEDD_DRIVER_STATIC, LEDD_RGB_BLACK, duration, 0);
        if (config->colour == LEDD_COLOUR_RANDOM)
            return play(pattern, (int)config->effect, ledd_random_rgb(random), duration, 1);
        return play(pattern, (int)config->effect, ledd_colour_rgb(config->colour), duration, 0);
    }
    case LEDD_MODE_DEFAULT:
    case LEDD_MODE_COUNT:
        break;
    }
    return LEDD_POLICY_NONE;
}

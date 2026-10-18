#ifndef BRICKOS_LEDD_POLICY_H
#define BRICKOS_LEDD_POLICY_H

#include "config.h"

// Turns the configured mode and the current readings into the pattern every LED group should
// play. Pure: the readings come from the caller, and so does randomness.

// The colour scales BrickTools' LED modes use.
#define LEDD_RGB_GREEN 0x00FF00u
#define LEDD_RGB_CHARTREUSE 0x7FFF00u
#define LEDD_RGB_YELLOW 0xFFFF00u
#define LEDD_RGB_ORANGE 0xFFA500u
#define LEDD_RGB_DARK_ORANGE 0xFF8C00u
#define LEDD_RGB_RED_ORANGE 0xFF4500u
#define LEDD_RGB_VERMILION 0xFF1400u
#define LEDD_RGB_RED 0xFF0000u
#define LEDD_RGB_BLACK 0x000000u

// The driver's effect numbers the modes use.
#define LEDD_DRIVER_BREATH 2
#define LEDD_DRIVER_STATIC 4
// One cycle of a status colour, and of the charging breath.
#define LEDD_STATUS_DURATION_MS 1000
#define LEDD_CHARGING_DURATION_MS 2000

struct ledd_pattern {
    int effect;       // driver effect number, 1 to 7
    unsigned rgb;     // 0xRRGGBB
    int duration_ms;  // one cycle
    int random;       // 1: one cycle per colour, with a new random colour for each
};

enum ledd_battery_status {
    LEDD_BATTERY_UNKNOWN,
    LEDD_BATTERY_DISCHARGING,
    LEDD_BATTERY_NOT_CHARGING,
    LEDD_BATTERY_CHARGING,
    LEDD_BATTERY_FULL,
};

// Each has_* is 0 when that reading could not be taken.
struct ledd_readings {
    int has_capacity;
    int capacity;  // %
    int has_status;
    enum ledd_battery_status status;
    int has_usb;
    int usb_online;
    int has_cpu;
    long cpu_khz;
    int has_temperature;
    long temperature_mc;  // m°C
};

struct ledd_random {
    unsigned (*next)(void *context);
    void *context;
};

enum ledd_policy_outcome {
    LEDD_POLICY_PATTERN,     // play *pattern
    LEDD_POLICY_NONE,        // leave the LEDs to the firmware: default mode, or no charger
    LEDD_POLICY_NO_READING,  // a reading the mode needs is missing; *missing names it
};

unsigned ledd_battery_rgb(int capacity);
unsigned ledd_cpu_rgb(long khz);
unsigned ledd_temperature_rgb(long millicelsius);
// A named colour's value; random has none (0).
unsigned ledd_colour_rgb(enum ledd_colour colour);
// One of the seven named colours, as BrickTools picks it: value % 7 + 1.
unsigned ledd_random_rgb(const struct ledd_random *random);
// A charger is connected when USB reports online, or the battery charges or is full.
int ledd_charger_connected(const struct ledd_readings *readings, int *known);

enum ledd_policy_outcome ledd_policy_pattern(const struct ledd_config *config, const struct ledd_readings *readings,
                                             const struct ledd_random *random, struct ledd_pattern *pattern,
                                             const char **missing);

#endif

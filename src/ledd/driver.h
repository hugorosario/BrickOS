#ifndef BRICKOS_LEDD_DRIVER_H
#define BRICKOS_LEDD_DRIVER_H

// The firmware's LED driver, /sys/class/led_anim under root ("" on the device). Only its effect
// engine is used: per group the colour, cycles, duration and effect number, and the global
// effect_enable, which is only ever set to 1. The LED switch (enable) and the scales are read,
// never written. docs/ledd.md has the driver notes these follow.

#define LEDD_LED_DIR "/sys/class/led_anim"
#define LEDD_GROUPS 4
#define LEDD_GROUP_LR 0  // the shoulder lights
#define LEDD_GROUP_M 1   // top
#define LEDD_GROUP_F1 2
#define LEDD_GROUP_F2 3
#define LEDD_CYCLES_ENDLESS (-1)

extern const char *const ledd_group_names[LEDD_GROUPS];

// One group's values as the driver reads them back. The values stay when an effect ends.
struct ledd_group_state {
    int readable;
    int effect;
    unsigned rgb;
    long cycles;
    long duration;
};

struct ledd_led_state {
    int enable;         // the LED switch; -1 when unreadable
    int effect_enable;  // the effect engine; -1 when unreadable
    struct ledd_group_state groups[LEDD_GROUPS];
};

// What the daemon writes to one group.
struct ledd_written {
    int effect;
    unsigned rgb;
    long cycles;
    long duration;
};

// Reads the switch, the engine and every group. Returns 0, or -1 when a file could not be read
// (the state then marks what was missing).
int ledd_driver_read(const char *root, struct ledd_led_state *state);
// Writes the colour, cycles and duration, then the effect number, which starts the effect.
int ledd_driver_write(const char *root, int group, const struct ledd_written *written);
// effect_enable 1.
int ledd_driver_engine_on(const char *root);
// One static black cycle: the group is left dark, and the others keep their effects.
int ledd_driver_dark(const char *root, int group);
// Whether a read-back group still shows what was written: the same effect number and colour.
int ledd_driver_shows(const struct ledd_group_state *state, const struct ledd_written *written);

#endif

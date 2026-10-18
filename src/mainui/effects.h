#ifndef BRICKOS_MAINUI_EFFECTS_H
#define BRICKOS_MAINUI_EFFECTS_H

#include "model.h"
#include <stddef.h>

// The immediate effects of reversible settings, as the official MainUI's changes left them on
// the reference device (the settings map of task 1.5): keymon's shared memory (key 0x4e4f4d4b,
// 23 integers), the LCD brightness ioctl, the LED, display, and motor sysfs files, the mixer's
// "digital volume" control, and the network services. The writes go through ops, so tests
// record them and the device adapter performs them.

enum mainui_keymon_index {
    MAINUI_SHM_VOL = 0, MAINUI_SHM_BRIGHTNESS = 1, MAINUI_SHM_KEYMAP = 2, MAINUI_SHM_MUTE = 3,
    MAINUI_SHM_VOL_CHANGED = 4, MAINUI_SHM_BGMVOL = 5, MAINUI_SHM_NETPLAY = 6, MAINUI_SHM_CPUFREQ = 7,
    MAINUI_SHM_LEDSWITCH = 8, MAINUI_SHM_MENUSTYLEL1 = 9, MAINUI_SHM_TOPLED = 10, MAINUI_SHM_MENUSTYLEL2 = 11,
    MAINUI_SHM_RUMBLE = 12, MAINUI_SHM_DIMTIME = 13, MAINUI_SHM_WIFI = 14, MAINUI_SHM_HOSTAP = 15,
    MAINUI_SHM_BLUETOOTH = 16, MAINUI_SHM_TAILLIGHT = 17, MAINUI_SHM_F1F2 = 18, MAINUI_SHM_LEDPOLICY = 19,
    MAINUI_SHM_OSDSHORTCUT = 21, MAINUI_SHM_BCONFIRM = 22,
};
#define MAINUI_SHM_VALUES 23
// The mixer control the volume steps set ("digital volume").
#define MAINUI_MIXER_DIGITAL_VOLUME 3

struct mainui_effect_ops {
    void *context;
    int (*shm_set)(void *context, int index, int value);
    int (*shm_get)(void *context, int index, int *value);  // keymon's value, for values it changes itself
    int (*write_file)(void *context, const char *path, const char *value);
    int (*lcd_brightness)(void *context, int value);   // /dev/disp LCD brightness
    int (*mixer_set)(void *context, int control, int value);
    int (*run)(void *context, const char *command);
    int (*write_bytes)(void *context, const char *path, const void *bytes, size_t size);
    // Every value of one control of another sound card (the official sets card 1 control 9).
    int (*mixer_card_set)(void *context, int card, int control, int value);
};

// Measured tables, indexed by the setting's value.
extern const int mainui_volume_mixer[21];     // "digital volume" for vol 0..20
extern const int mainui_lcd_brightness[11];   // LCD brightness for brightness 0..10
extern const int mainui_motor_voltage[6];     // /sys/class/motor/voltage for rumblelevel 0..5

// Applies a SET effect's immediate effect. settings holds the values after the change (for
// effects that depend on another setting). Other effects are left to their adapters. Returns
// the number of writes, or -1 when one failed.
int mainui_effects_apply(const struct mainui_effect_ops *ops, const struct mainui_effect *effect,
                         const int settings[MAINUI_SET_COUNT]);

// A successful modern or built-in game handoff applies keymon's current keymap.
// The stock button_config write includes its terminating NUL (12 bytes), then
// inputd's rotate_270 marker is touched for keymap 1 or removed for any other value.
// Failure is reported but does not suppress the independent rotation request.
int mainui_effects_launch_keymap(const struct mainui_effect_ops *ops, int fallback);

// Stock letter-group OSD request. directory selects the default toast with localized text;
// otherwise message is one ASCII initial. Publishes JSON directly, without a shell command.
int mainui_effects_letter_toast(const struct mainui_effect_ops *ops, const char *message, int directory);

// Stock theme preview: a two-second default toast with a blank message and explicit image.
int mainui_effects_theme_preview(const struct mainui_effect_ops *ops, const char *preview);

#endif

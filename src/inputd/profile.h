#ifndef BRICKOS_INPUTD_PROFILE_H
#define BRICKOS_INPUTD_PROFILE_H

// The stock controller as observed: the stock trimui_inputd tables (s_key_io, s_tm_map,
// s_tm_map_r_270, s_tm_map_axis, s_tm_map_dpad_to_vaxis) and traces of its output.

#define INPUTD_BITS 32

// Bits of the physical snapshot, as in the stock s_key_io table (active low on the pins).
enum inputd_bit {
    INPUTD_BIT_B = 0, INPUTD_BIT_BIT1 = 1, INPUTD_BIT_SELECT = 2, INPUTD_BIT_START = 3,
    INPUTD_BIT_UP = 4, INPUTD_BIT_DOWN = 5, INPUTD_BIT_LEFT = 6, INPUTD_BIT_RIGHT = 7,
    INPUTD_BIT_A = 8, INPUTD_BIT_BIT9 = 9, INPUTD_BIT_L1 = 10, INPUTD_BIT_R1 = 11,
    INPUTD_BIT_L2 = 12, INPUTD_BIT_R2 = 13, INPUTD_BIT_MENU = 16, INPUTD_BIT_F1 = 17,
    INPUTD_BIT_F2 = 18, INPUTD_BIT_VOLUP = 19, INPUTD_BIT_VOLDOWN = 20,
};

// GPIO number per bit; -1 where the stock table has no pin.
extern const int inputd_key_io[INPUTD_BITS];

// FN is read from sysfs GPIO 243 once a second, outside the scan.
#define INPUTD_FN_GPIO 243

enum { INPUTD_EV_SYN = 0, INPUTD_EV_KEY = 1, INPUTD_EV_ABS = 3, INPUTD_EV_SW = 5 };
enum { INPUTD_ABS_X = 0, INPUTD_ABS_Y = 1, INPUTD_ABS_Z = 2, INPUTD_ABS_RZ = 5, INPUTD_ABS_HAT0X = 16, INPUTD_ABS_HAT0Y = 17 };
#define INPUTD_SW_FN 1
#define INPUTD_AXIS_MAX 32767
#define INPUTD_TRIGGER_MAX 255

struct inputd_key_map {
    int bit;
    int code;
};

// Key bits in stock order, and the rotate_270 variant.
#define INPUTD_KEYS 13
extern const struct inputd_key_map inputd_keys[INPUTD_KEYS];
extern const struct inputd_key_map inputd_keys_r270[INPUTD_KEYS];

struct inputd_axis_map {
    int bit;
    int axis;
    int value;
};

// D-pad bits as hats, and as stick axes for input_dpad_to_joystick and held ALT.
#define INPUTD_DPAD 4
extern const struct inputd_axis_map inputd_dpad_hat[INPUTD_DPAD];
extern const struct inputd_axis_map inputd_dpad_axis[INPUTD_DPAD];

// Marker files and the bits they act on (stock s_tm_map_turbo and
// s_tm_map_dpad_to_vaxis_hold_alt).
struct inputd_marker {
    int bit;
    const char *path;
};
#define INPUTD_TURBOS 8
#define INPUTD_ALTS 6
extern const struct inputd_marker inputd_turbo_markers[INPUTD_TURBOS];
extern const struct inputd_marker inputd_alt_markers[INPUTD_ALTS];
#define INPUTD_MARKER_DIR "/tmp/trimui_inputd"
#define INPUTD_NO_DPAD INPUTD_MARKER_DIR "/input_no_dpad"
#define INPUTD_DPAD2AXIS INPUTD_MARKER_DIR "/input_dpad_to_joystick"
#define INPUTD_ROTATE_270 "/var/trimui_inputd/rotate_270"
#define INPUTD_GRAB INPUTD_MARKER_DIR "/grab"
#define INPUTD_SUSPEND "/tmp/system_suspend"

// Physical sources of the version-one profile. In its names, "X" is the GPIO 115 bit (stock
// code 307) and "Y" the GPIO 114 bit (308), swapped relative to the Brick's labels.
#define INPUTD_SOURCES 18
struct inputd_source {
    const char *name;
    int bit; // -1 for FN
};
extern const struct inputd_source inputd_sources[INPUTD_SOURCES];

#endif

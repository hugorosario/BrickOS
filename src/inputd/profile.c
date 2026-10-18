#include "profile.h"

const int inputd_key_io[INPUTD_BITS] = {
    109, 115, 239, 240, 113, 110, 111, 112, 108, 114, 36, 241, 34, 37, -1, -1,
    238, 236, 237, 117, 116, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

const struct inputd_key_map inputd_keys[INPUTD_KEYS] = {
    {8, 305}, {0, 304}, {9, 308}, {1, 307}, {10, 310}, {11, 311}, {2, 314},
    {3, 315}, {16, 316}, {17, 317}, {18, 318}, {19, 115}, {20, 114},
};

const struct inputd_key_map inputd_keys_r270[INPUTD_KEYS] = {
    {8, 308}, {0, 305}, {9, 307}, {1, 304}, {10, 310}, {11, 311}, {2, 314},
    {3, 315}, {16, 316}, {17, 317}, {18, 318}, {19, 115}, {20, 114},
};

const struct inputd_axis_map inputd_dpad_hat[INPUTD_DPAD] = {
    {INPUTD_BIT_UP, INPUTD_ABS_HAT0Y, -1}, {INPUTD_BIT_DOWN, INPUTD_ABS_HAT0Y, 1},
    {INPUTD_BIT_LEFT, INPUTD_ABS_HAT0X, -1}, {INPUTD_BIT_RIGHT, INPUTD_ABS_HAT0X, 1},
};

const struct inputd_axis_map inputd_dpad_axis[INPUTD_DPAD] = {
    {INPUTD_BIT_UP, INPUTD_ABS_Y, -INPUTD_AXIS_MAX}, {INPUTD_BIT_DOWN, INPUTD_ABS_Y, INPUTD_AXIS_MAX},
    {INPUTD_BIT_LEFT, INPUTD_ABS_X, -INPUTD_AXIS_MAX}, {INPUTD_BIT_RIGHT, INPUTD_ABS_X, INPUTD_AXIS_MAX},
};

const struct inputd_source inputd_sources[INPUTD_SOURCES] = {
    {"A", INPUTD_BIT_A}, {"B", INPUTD_BIT_B}, {"X", INPUTD_BIT_BIT1}, {"Y", INPUTD_BIT_BIT9},
    {"L1", INPUTD_BIT_L1}, {"R1", INPUTD_BIT_R1}, {"SELECT", INPUTD_BIT_SELECT},
    {"START", INPUTD_BIT_START}, {"GUIDE", INPUTD_BIT_MENU}, {"F1", INPUTD_BIT_F1},
    {"F2", INPUTD_BIT_F2}, {"VOLUME_UP", INPUTD_BIT_VOLUP}, {"VOLUME_DOWN", INPUTD_BIT_VOLDOWN},
    {"DPAD_UP", INPUTD_BIT_UP}, {"DPAD_DOWN", INPUTD_BIT_DOWN}, {"DPAD_LEFT", INPUTD_BIT_LEFT},
    {"DPAD_RIGHT", INPUTD_BIT_RIGHT}, {"FN", -1},
};

const struct inputd_marker inputd_turbo_markers[INPUTD_TURBOS] = {
    {INPUTD_BIT_A, INPUTD_MARKER_DIR "/turbo_a"}, {INPUTD_BIT_B, INPUTD_MARKER_DIR "/turbo_b"},
    {INPUTD_BIT_BIT9, INPUTD_MARKER_DIR "/turbo_x"}, {INPUTD_BIT_BIT1, INPUTD_MARKER_DIR "/turbo_y"},
    {INPUTD_BIT_R1, INPUTD_MARKER_DIR "/turbo_r"}, {INPUTD_BIT_R2, INPUTD_MARKER_DIR "/turbo_r2"},
    {INPUTD_BIT_L1, INPUTD_MARKER_DIR "/turbo_l"}, {INPUTD_BIT_L2, INPUTD_MARKER_DIR "/turbo_l2"},
};

const struct inputd_marker inputd_alt_markers[INPUTD_ALTS] = {
    {INPUTD_BIT_F1, INPUTD_MARKER_DIR "/dpad2axis_hold_f1"}, {INPUTD_BIT_F2, INPUTD_MARKER_DIR "/dpad2axis_hold_f2"},
    {INPUTD_BIT_R2, INPUTD_MARKER_DIR "/dpad2axis_hold_r2"}, {INPUTD_BIT_L2, INPUTD_MARKER_DIR "/dpad2axis_hold_l2"},
    {INPUTD_BIT_SELECT, INPUTD_MARKER_DIR "/dpad2axis_hold_select"},
    {INPUTD_BIT_START, INPUTD_MARKER_DIR "/dpad2axis_hold_start"},
};

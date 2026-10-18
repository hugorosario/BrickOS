#ifndef BRICKOS_MAINUI_INPUT_H
#define BRICKOS_MAINUI_INPUT_H

#include "model.h"

#include <stdint.h>

// Turns controller events into the model's key presses, without SDL. The controller is inputd's
// TRIMUI Player1: A 305, B 304, X 308, Y 307, L1 310, R1 311, SELECT 314, START 315, MENU 316,
// F1 317, F2 318 as keys, L2 and R2 as ABS_Z and ABS_RZ, and the D-pad as HAT0.
//
// Behavior of the official MainUI:
// - Face/shoulder actions occur on button-up. Button-down clicks and cancels
//   direction repeat without erasing the raw Hardware Test held state.
// - MENU, START, and SELECT act when released, and not at all when pressed together; MENU with
//   START is the chord that leaves the IO test (static: "joybuttonup menu ms ..., return
//   KEY_MENU_KEY"). MENU with SELECT belongs to keymon's OSD shortcut.
// - Only the D-pad repeats. The official worker checks every 100 ms, first after
//   strictly more than 500 ms, on an already-running 100 ms clock. Its phase
//   gives a 501..600 ms initial delay, without late acceleration. Content lists
//   accelerate by paging after a sustained direction streak, rather than by
//   generating more input/sound events (ListMenu::eventHandling, 0x41f68c).
//   Physical delivery/phase remains an acceptance gate.
// - Holding POWER for more than 1.5 s opens the power-off dialog (static: the first POWER
//   autorepeat after 1500 ms shows it).

#define MAINUI_INPUT_QUEUE 64
#define MAINUI_REPEAT_DELAY_MS 500
#define MAINUI_REPEAT_INTERVAL_MS 100
#define MAINUI_POWER_HOLD_MS 1500
#define MAINUI_TRIGGER_THRESHOLD 128

enum mainui_code {
    MAINUI_CODE_UP = 103, MAINUI_CODE_DOWN = 108, MAINUI_CODE_LEFT = 105, MAINUI_CODE_RIGHT = 106,
    MAINUI_CODE_B = 304, MAINUI_CODE_A = 305, MAINUI_CODE_Y = 307, MAINUI_CODE_X = 308,
    MAINUI_CODE_L1 = 310, MAINUI_CODE_R1 = 311, MAINUI_CODE_SELECT = 314, MAINUI_CODE_START = 315,
    MAINUI_CODE_MENU = 316, MAINUI_CODE_F1 = 317, MAINUI_CODE_F2 = 318,
    MAINUI_CODE_ABS_Z = 2, MAINUI_CODE_ABS_RZ = 5,
};

// KeyTestWindow2's control order, independent of menu actions and A/B confirmation.
enum mainui_test_control {
    MAINUI_TEST_A, MAINUI_TEST_B, MAINUI_TEST_X, MAINUI_TEST_Y,
    MAINUI_TEST_UP, MAINUI_TEST_DOWN, MAINUI_TEST_LEFT, MAINUI_TEST_RIGHT,
    MAINUI_TEST_START, MAINUI_TEST_SELECT, MAINUI_TEST_MENU, MAINUI_TEST_L1,
    MAINUI_TEST_R1, MAINUI_TEST_L2, MAINUI_TEST_R2, MAINUI_TEST_POWER,
    MAINUI_TEST_VOLUP, MAINUI_TEST_VOLDOWN, MAINUI_TEST_F1, MAINUI_TEST_F2,
    MAINUI_TEST_CONTROLS,
};

struct mainui_input {
    enum mainui_key queue[MAINUI_INPUT_QUEUE];
    unsigned char repeated[MAINUI_INPUT_QUEUE]; // generated repeat, not a raw event action
    int head, count;
    // The held direction and when its next repeat is due.
    int direction;               // a mainui_key, or -1
    uint64_t direction_since, next_repeat;
    uint64_t repeat_epoch;
    int repeat_clock_set;
    int hat_x, hat_y;
    // MENU, START, SELECT: held, and whether another of them joined while held.
    int menu, start, select, chorded;
    int l2, r2;
    unsigned int buttons;        // held face/shoulder buttons, to ignore duplicate reports
    uint32_t test_buttons;        // raw held controls, including F1/F2 and volume
    int power;
    uint64_t power_since;
    int power_reported;
    int power_hold;              // set when the hold time passes; cleared by the reader
};

void mainui_input_init(struct mainui_input *in);
// Set the common frontend repeat-clock origin before controller input starts.
// Reconnection uses the same origin; release/reset does not rephase the clock.
void mainui_input_repeat_clock(struct mainui_input *in, uint64_t epoch_ms);
// Stock reset_repeat_key: keep produced events/raw controls/POWER, cancel the
// held logical direction, and restart the repeat clock after its wake signal.
void mainui_input_interrupt_repeat(struct mainui_input *in, uint64_t now_ms);
// Evdev key code (see enum mainui_code), pressed 1 or 0.
void mainui_input_button(struct mainui_input *in, int code, int pressed, uint64_t now_ms);
// HAT0: x and y in -1..1.
void mainui_input_hat(struct mainui_input *in, int x, int y, uint64_t now_ms);
// ABS_Z or ABS_RZ, 0..255.
void mainui_input_axis(struct mainui_input *in, int code, int value, uint64_t now_ms);
void mainui_input_power(struct mainui_input *in, int pressed, uint64_t now_ms);
// Hardware-test state only: never produces a menu action or repeat.
void mainui_input_test_button(struct mainui_input *in, int code, int pressed);
void mainui_input_test_axis(struct mainui_input *in, int code, int value);
uint32_t mainui_input_test_held(const struct mainui_input *in);
// The controller went away: everything held is released without a key, so nothing sticks.
void mainui_input_reset(struct mainui_input *in);
// inputd's grab transition clears the logical system-button/chord state. Keep
// already-produced keys, physical direction/face state and the separate POWER hold.
void mainui_input_reset_system(struct mainui_input *in);
// Takes the next key, with repeats due by now_ms. Returns 0 when there is none.
int mainui_input_next(struct mainui_input *in, uint64_t now_ms, enum mainui_key *key);
// Same queue, retaining the repeat origin for the SDL loop's sound dispatch.
int mainui_input_next_event(struct mainui_input *in, uint64_t now_ms, enum mainui_key *key, int *repeated);
// 1 once when POWER has been held long enough (checked with the time of the last call).
int mainui_input_power_hold(struct mainui_input *in, uint64_t now_ms);

#define MAINUI_INPUT_GRAB_CHANGED 1
#define MAINUI_INPUT_OSD_CHANGED 2
struct mainui_input_gate { int grab, osd; };
// Stock's two independent markers: grab resets logical chords; osdd_show_up
// consumes input without delivering actions. Both are read only, including dry runs.
int mainui_input_gate_update(struct mainui_input_gate *gate, int grab, int osd);
int mainui_input_gate_read(struct mainui_input_gate *gate, const char *root);
// Drain suppressed keys/repeats while the OSD owns input, so none are replayed
// when its signal disappears. Physical held state continues to follow reports.
int mainui_input_next_visible(struct mainui_input *in, uint64_t now_ms, int osd, enum mainui_key *key);

// SDL numbers a joystick's buttons by key code: BTN_JOYSTICK (0x120) up to KEY_MAX, then from
// 0. Given the device's EV_KEY bits, fills codes[i] with the code of SDL button i; returns the
// button count.
int mainui_input_sdl_buttons(const unsigned long *key_bits, int bits_longs, int *codes, int max);

#endif

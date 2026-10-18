#include "input.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void mainui_input_init(struct mainui_input *in) {
    memset(in, 0, sizeof(*in));
    in->direction = -1;
}

void mainui_input_repeat_clock(struct mainui_input *in, uint64_t epoch) {
    in->repeat_epoch = epoch;
    in->repeat_clock_set = 1;
}

void mainui_input_interrupt_repeat(struct mainui_input *in, uint64_t now) {
    in->direction = -1;
    mainui_input_repeat_clock(in, now);
}

static void push(struct mainui_input *in, enum mainui_key key) {
    if (in->count == MAINUI_INPUT_QUEUE) return;
    int tail = (in->head + in->count++) % MAINUI_INPUT_QUEUE;
    in->queue[tail] = key;
    in->repeated[tail] = 0;
}

static void press_direction(struct mainui_input *in, int key, uint64_t now) {
    in->direction = key;
    in->direction_since = now;
    if (!in->repeat_clock_set) mainui_input_repeat_clock(in, now);
    uint64_t phase = now >= in->repeat_epoch ? (now - in->repeat_epoch) % MAINUI_REPEAT_INTERVAL_MS : 0;
    // Strictly >500 ms, sampled on the existing clock, rather than restarting
    // a fixed 600 ms timer on every press. Subsequent events retain that phase.
    in->next_repeat = now + MAINUI_REPEAT_DELAY_MS + MAINUI_REPEAT_INTERVAL_MS - phase;
    push(in, (enum mainui_key)key);
}

// The direction a hat position holds; with a diagonal, the vertical one (the lists' axis).
static int hat_direction(int x, int y) {
    if (y < 0) return MAINUI_KEY_UP;
    if (y > 0) return MAINUI_KEY_DOWN;
    if (x < 0) return MAINUI_KEY_LEFT;
    if (x > 0) return MAINUI_KEY_RIGHT;
    return -1;
}

void mainui_input_hat(struct mainui_input *in, int x, int y, uint64_t now) {
    if (x == in->hat_x && y == in->hat_y) return;  // a repeated report changes nothing
    in->hat_x = x;
    in->hat_y = y;
    int direction = hat_direction(x, y);
    if (direction < 0) in->direction = -1;
    else if (direction != in->direction) press_direction(in, direction, now);
}

// MENU, START, and SELECT: pressed alone, the key acts on release.
static void system_button(struct mainui_input *in, int *held, int pressed, enum mainui_key key) {
    if (pressed) {
        if (*held) return;
        if (in->menu || in->start || in->select) in->chorded = 1;
        *held = 1;
        return;
    }
    if (!*held) return;
    *held = 0;
    int others = in->menu + in->start + in->select;
    if (!in->chorded) push(in, key);
    else if (key == MAINUI_KEY_MENU || key == MAINUI_KEY_START) {
        // The chord acts once, when its first key comes up.
        if (in->chorded == 1 && ((key == MAINUI_KEY_MENU && in->start) || (key == MAINUI_KEY_START && in->menu))) {
            push(in, MAINUI_KEY_MENU_START);
            in->chorded = 2;
        }
    }
    if (!others) in->chorded = 0;
}

void mainui_input_button(struct mainui_input *in, int code, int pressed, uint64_t now) {
    mainui_input_test_button(in, code, pressed);
    if (pressed) mainui_input_interrupt_repeat(in, now);
    switch (code) {
    case MAINUI_CODE_MENU: system_button(in, &in->menu, pressed, MAINUI_KEY_MENU); return;
    case MAINUI_CODE_START: system_button(in, &in->start, pressed, MAINUI_KEY_START); return;
    case MAINUI_CODE_SELECT: system_button(in, &in->select, pressed, MAINUI_KEY_SELECT); return;
    default: break;
    }
    // Stock wait_event emits these actions on UP, including an UP whose DOWN
    // preceded controller discovery. Repeated DOWN reports only update held
    // state/reset repeat; their raw click belongs to the SDL sound path.
    if (code < MAINUI_CODE_B || code > MAINUI_CODE_R1) return;
    unsigned int bit = 1U << (code - MAINUI_CODE_B);
    if (pressed) { in->buttons |= bit; return; }
    in->buttons &= ~bit;
    switch (code) {
    case MAINUI_CODE_A: push(in, MAINUI_KEY_A); break;
    case MAINUI_CODE_B: push(in, MAINUI_KEY_B); break;
    case MAINUI_CODE_X: push(in, MAINUI_KEY_X); break;
    case MAINUI_CODE_Y: push(in, MAINUI_KEY_Y); break;
    case MAINUI_CODE_L1: push(in, MAINUI_KEY_L1); break;
    case MAINUI_CODE_R1: push(in, MAINUI_KEY_R1); break;
    default: break;  // F1 and F2 belong to keymon
    }
}

void mainui_input_test_button(struct mainui_input *in, int code, int pressed) {
    int control;
    switch (code) {
    case MAINUI_CODE_UP: control = MAINUI_TEST_UP; break;
    case MAINUI_CODE_DOWN: control = MAINUI_TEST_DOWN; break;
    case MAINUI_CODE_LEFT: control = MAINUI_TEST_LEFT; break;
    case MAINUI_CODE_RIGHT: control = MAINUI_TEST_RIGHT; break;
    case MAINUI_CODE_A: control = MAINUI_TEST_A; break;
    case MAINUI_CODE_B: control = MAINUI_TEST_B; break;
    case MAINUI_CODE_X: control = MAINUI_TEST_X; break;
    case MAINUI_CODE_Y: control = MAINUI_TEST_Y; break;
    case MAINUI_CODE_START: control = MAINUI_TEST_START; break;
    case MAINUI_CODE_SELECT: control = MAINUI_TEST_SELECT; break;
    case MAINUI_CODE_MENU: control = MAINUI_TEST_MENU; break;
    case MAINUI_CODE_L1: control = MAINUI_TEST_L1; break;
    case MAINUI_CODE_R1: control = MAINUI_TEST_R1; break;
    case MAINUI_CODE_F1: control = MAINUI_TEST_F1; break;
    case MAINUI_CODE_F2: control = MAINUI_TEST_F2; break;
    case 115: control = MAINUI_TEST_VOLUP; break; // KEY_VOLUMEUP
    case 114: control = MAINUI_TEST_VOLDOWN; break;
    default: return;
    }
    uint32_t bit = UINT32_C(1) << control;
    if (pressed) in->test_buttons |= bit;
    else in->test_buttons &= ~bit;
}

uint32_t mainui_input_test_held(const struct mainui_input *in) {
    uint32_t held = in->test_buttons;
    if (in->hat_y < 0) held |= UINT32_C(1) << MAINUI_TEST_UP;
    if (in->hat_y > 0) held |= UINT32_C(1) << MAINUI_TEST_DOWN;
    if (in->hat_x < 0) held |= UINT32_C(1) << MAINUI_TEST_LEFT;
    if (in->hat_x > 0) held |= UINT32_C(1) << MAINUI_TEST_RIGHT;
    if (in->power) held |= UINT32_C(1) << MAINUI_TEST_POWER;
    return held;
}

void mainui_input_axis(struct mainui_input *in, int code, int value, uint64_t now) {
    (void)now;
    mainui_input_test_axis(in, code, value);
    int *held = code == MAINUI_CODE_ABS_Z ? &in->l2 : code == MAINUI_CODE_ABS_RZ ? &in->r2 : NULL;
    if (!held) return;
    int pressed = value >= MAINUI_TRIGGER_THRESHOLD;
    if (pressed && !*held) push(in, code == MAINUI_CODE_ABS_Z ? MAINUI_KEY_L2 : MAINUI_KEY_R2);
    *held = pressed;
}

void mainui_input_test_axis(struct mainui_input *in, int code, int value) {
    if (code != MAINUI_CODE_ABS_Z && code != MAINUI_CODE_ABS_RZ) return;
    uint32_t bit = UINT32_C(1) << (code == MAINUI_CODE_ABS_Z ? MAINUI_TEST_L2 : MAINUI_TEST_R2);
    // KeyTestWindow2 tests the raw SDL axis sign, independently of the menu threshold.
    if (value > 0) in->test_buttons |= bit;
    else in->test_buttons &= ~bit;
}

void mainui_input_power(struct mainui_input *in, int pressed, uint64_t now) {
    if (pressed && !in->power) {
        in->power = 1;
        in->power_since = now;
        in->power_reported = 0;
    } else if (!pressed) {
        in->power = 0;
    }
}

int mainui_input_power_hold(struct mainui_input *in, uint64_t now) {
    if (in->power && !in->power_reported && now >= in->power_since && now - in->power_since > MAINUI_POWER_HOLD_MS) {
        in->power_reported = 1;
        return 1;
    }
    return 0;
}

void mainui_input_reset(struct mainui_input *in) {
    enum mainui_key queue[MAINUI_INPUT_QUEUE];
    unsigned char repeated[MAINUI_INPUT_QUEUE];
    int head = in->head, count = in->count;
    uint64_t repeat_epoch = in->repeat_epoch;
    int repeat_clock_set = in->repeat_clock_set;
    memcpy(queue, in->queue, sizeof(queue));
    memcpy(repeated, in->repeated, sizeof(repeated));
    mainui_input_init(in);
    in->repeat_epoch = repeat_epoch;
    in->repeat_clock_set = repeat_clock_set;
    // Keys already produced still count; only held state is dropped.
    memcpy(in->queue, queue, sizeof(queue));
    memcpy(in->repeated, repeated, sizeof(repeated));
    in->head = head;
    in->count = count;
}

void mainui_input_reset_system(struct mainui_input *in) {
    in->menu = in->start = in->select = in->chorded = 0;
}

int mainui_input_gate_update(struct mainui_input_gate *gate, int grab, int osd) {
    grab = !!grab; osd = !!osd;
    int changed = (grab != gate->grab ? MAINUI_INPUT_GRAB_CHANGED : 0) |
                  (osd != gate->osd ? MAINUI_INPUT_OSD_CHANGED : 0);
    gate->grab = grab; gate->osd = osd;
    return changed;
}

static int input_marker(const char *root, const char *name) {
    char path[1100];
    int n = snprintf(path, sizeof(path), "%s%s", root ? root : "", name);
    return n >= 0 && n < (int)sizeof(path) && access(path, F_OK) == 0;
}

int mainui_input_gate_read(struct mainui_input_gate *gate, const char *root) {
    int grab = input_marker(root, "/tmp/trimui_inputd/grab");
    int osd = input_marker(root, "/tmp/trimui_osd/osdd_show_up");
    return mainui_input_gate_update(gate, grab, osd);
}

int mainui_input_next_event(struct mainui_input *in, uint64_t now, enum mainui_key *key, int *repeated) {
    // Repeats due by now join the queue in order.
    while (in->direction >= 0 && now >= in->next_repeat && in->count < MAINUI_INPUT_QUEUE) {
        push(in, (enum mainui_key)in->direction);
        in->repeated[(in->head + in->count - 1) % MAINUI_INPUT_QUEUE] = 1;
        in->next_repeat += MAINUI_REPEAT_INTERVAL_MS;
    }
    if (!in->count) return 0;
    *key = in->queue[in->head];
    if (repeated) *repeated = in->repeated[in->head];
    in->head = (in->head + 1) % MAINUI_INPUT_QUEUE;
    --in->count;
    return 1;
}

int mainui_input_next(struct mainui_input *in, uint64_t now, enum mainui_key *key) {
    return mainui_input_next_event(in, now, key, NULL);
}

int mainui_input_next_visible(struct mainui_input *in, uint64_t now, int osd, enum mainui_key *key) {
    while (mainui_input_next(in, now, key)) if (!osd) return 1;
    return 0;
}

int mainui_input_sdl_buttons(const unsigned long *key_bits, int bits_longs, int *codes, int max) {
    const int bits_per_long = (int)(sizeof(unsigned long) * CHAR_BIT);
    const int total = bits_longs * bits_per_long, joystick = 0x120;
    int n = 0;
#define HAS(code) (((key_bits[(code) / bits_per_long]) >> ((code) % bits_per_long)) & 1UL)
    for (int code = joystick; code < total && n < max; ++code)
        if (HAS(code)) codes[n++] = code;
    for (int code = 0; code < joystick && code < total && n < max; ++code)
        if (HAS(code)) codes[n++] = code;
#undef HAS
    return n;
}

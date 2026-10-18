#include "engine.h"

#include <string.h>

static void emit(struct inputd_engine *e, struct inputd_events *out, int type, int code, int value) {
    if (out->count < INPUTD_MAX_EVENTS) out->ev[out->count++] = (struct inputd_event){type, code, value};
    if (type == INPUTD_EV_KEY && code >= 0 && code < INPUTD_KEY_CODES) e->key_out[code] = value != 0;
    if (type == INPUTD_EV_ABS && code >= 0 && code < INPUTD_ABS_CODES) e->abs_out[code] = value;
}

static int hat_index(int axis) { return axis == INPUTD_ABS_HAT0X ? 0 : axis == INPUTD_ABS_HAT0Y ? 1 : -1; }

// A key output. Keys named by a binding are reference counted: pressed on the first holder, released when the count reaches zero.
static void key(struct inputd_engine *e, struct inputd_events *out, int code, int pressed) {
    if (code < 0 || code >= INPUTD_KEY_CODES || !e->managed_key[code]) {
        emit(e, out, INPUTD_EV_KEY, code, pressed);
        return;
    }
    if (pressed) {
        if (++e->key_count[code] == 1) emit(e, out, INPUTD_EV_KEY, code, 1);
        return;
    }
    if (e->key_count[code] > 0) --e->key_count[code];
    if (e->key_count[code] == 0) emit(e, out, INPUTD_EV_KEY, code, 0);
}

static void hat_emit(struct inputd_engine *e, struct inputd_events *out, int axis) {
    int i = hat_index(axis);
    int target = e->hat_pos[i] > 0 && e->hat_neg[i] == 0 ? 1 : e->hat_neg[i] > 0 && e->hat_pos[i] == 0 ? -1 : 0;
    if (target == e->hat_cur[i]) return;
    e->hat_cur[i] = target;
    emit(e, out, INPUTD_EV_ABS, axis, target);
}

// A counted hat direction: the axis shows a direction while only that direction is held.
static void hat_count(struct inputd_engine *e, struct inputd_events *out, int axis, int value, int pressed) {
    int i = hat_index(axis);
    int *count = value > 0 ? &e->hat_pos[i] : &e->hat_neg[i];
    if (pressed) ++*count;
    else if (*count > 0) --*count;
    hat_emit(e, out, axis);
}

static int hat_managed(const struct inputd_engine *e, int axis) {
    int i = hat_index(axis);
    return i >= 0 && e->managed_hat[i];
}

// The stock axis_table: an edge of each D-pad bit writes its axis; a release writes 0 only
// when no other entry on the same axis is held.
static void axis_table(struct inputd_engine *e, struct inputd_events *out, const struct inputd_axis_map *t,
                       uint32_t st, uint32_t last) {
    for (int k = 0; k < INPUTD_DPAD; ++k) {
        uint32_t b = 1u << t[k].bit;
        int managed = hat_managed(e, t[k].axis);
        if ((st & b) && !(last & b)) {
            if (managed) hat_count(e, out, t[k].axis, t[k].value, 1);
            else emit(e, out, INPUTD_EV_ABS, t[k].axis, t[k].value);
        } else if (!(st & b) && (last & b)) {
            if (managed) {
                hat_count(e, out, t[k].axis, t[k].value, 0);
                continue;
            }
            uint32_t same = b;
            for (int j = 0; j < INPUTD_DPAD; ++j)
                if (j != k && t[j].axis == t[k].axis) same |= 1u << t[j].bit;
            if (!(st & same)) emit(e, out, INPUTD_EV_ABS, t[k].axis, 0);
        }
    }
}

static void dest(struct inputd_engine *e, struct inputd_events *out, const struct inputd_dest *d, int pressed) {
    switch (d->type) {
    case INPUTD_EV_KEY: key(e, out, d->code, pressed); break;
    case INPUTD_EV_ABS: hat_count(e, out, d->code, d->value, pressed); break;
    case INPUTD_EV_SW: emit(e, out, INPUTD_EV_SW, d->code, pressed ? d->value : 0); break;
    default: break; // "disabled"
    }
}

static void load_profile(struct inputd_engine *e, const struct inputd_config *config) {
    e->profile = config != NULL;
    e->bound = 0;
    e->fn_bound = 0;
    memset(e->bit_dest, 0, sizeof(e->bit_dest));
    memset(e->managed_key, 0, sizeof(e->managed_key));
    memset(e->key_count, 0, sizeof(e->key_count));
    memset(e->managed_hat, 0, sizeof(e->managed_hat));
    memset(e->hat_pos, 0, sizeof(e->hat_pos));
    memset(e->hat_neg, 0, sizeof(e->hat_neg));
    memset(e->hat_cur, 0, sizeof(e->hat_cur));
    if (!config) return;
    for (int s = 0; s < INPUTD_SOURCES; ++s) {
        if (!config->bound[s]) continue;
        const struct inputd_dest *d = &config->dest[s];
        int bit = inputd_sources[s].bit;
        if (bit < 0) {
            e->fn_bound = 1;
            e->fn_dest = *d;
        } else {
            e->bound |= 1u << bit;
            e->bit_dest[bit] = *d;
        }
        if (d->type == INPUTD_EV_KEY && d->code >= 0 && d->code < INPUTD_KEY_CODES) e->managed_key[d->code] = 1;
        if (d->type == INPUTD_EV_ABS && hat_index(d->code) >= 0) e->managed_hat[hat_index(d->code)] = 1;
    }
}

void inputd_engine_init(struct inputd_engine *e, const struct inputd_config *config) {
    memset(e, 0, sizeof(*e));
    load_profile(e, config);
}

void inputd_engine_scan(struct inputd_engine *e, uint32_t pressed, int suspended, struct inputd_events *out) {
    if (suspended) return;
    e->disarmed &= pressed;
    uint32_t live = pressed & ~e->disarmed;
    uint32_t st = live & ~e->bound;
    if (e->toggle) st &= ~e->markers.turbo; // the turbo phase: held turbo keys read released
    e->toggle = !e->toggle;
    uint32_t last = e->last;
    const struct inputd_key_map *map = e->markers.rotate ? inputd_keys_r270 : inputd_keys;
    for (int i = 0; i < INPUTD_KEYS; ++i) {
        uint32_t b = 1u << map[i].bit;
        if ((st & b) && !(last & b)) key(e, out, map[i].code, 1);
        else if (!(st & b) && (last & b)) key(e, out, map[i].code, 0);
    }
    static const int triggers[2][2] = {{INPUTD_BIT_L2, INPUTD_ABS_Z}, {INPUTD_BIT_R2, INPUTD_ABS_RZ}};
    for (int i = 0; i < 2; ++i) {
        uint32_t b = 1u << triggers[i][0];
        if ((st & b) && !(last & b)) emit(e, out, INPUTD_EV_ABS, triggers[i][1], INPUTD_TRIGGER_MAX);
        else if (!(st & b) && (last & b)) emit(e, out, INPUTD_EV_ABS, triggers[i][1], 0);
    }
    int mode;
    if (e->markers.dpad2axis) {
        mode = 1;
        if (!e->markers.no_dpad) axis_table(e, out, inputd_dpad_hat, st, last);
        axis_table(e, out, inputd_dpad_axis, st, last);
    } else if (st & e->markers.alt) {
        mode = 2;
        axis_table(e, out, inputd_dpad_axis, st, last);
    } else {
        mode = 0;
        if (!e->markers.no_dpad) axis_table(e, out, inputd_dpad_hat, st, last);
        if (e->mode_last)
            for (int k = 0; k < INPUTD_DPAD; ++k) emit(e, out, INPUTD_EV_ABS, inputd_dpad_axis[k].axis, 0);
    }
    e->last = st;
    e->mode_last = mode;
    uint32_t bst = live & e->bound;
    for (int bit = 0; bit < INPUTD_BITS; ++bit) {
        uint32_t b = 1u << bit;
        if (!(e->bound & b)) continue;
        if ((bst & b) && !(e->bound_last & b)) dest(e, out, &e->bit_dest[bit], 1);
        else if (!(bst & b) && (e->bound_last & b)) dest(e, out, &e->bit_dest[bit], 0);
    }
    e->bound_last = bst;
}

void inputd_engine_grab(struct inputd_engine *e, struct inputd_events *out) {
    for (int i = 0; i < INPUTD_KEYS; ++i) {
        int code = inputd_keys[i].code;
        e->key_count[code] = 0;
        emit(e, out, INPUTD_EV_KEY, code, 0);
    }
    static const int axes[] = {INPUTD_ABS_HAT0Y, INPUTD_ABS_HAT0Y, INPUTD_ABS_HAT0X, INPUTD_ABS_HAT0X, INPUTD_ABS_Z, INPUTD_ABS_RZ};
    for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); ++i) emit(e, out, INPUTD_EV_ABS, axes[i], 0);
    memset(e->hat_pos, 0, sizeof(e->hat_pos));
    memset(e->hat_neg, 0, sizeof(e->hat_neg));
    memset(e->hat_cur, 0, sizeof(e->hat_cur));
}

void inputd_engine_fn(struct inputd_engine *e, int value, struct inputd_events *out) {
    if (!e->fn_bound) {
        emit(e, out, INPUTD_EV_SW, INPUTD_SW_FN, value);
        return;
    }
    int held = value == 1;
    if (held != e->fn_held) dest(e, out, &e->fn_dest, held);
    e->fn_held = held;
}

void inputd_engine_neutral(struct inputd_engine *e, struct inputd_events *out) {
    for (int code = 0; code < INPUTD_KEY_CODES; ++code) {
        if (e->key_out[code]) emit(e, out, INPUTD_EV_KEY, code, 0);
        e->key_count[code] = 0;
    }
    for (int code = 0; code < INPUTD_ABS_CODES; ++code)
        if (e->abs_out[code]) emit(e, out, INPUTD_EV_ABS, code, 0);
    memset(e->hat_pos, 0, sizeof(e->hat_pos));
    memset(e->hat_neg, 0, sizeof(e->hat_neg));
    memset(e->hat_cur, 0, sizeof(e->hat_cur));
}

void inputd_engine_reload(struct inputd_engine *e, const struct inputd_config *config, uint32_t pressed,
                          struct inputd_events *out) {
    inputd_engine_neutral(e, out);
    load_profile(e, config);
    e->disarmed = pressed;
    e->last = 0;
    e->bound_last = 0;
    e->mode_last = 0;
    e->fn_held = 0;
}

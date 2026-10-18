#include "sound.h"

#include <stdio.h>
#include <string.h>

int mainui_sound_input_click(enum mainui_sound_input source, int value, int enabled) {
    return enabled && (source == MAINUI_SOUND_INPUT_BUTTON_DOWN ||
                       (source == MAINUI_SOUND_INPUT_HAT && value != 0) ||
                       source == MAINUI_SOUND_INPUT_REPEAT);
}

int mainui_sound_bgm_volume(int bgmvol) {
    if (bgmvol < 0) bgmvol = 0;
    if (bgmvol > 20) bgmvol = 20;
    return bgmvol * 64 / 20;
}

int mainui_sound_bluealsa_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 20) vol = 20;
    return vol * 127 / 20;
}

int mainui_sound_mixer_control(const char *line, char *out, size_t size) {
    if (!line || !strstr(line, "Simple mixer control ")) return -1;
    const char *first = strchr(line, '\'');
    if (!first) return -1;
    const char *last = strrchr(first + 1, '\'');
    if (!last) return -1;
    size_t n = (size_t)(last - first - 1);
    if (n + 1 > size) return -1;
    memcpy(out, first + 1, n);
    out[n] = 0;
    return 0;
}

int mainui_sound_theme_file(const char *theme, int chosen, char *out, size_t size) {
    int resources = (chosen && strcmp(theme, "../res/") != 0) || theme[0] == '/';
    int n = resources ? snprintf(out, size, "%s/skin/osdd_resource/", theme)
                                      : snprintf(out, size, "%s", theme);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

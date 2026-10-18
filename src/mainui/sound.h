#ifndef BRICKOS_MAINUI_SOUND_H
#define BRICKOS_MAINUI_SOUND_H

#include <stddef.h>
#include <stdint.h>

// The official frontend's menu sound rules, without SDL (task 5.3; the s1 trace and static
// analysis of its init_skin, wait_event, draw_nav_bar, set_bgm_volume, reopen_audio, and main):
// - It opens the mixer at 44100 Hz, 16-bit, stereo, with 64-sample chunks, keeps 4 channels,
//   loads the theme's sound/click.wav and sound/bgm.mp3, and plays the music without end.
// - Button-down and nonneutral hat events click before action filtering. Releases,
//   axis and keyboard events do not click in wait_event; the repeat worker clicks
//   when it produces direction repeats. Enable Keytone is read before the action.
// - When keymon reports a volume button, the click plays 150 ms after the last report, with
//   Enable Keytone on or off, and the report is cleared.
// - When a Bluetooth audio device comes or goes, and on waking up, it opens the mixer again, on
//   bluealsa while an audio device is connected.

#define MAINUI_SOUND_RATE 44100
#define MAINUI_SOUND_RATE_48K 48000       // on bluealsa, when /mnt/SDCARD/bt_headphone_48000 exists
#define MAINUI_SOUND_CHUNK 64
#define MAINUI_SOUND_CHANNELS 4
#define MAINUI_SOUND_CLICK_DELAY_MS 150

enum mainui_sound_input {
    MAINUI_SOUND_INPUT_OTHER,
    MAINUI_SOUND_INPUT_BUTTON_DOWN,
    MAINUI_SOUND_INPUT_HAT,
    MAINUI_SOUND_INPUT_REPEAT,
};
// The raw-event click decision, before dispatch. value is the hat position.
int mainui_sound_input_click(enum mainui_sound_input source, int value, int enabled);

// SDL_mixer's music volume (0..128) for BGM Volume (0..20): bgmvol * 64 / 20, so the setting's
// top is half of the mixer's top (static: set_bgm_volume; s1: 1 gave 3, 2 gave 6).
int mainui_sound_bgm_volume(int bgmvol);

// The bluealsa mixer level (0..127) for the system volume (0..20): vol * 127 / 20.
int mainui_sound_bluealsa_volume(int vol);

// The control named by a line of `amixer scontents -D bluealsa`: the text between the first and
// the last single quote of a line that holds "Simple mixer control ". Returns 0, or -1.
int mainui_sound_mixer_control(const char *line, char *out, size_t size);

// What MainUI writes to /tmp/trimui_osd/theme_file and /tmp/trimui_music/theme_file for a theme
// value: at start, an absolute theme gives "<theme>/skin/osdd_resource/" and a relative one
// ("../res/") itself. Selecting Default also publishes "../res/"; a chosen SD theme
// publishes "<theme>/skin/osdd_resource/", retaining its doubled slash.
int mainui_sound_theme_file(const char *theme, int chosen, char *out, size_t size);

#endif

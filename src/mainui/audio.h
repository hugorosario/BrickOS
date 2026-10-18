#ifndef BRICKOS_MAINUI_AUDIO_H
#define BRICKOS_MAINUI_AUDIO_H

#include "catalog.h"

#include <SDL_mixer.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

// The menu sounds and the background music, played with SDL_mixer as the official frontend
// plays them (the rules are in sound.h). The installed theme's sound/click.wav and sound/bgm.mp3
// are used, with the Default theme's files when a theme has none.
struct mainui_audio {
    int open;                 // the mixer is open (the official plays nothing otherwise)
    Mix_Chunk *click;
    Mix_Music *music;
    int volume;               // the music volume for BGM Volume, applied again after a reopen
    int playing;              // the music plays: only while its volume is above 0
    atomic_int clicks;
    atomic_int volume_failures;
    // The one-shot volume worker is joined before replacing it or releasing audio.
    pthread_t volume_thread;
    pthread_mutex_t volume_mutex;
    pthread_cond_t volume_cond;
    struct timespec volume_until;
    int volume_ready, volume_live, volume_cancel;
    char theme[MAINUI_THEME_PATH_BYTES]; // the theme the sounds came from
    char route[18];           // the Bluetooth audio device the sound goes to; "" for the speaker
    int dry;                  // records bluealsa's commands and files instead of carrying them out
    FILE *record;
    // Runs a command, and runs one keeping what it prints (amixer); NULL in a dry run.
    void *context;
    int (*run)(void *context, const char *command);
    int (*capture)(void *context, const char *command, char *out, size_t size);
};

// Opens the mixer on the speaker, or on bluealsa when route names a connected audio device,
// loads the theme's sounds, and starts the music at BGM Volume.
void mainui_audio_open(struct mainui_audio *a, const struct mainui_fs *fs, const char *theme, const char *route, int bgmvol);
// The key click, while Enable Keytone is on (the caller checks), and the volume buttons' click.
void mainui_audio_click(struct mainui_audio *a);
// keymon's positive volume report: replace the owned 150 ms click timer.
// Returns -1 on scheduling failure; dry mode creates no worker.
int mainui_audio_volume_report(struct mainui_audio *a);
// BGM Volume, 0..20.
void mainui_audio_volume(struct mainui_audio *a, int bgmvol);
// A setting callback re-applies even an unchanged level; periodic refresh stays cached.
void mainui_audio_apply_volume(struct mainui_audio *a, int bgmvol);
// A chosen theme: its sounds replace the old ones and the music starts again.
void mainui_audio_theme(struct mainui_audio *a, const struct mainui_fs *fs, const char *theme);
// Opens the mixer again, on the device route names (a Bluetooth audio device that came or went,
// or the wake-up after a suspend); vol is the system volume, for bluealsa's level.
void mainui_audio_reopen(struct mainui_audio *a, const char *route, int vol);
void mainui_audio_close(struct mainui_audio *a);

#endif

#define _POSIX_C_SOURCE 200809L
#include "audio.h"

#include "assets.h"
#include "sound.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#define SUSPENDING "/tmp/system_suspend"
#define HEADPHONE_48K "/mnt/SDCARD/bt_headphone_48000"
#define VOLUME_CONTROL "/tmp/bt_alsa_volume_dev"

static void note(struct mainui_audio *a, const char *format, const char *text) {
    if (!a->record) return;
    fprintf(a->record, format, text);
    fputc('\n', a->record);
    fflush(a->record);
}

static void pause_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

static int open_mixer(struct mainui_audio *a, int rate) {
    a->open = Mix_OpenAudio(rate, AUDIO_S16LSB, 2, MAINUI_SOUND_CHUNK) == 0;
    if (a->record) fprintf(a->record, "audio open rate=%d route=%s %s\n", rate, a->route[0] ? "bluealsa" : "speaker",
                           a->open ? "ok" : SDL_GetError());
    return a->open;
}

static void audio_lock(struct mainui_audio *a) { if (a->volume_ready) pthread_mutex_lock(&a->volume_mutex); }
static void audio_unlock(struct mainui_audio *a) { if (a->volume_ready) pthread_mutex_unlock(&a->volume_mutex); }

static void click_locked(struct mainui_audio *a, int require_open) {
    if ((require_open && !a->open) || !a->click) return;
    Mix_HaltChannel(-1);
    if (Mix_PlayChannel(-1, a->click, 0) < 0) note(a, "audio click failed %s", SDL_GetError());
    ++a->clicks;
}

static void timer_failure(struct mainui_audio *a, const char *operation, int error) {
    ++a->volume_failures;
    if (a->record) fprintf(a->record, "audio volume timer failed operation=%s error=%d\n", operation, error);
}

static void *volume_worker(void *context) {
    struct mainui_audio *a = context;
    pthread_mutex_lock(&a->volume_mutex);
    while (!a->volume_cancel) {
        int result = pthread_cond_timedwait(&a->volume_cond, &a->volume_mutex, &a->volume_until);
        if (a->volume_cancel) break;
        if (result == ETIMEDOUT) {
            // Stock's volume callback checks the chunk, independently of keytone
            // and the mixer-open flag; SDL_mixer owns a refused playback attempt.
            click_locked(a, 0);
            break;
        }
        if (result) { timer_failure(a, "wait", result); break; }
    }
    pthread_mutex_unlock(&a->volume_mutex);
    return NULL;
}

static void volume_stop(struct mainui_audio *a) {
    if (!a->volume_live) return;
    pthread_mutex_lock(&a->volume_mutex);
    a->volume_cancel = 1;
    pthread_cond_signal(&a->volume_cond);
    pthread_mutex_unlock(&a->volume_mutex);
    pthread_join(a->volume_thread, NULL);
    a->volume_live = 0;
}

int mainui_audio_volume_report(struct mainui_audio *a) {
    if (a->dry) return 0;
    if (a->volume_live) {
        // Keep ownership after expiry, matching the retained stock timer ID.
        volume_stop(a);
        Mix_HaltChannel(0);
    }
    if (!a->volume_ready) {
        pthread_condattr_t attr;
        int result = pthread_mutex_init(&a->volume_mutex, NULL);
        if (result) { timer_failure(a, "mutex", result); return -1; }
        result = pthread_condattr_init(&attr);
        if (!result) {
            result = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
            if (!result) result = pthread_cond_init(&a->volume_cond, &attr);
            pthread_condattr_destroy(&attr);
        }
        if (result) { pthread_mutex_destroy(&a->volume_mutex); timer_failure(a, "condition", result); return -1; }
        a->volume_ready = 1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &a->volume_until)) { timer_failure(a, "clock", errno); return -1; }
    a->volume_until.tv_nsec += MAINUI_SOUND_CLICK_DELAY_MS * 1000000L;
    if (a->volume_until.tv_nsec >= 1000000000L) { ++a->volume_until.tv_sec; a->volume_until.tv_nsec -= 1000000000L; }
    a->volume_cancel = 0;
    int result = pthread_create(&a->volume_thread, NULL, volume_worker, a);
    if (result) { timer_failure(a, "create", result); return -1; }
    a->volume_live = 1;
    return 0;
}

static int sound_path(const struct mainui_fs *fs, const char *theme, const char *relative, char *out, size_t size) {
    char device_path[1100];
    out[0] = 0;
    if (mainui_asset_find(fs, theme, relative, device_path, sizeof(device_path)) < 0) return -1;
    const char *root = fs && fs->root && device_path[0] == '/' ? fs->root : "";
    int n = snprintf(out, size, "%s%s", root, device_path);
    if (n < 0 || (size_t)n >= size) { out[0] = 0; return -1; }
    return 0;
}

static void load_sounds(struct mainui_audio *a, const struct mainui_fs *fs, const char *theme) {
    char path[2200];
    snprintf(a->theme, sizeof(a->theme), "%s", theme);
    a->click = sound_path(fs, theme, "sound/click.wav", path, sizeof(path)) == 0 ? Mix_LoadWAV(path) : NULL;
    note(a, a->click ? "audio click %s" : "audio click missing %s", path);
    a->music = sound_path(fs, theme, "sound/bgm.mp3", path, sizeof(path)) == 0 ? Mix_LoadMUS(path) : NULL;
    note(a, a->music ? "audio music %s" : "audio music missing %s", path);
}

static void store_route(struct mainui_audio *a, const char *route) {
    if (!route) route = "";
    size_t length = strnlen(route, sizeof(a->route) - 1);
    // Wake calls reopen with a->route itself. A printf destination must not overlap its source.
    memmove(a->route, route, length);
    a->route[length] = 0;
}

void mainui_audio_open(struct mainui_audio *a, const struct mainui_fs *fs, const char *theme, const char *route, int bgmvol) {
    store_route(a, route);
    if (a->route[0]) SDL_setenv("AUDIODEV", "bluealsa", 1);
    if (open_mixer(a, a->route[0] && access(HEADPHONE_48K, F_OK) == 0 ? MAINUI_SOUND_RATE_48K : MAINUI_SOUND_RATE))
        Mix_AllocateChannels(MAINUI_SOUND_CHANNELS);
    load_sounds(a, fs, theme);
    a->playing = 0;
    a->volume = -1;
    mainui_audio_volume(a, bgmvol);
}

// The music plays only while it can be heard: at volume 0 it is stopped, and it starts again
// from its beginning when the volume rises.
static void music_follows_volume(struct mainui_audio *a) {
    if (a->music && a->volume > 0 && !a->playing) a->playing = Mix_PlayMusic(a->music, -1) == 0;
    else if (a->playing && a->volume <= 0) {
        Mix_HaltMusic();
        a->playing = 0;
    }
}

void mainui_audio_click(struct mainui_audio *a) {
    audio_lock(a);
    click_locked(a, 1);
    audio_unlock(a);
}

void mainui_audio_apply_volume(struct mainui_audio *a, int bgmvol) {
    int volume = mainui_sound_bgm_volume(bgmvol);
    a->volume = volume;
    music_follows_volume(a);
    Mix_VolumeMusic(volume);
    if (a->record) fprintf(a->record, "audio music volume=%d\n", volume);
}

void mainui_audio_volume(struct mainui_audio *a, int bgmvol) {
    if (mainui_sound_bgm_volume(bgmvol) != a->volume) mainui_audio_apply_volume(a, bgmvol);
}

void mainui_audio_theme(struct mainui_audio *a, const struct mainui_fs *fs, const char *theme) {
    if (strcmp(a->theme, theme) == 0) return;
    audio_lock(a);
    Mix_FreeMusic(a->music);
    Mix_FreeChunk(a->click);
    a->playing = 0;
    load_sounds(a, fs, theme);
    music_follows_volume(a);
    audio_unlock(a);
}

// The level of the bluealsa control keymon's volume buttons then use: its name is kept in
// /tmp/bt_alsa_volume_dev (the last one amixer lists), and it is set to the system volume.
static void bluealsa_level(struct mainui_audio *a, int vol) {
    static char listing[8192];
    char control[256] = "", command[512];
    if (a->dry || !a->capture || a->capture(a->context, "amixer scontents -D bluealsa", listing, sizeof(listing)) < 0) {
        note(a, "audio bluealsa level %s", a->dry ? "(dry run)" : "unread");
        return;
    }
    for (char *line = strtok(listing, "\n"); line; line = strtok(NULL, "\n")) mainui_sound_mixer_control(line, control, sizeof(control));
    if (!control[0]) { note(a, "audio bluealsa level %s", "no control"); return; }
    FILE *f = fopen(VOLUME_CONTROL, "w");
    if (f) {
        fputs(control, f);
        fclose(f);
    }
    snprintf(command, sizeof(command), "amixer -D bluealsa sset \"%s\" %d", control, mainui_sound_bluealsa_volume(vol));
    note(a, "audio %s", command);
    if (a->run) a->run(a->context, command);
}

void mainui_audio_reopen(struct mainui_audio *a, const char *route, int vol) {
    store_route(a, route);
    // Not while keymon suspends the console; the wake-up reopens it.
    if (access(SUSPENDING, F_OK) == 0) { note(a, "audio reopen %s", "skipped while suspending"); return; }
    audio_lock(a);
    Mix_CloseAudio();
    int rate = MAINUI_SOUND_RATE;
    if (a->route[0]) {
        SDL_setenv("AUDIODEV", "bluealsa", 1);
        if (access(HEADPHONE_48K, F_OK) == 0) rate = MAINUI_SOUND_RATE_48K;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        pause_ms(50);
        SDL_InitSubSystem(SDL_INIT_AUDIO);
        pause_ms(50);
    } else {
        SDL_setenv("AUDIODEV", "default", 1);
    }
    // Closing the mixer stopped the music.
    a->playing = 0;
    if (open_mixer(a, rate)) {
        music_follows_volume(a);
        // Retain the persisted BGM level after the mixer resets its volume on opening.
        int volume = a->volume;
        a->volume = -1;
        Mix_VolumeMusic(volume);
        a->volume = volume;
    } else {
        Mix_PauseMusic();
    }
    audio_unlock(a);
    if (a->route[0]) bluealsa_level(a, vol);
    else if (!a->dry) unlink(VOLUME_CONTROL);
}

void mainui_audio_close(struct mainui_audio *a) {
    volume_stop(a);
    Mix_HaltChannel(-1);
    Mix_HaltMusic();
    Mix_FreeChunk(a->click);
    Mix_FreeMusic(a->music);
    a->click = NULL;
    a->music = NULL;
    a->playing = 0;
    if (a->open) Mix_CloseAudio();
    a->open = 0;
    if (a->volume_ready) {
        pthread_cond_destroy(&a->volume_cond);
        pthread_mutex_destroy(&a->volume_mutex);
        a->volume_ready = 0;
    }
}

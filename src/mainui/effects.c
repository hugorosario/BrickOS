#include "effects.h"

#include "network.h"
#include "json.h"

#include <stdio.h>
#include <string.h>

const int mainui_volume_mixer[21] = {63, 36, 34, 32, 30, 28, 26, 24, 22, 20, 19, 17, 15, 13, 11, 9, 7, 5, 3, 1, 0};
const int mainui_lcd_brightness[11] = {4, 21, 38, 55, 72, 90, 107, 124, 141, 158, 176};
const int mainui_motor_voltage[6] = {500000, 2000000, 2500000, 3000000, 3300000, 3500000};

#define LED "/sys/class/led_anim/"
#define DISP "/sys/devices/virtual/disp/disp/attr/"

struct writer {
    const struct mainui_effect_ops *ops;
    int count;
    int failed;
};

static void shm(struct writer *w, int index, int value) {
    if (w->failed || !w->ops->shm_set) return;
    if (w->ops->shm_set(w->ops->context, index, value) != 0) w->failed = 1;
    else ++w->count;
}

static void file_number(struct writer *w, const char *path, int value) {
    char text[32];
    if (w->failed || !w->ops->write_file) return;
    snprintf(text, sizeof(text), "%d", value);
    if (w->ops->write_file(w->ops->context, path, text) != 0) w->failed = 1;
    else ++w->count;
}

static void run(struct writer *w, const char *command) {
    if (w->failed || !w->ops->run) return;
    if (w->ops->run(w->ops->context, command) != 0) w->failed = 1;
    else ++w->count;
}

static int in_range(int value, int count) { return value >= 0 && value < count; }

static int keymon_value(const struct mainui_effect_ops *ops, int index, int fallback) {
    int value;
    return ops->shm_get && ops->shm_get(ops->context, index, &value) == 0 ? value : fallback;
}

static int led_level(const struct mainui_effect_ops *ops, const int *settings, int index,
                     enum mainui_setting setting) {
    int value = keymon_value(ops, index, settings ? settings[setting] : 0);
    return value < 0 ? 0 : value > 10 ? 10 : value;
}

int mainui_effects_letter_toast(const struct mainui_effect_ops *ops, const char *message, int directory) {
    if (!ops || !ops->write_file || !message || !message[0]) return -1;
    if (!directory && (message[1] || (unsigned char)message[0] >= 128)) return -1;
    char escaped[1024], request[1400];
    if (mainui_json_escape(escaped, sizeof(escaped), message, strlen(message)) < 0) return -1;
    int n = snprintf(request, sizeof(request),
        "{\"type\":\"%s\",\"id\":\"com.trimui.osd.msg.lettergroup\",\"duration\":2000,\"size\":0,"
        "\"x\":490,\"y\":30,\"w\":80,\"h\":80,\"message\":%s,\"font\":\"\",\"bg\":\"\",\"icon\":\"\","
        "\"fontsize\":24,\"fontcolor\":\"FFFFFFFF\"}\n", directory ? "default" : "letter", escaped);
    if (n < 0 || (size_t)n >= sizeof(request)) return -1;
    return ops->write_file(ops->context, "/tmp/trimui_osd/osd_toast_msg", request) == 0 ? 1 : -1;
}

int mainui_effects_theme_preview(const struct mainui_effect_ops *ops, const char *preview) {
    if (!ops || !ops->write_file || !preview || preview[0] != '/' || strlen(preview) >= 512) return -1;
    char escaped[3074], request[3400];
    if (mainui_json_escape(escaped, sizeof(escaped), preview, strlen(preview)) < 0) return -1;
    int n = snprintf(request, sizeof(request),
        "{\"type\":\"default\",\"id\":\"com.trimui.osd.msg.themepreview\",\"duration\":2000,\"size\":0,"
        "\"x\":690,\"y\":200,\"w\":80,\"h\":80,\"message\":\" \",\"font\":\"\",\"icon\":\"\",\"bg\":%s,"
        "\"fontsize\":24,\"fontcolor\":\"FFFFFFFF\"}\n", escaped);
    if (n < 0 || (size_t)n >= sizeof(request)) return -1;
    return ops->write_file(ops->context, "/tmp/trimui_osd/osd_toast_msg", request) == 0 ? 1 : -1;
}

int mainui_effects_launch_keymap(const struct mainui_effect_ops *ops, int fallback) {
    int keymap = fallback, current;
    if (ops->shm_get && ops->shm_get(ops->context, MAINUI_SHM_KEYMAP, &current) == 0) keymap = current;
    static const char normal[] = "A,B,X,Y,L,R", rotated[] = "B,Y,A,X,L,R";
    int count = 0, failed = 0;
    if (ops->write_bytes) {
        if (ops->write_bytes(ops->context, "/sys/module/gpio_keys_polled/parameters/button_config",
                             keymap == 1 ? rotated : normal, sizeof(normal)) != 0) failed = 1;
        else ++count;
    }
    if (ops->run) {
        if (ops->run(ops->context, keymap == 1 ? "touch /var/trimui_inputd/rotate_270" :
                                               "rm -f /var/trimui_inputd/rotate_270") != 0) failed = 1;
        else ++count;
    }
    return failed ? -1 : count;
}

int mainui_effects_apply(const struct mainui_effect_ops *ops, const struct mainui_effect *effect,
                         const int settings[MAINUI_SET_COUNT]) {
    struct writer w = { ops, 0, 0 };
    if (effect->kind != MAINUI_EFFECT_SET) return 0;
    int v = effect->value;
    switch ((enum mainui_setting)effect->setting) {
    case MAINUI_SET_VOL:
        // A volume step clears mute and sets the mixer by the measured curve (g01, g11).
        // Open: whether MainUI or keymon writes the mixer; the resulting state is the same.
        if (!in_range(v, 21)) return -1;
        shm(&w, MAINUI_SHM_VOL, v);
        shm(&w, MAINUI_SHM_MUTE, 0);
        if (!w.failed && ops->mixer_set) {
            if (ops->mixer_set(ops->context, MAINUI_MIXER_DIGITAL_VOLUME, mainui_volume_mixer[v]) != 0) w.failed = 1;
            else ++w.count;
        }
        break;
    case MAINUI_SET_BGMVOL: shm(&w, MAINUI_SHM_BGMVOL, v); break;
    case MAINUI_SET_RUMBLELEVEL:
        if (!in_range(v, 6)) return -1;
        shm(&w, MAINUI_SHM_RUMBLE, v);
        file_number(&w, "/sys/class/motor/voltage", mainui_motor_voltage[v]);
        break;
    case MAINUI_SET_BRIGHTNESS:
        if (!in_range(v, 11)) return -1;
        shm(&w, MAINUI_SHM_BRIGHTNESS, v);
        if (!w.failed && ops->lcd_brightness) {
            if (ops->lcd_brightness(ops->context, mainui_lcd_brightness[v]) != 0) w.failed = 1;
            else ++w.count;
        }
        break;
    case MAINUI_SET_LEDSWITCH:
        shm(&w, MAINUI_SHM_LEDSWITCH, v);
        file_number(&w, LED "enable", v ? 1 : 0);
        if (v) {
            file_number(&w, LED "max_scale", 8 * led_level(ops, settings, MAINUI_SHM_TOPLED, MAINUI_SET_TOPLED));
            file_number(&w, LED "max_scale_lr", 10 * led_level(ops, settings, MAINUI_SHM_TAILLIGHT, MAINUI_SET_SHOULDERLED));
            file_number(&w, LED "max_scale_f1f2", 48 * led_level(ops, settings, MAINUI_SHM_F1F2, MAINUI_SET_F1F2LED) / 10);
        }
        break;
    case MAINUI_SET_TOPLED:
        // max_scale = 8 x level, measured with the LEDs on (g03).
        shm(&w, MAINUI_SHM_TOPLED, v);
        file_number(&w, LED "max_scale", keymon_value(ops, MAINUI_SHM_LEDSWITCH,
            settings ? settings[MAINUI_SET_LEDSWITCH] : 1) ? 8 * v : 0);
        break;
    case MAINUI_SET_SHOULDERLED:
        shm(&w, MAINUI_SHM_TAILLIGHT, v);
        file_number(&w, LED "max_scale_lr", keymon_value(ops, MAINUI_SHM_LEDSWITCH,
            settings ? settings[MAINUI_SET_LEDSWITCH] : 1) ? 10 * v : 0);
        break;
    case MAINUI_SET_F1F2LED:
        // floor(4.8 x level): 19, 24, 28, 33, 38, 43, 48 for 4..10 (g12).
        shm(&w, MAINUI_SHM_F1F2, v);
        file_number(&w, LED "max_scale_f1f2", keymon_value(ops, MAINUI_SHM_LEDSWITCH,
            settings ? settings[MAINUI_SET_LEDSWITCH] : 1) ? v * 48 / 10 : 0);
        break;
    case MAINUI_SET_LEDPOLICY: shm(&w, MAINUI_SHM_LEDPOLICY, v); break;
    case MAINUI_SET_DIMTIME: shm(&w, MAINUI_SHM_DIMTIME, v); break;
    case MAINUI_SET_COLORCONTRAST: file_number(&w, DISP "enhance_contrast", 10 * v); break;
    case MAINUI_SET_COLORSATURATION: file_number(&w, DISP "enhance_saturation", 10 * v); break;
    case MAINUI_SET_COLORBRIGHTNESS: file_number(&w, DISP "enhance_bright", 10 * v); break;
    case MAINUI_SET_COLORTEMPERATURE: file_number(&w, DISP "color_temperature", (v - 10) * 5); break;
    case MAINUI_SET_KEYMAP: shm(&w, MAINUI_SHM_KEYMAP, v); break;
    case MAINUI_SET_BCONFIRM: shm(&w, MAINUI_SHM_BCONFIRM, v); break;
    case MAINUI_SET_OSDSHORTCUT: shm(&w, MAINUI_SHM_OSDSHORTCUT, v); break;
    case MAINUI_SET_NETPLAY: shm(&w, MAINUI_SHM_NETPLAY, v); break;
    case MAINUI_SET_MENUSTYLEL1: shm(&w, MAINUI_SHM_MENUSTYLEL1, v); break;
    case MAINUI_SET_MENUSTYLEL2: shm(&w, MAINUI_SHM_MENUSTYLEL2, v); break;
    case MAINUI_SET_WIFI:
        // MainUI asks hardwareservice, which stops or starts wpa_supplicant and udhcpc (static:
        // touch /tmp/system/wifi_turn_on and wifi_turn_off in MainUI; g08 saw the services go).
        shm(&w, MAINUI_SHM_WIFI, v);
        run(&w, v ? "touch /tmp/system/wifi_turn_on" : "touch /tmp/system/wifi_turn_off");
        break;
    case MAINUI_SET_BLUETOOTH:
        // hardwareservice starts hciattach, bluetoothd, and trimui_btmanager, or stops them
        // (static; the w1 session saw the services start and go).
        shm(&w, MAINUI_SHM_BLUETOOTH, v);
        run(&w, v ? "touch /tmp/system/bluetooth_turn_on" : "touch /tmp/system/bluetooth_turn_off");
        break;
    case MAINUI_SET_HOSTAP: {
        // MainUI runs the hotspot itself: it stops the Wi-Fi client and starts hostapd and udhcpd,
        // or the reverse (static; g10 saw hostapd and udhcpd start and the client stop).
        const char *lines[8];
        shm(&w, MAINUI_SHM_HOSTAP, v);
        int count = mainui_hotspot_commands(v, settings ? settings[MAINUI_SET_WIFI] : 0, lines, 8);
        // Every line runs, as the official's system() calls do: a killall with nothing to stop
        // exits 1 and must not keep hostapd from starting (2026-10-07, hotspot live check).
        for (int i = 0; i < count && !w.failed && ops->run; ++i)
            if (ops->run(ops->context, lines[i]) == 0) ++w.count;
        break;
    }
    default:
        return 0;
    }
    return w.failed ? -1 : w.count;
}

#define _POSIX_C_SOURCE 200809L
#include "actions.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

// USB Mode runs the role script of its value (Host, Dock, Data).
static const char *const usb_scripts[3] = {
    "/usr/trimui/bin/usb_host.sh", "/usr/trimui/bin/usb_dock.sh", "/usr/trimui/bin/usb_device.sh",
};

// removeuserdata at 0x496094, after removeAllDBFiles. Its three malformed unlink
// arguments include "rm -f " in the filename; execute those intended removals.
// Keep synchronization, history, Wi-Fi restore and hotspot defaults before reboot.
static const char *const factory_reset[] = {
    "rm -f /mnt/SDCARD/Roms/ARCADE//.mame4all/configs/*.cfg",
    "rm -f /mnt/SDCARD/Roms/FC//.fceux/fceux.cfg",
    "rm -f /mnt/SDCARD/Roms/FC//.fceux/cfg/*.cfg",
    "sync",
    "rm -f /mnt/romwinidx.json",
    "rm -f /mnt/SDCARD/Roms/favourite2.json",
    "rm -f /mnt/UDISK/system.json",
    "rm -f /mnt/UDISK/.inited.lang",
    "rm -f /mnt/UDISK/player.json",
    "rm -f /mnt/UDISK/moonlight.json",
    "rm -f /mnt/SDCARD/Roms/recentlist.json",
    "sync",
    "cp /etc/wifi/wpa_supplicant_original.conf /etc/wifi/wpa_supplicant.conf",
    "sed -i 's/^ssid=.*/ssid=TRIMUI-AP/;s/^wpa_passphrase=.*/wpa_passphrase=12345678/' /etc/wifi/hostapd.conf",
    "sync",
    "reboot",
};

int mainui_actions_commands(const struct mainui_effect *effect, const char **commands, int max) {
    int n = 0;
#define ADD(command) do { if (n < max) commands[n] = (command); ++n; } while (0)
    switch (effect->kind) {
    case MAINUI_EFFECT_SET:
        switch (effect->setting) {
        case MAINUI_SET_USBMODE:
            if (effect->value >= 0 && effect->value < 3) ADD(usb_scripts[effect->value]);
            break;
        case MAINUI_SET_ENABLESSH:
            ADD(effect->value ? "/etc/init.d/sshd start" : "/etc/init.d/sshd stop");
            break;
        case MAINUI_SET_USENETWORKTIME:
            if (effect->value) { ADD("/etc/init.d/sysntpd enable"); ADD("/etc/init.d/ntpd restart"); }
            else { ADD("/etc/init.d/sysntpd disable"); ADD("/etc/init.d/ntpd stop"); }
            break;
        default:
            break;
        }
        break;
    case MAINUI_EFFECT_IO_TEST:
        // keymon reads the marker while the IO test runs; MainUI removes it on the way out.
        ADD(effect->value ? "touch /tmp/keytestmode" : "rm -f /tmp/keytestmode");
        break;
    case MAINUI_EFFECT_FLASH_MODE:
        ADD("reboot efex");
        break;
    case MAINUI_EFFECT_POWER_OFF:
        // The frontend worker checks /tmp/poweroff_flag for hold/marker sources.
        // Settings always calls this command; neither source writes the marker.
        ADD("poweroff");
        break;
    case MAINUI_EFFECT_REBOOT:
        // The power dialog's Reboot, under the same worker and marker rules as Power off.
        ADD("reboot");
        break;
    case MAINUI_EFFECT_FACTORY_RESET:
        for (unsigned i = 0; i < sizeof(factory_reset) / sizeof(factory_reset[0]); ++i) ADD(factory_reset[i]);
        break;
    default:
        break;
    }
#undef ADD
    return n < max ? n : max;
}

int mainui_actions_apply(const struct mainui_runner *runner, const struct mainui_effect *effect) {
    if (effect->kind == MAINUI_EFFECT_APPLY_DATE) {
        int y, mo, d, h, mi, s;
        if (strlen(effect->text) != 19 || strspn(effect->text, "0123456789- :") != 19 ||
            effect->text[4] != '-' || effect->text[7] != '-' || effect->text[10] != ' ' ||
            effect->text[13] != ':' || effect->text[16] != ':' ||
            sscanf(effect->text, "%4d-%2d-%2d %2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6 ||
            y < 1900 || y > 3000 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
            h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59 ||
            effect->value < -1 || effect->value > 1) return -1;
        struct tm calendar = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
                               .tm_hour = h, .tm_min = mi, .tm_sec = s, .tm_isdst = effect->value };
        if (mktime(&calendar) == (time_t)-1) return -1;
        char normalized[32], command[64];
        if (!strftime(normalized, sizeof(normalized), "%Y-%m-%d %H:%M:%S", &calendar)) return -1;
        snprintf(command, sizeof(command), "date -s \"%s\"", normalized);
        if (runner->run(runner->context, command) != 0) return -1;
        return runner->run(runner->context, "hwclock -w") == 0 ? 2 : -1;
    }
    const char *commands[16];
    int n = mainui_actions_commands(effect, commands, 16);
    for (int i = 0; i < n; ++i)
        if (runner->run(runner->context, commands[i]) != 0) return -1;
    return n;
}

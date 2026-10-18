#ifndef BRICKOS_MAINUI_NETWORK_H
#define BRICKOS_MAINUI_NETWORK_H

#include <stddef.h>
#include <stdint.h>

// The network, Bluetooth, and clock adapters the Settings screens use, as the official MainUI
// drives them (static: the strings of MainUI, trimui_btmanager, and hardwareservice; observed:
// session w1). This file parses the programs' output and builds their command lines; the
// frontend runs them.
//
// - Wi-Fi: wpa_cli -p /etc/wifi/sockets -i wlan0 with scan, scan_result, status, list_network,
//   add_network, set_network, enable_network, select_network, and save_config. The switch is a
//   request to hardwareservice: touch /tmp/system/wifi_turn_on or wifi_turn_off.
// - Hotspot: MainUI stops the Wi-Fi client and runs hostapd and udhcpd itself, and edits
//   /etc/wifi/hostapd.conf for the name and password.
// - Bluetooth: touch /tmp/system/bluetooth_turn_on or _off (hardwareservice starts or stops the
//   services); requests go to trimui_btmanager through the FIFO /tmp/bluetooth_request and JSON
//   answers come back through /tmp/bluetooth_result. /tmp/bluetooth_ready means it is up.
// - Clock: the Time Zone screen browses /usr/share/zoneinfo; a zone is linked as /tmp/localtime
//   (where /etc/localtime points) and stored as "timezone" in system.json, relative to
//   zoneinfo ("Europe/Lisbon"). MainUI relinks a stored zone when it starts; without one the
//   screen shows Default and the boot script's link stays (static: main and FileAction::dowork).
//   Setting the date runs date -s "YYYY-MM-DD HH:MM:SS" and hwclock -w.

#define MAINUI_WPA_CLI "wpa_cli -p /etc/wifi/sockets -i wlan0"
#define MAINUI_SSID_MAX 64
#define MAINUI_BT_TEXT 64
#define MAINUI_ZONEINFO "/usr/share/zoneinfo"

// HallMenu receives udpbcast's JSON on UDP 13588. Items stay in arrival order,
// a repeated label refreshes its timestamp, and silence for more than 3000 ms removes it.
#define MAINUI_NETPLAY_PORT 13588
#define MAINUI_NETPLAY_MAX 64
struct mainui_netplay_host {
    char label[256], iconpath[512], rompath[512], hostip[16], gamelabel[128];
    int gametype;
    uint64_t seen_ms;
    // Frontend-owned image state; these fields never appear in advertisements.
    char coverpath[1024];
    uint64_t cover_after_ms;
    int cover_attempted;
    int cover_temporary;         // only frontend-owned temporary files are removed on expiry
};
int mainui_netplay_parse(const char *text, size_t length, struct mainui_netplay_host *out);
// Formats the six stock advertisement fields, escaping JSON and enforcing the 1024-byte wire limit.
int mainui_netplay_format(const struct mainui_netplay_host *host, char *out, size_t size);
// Reads the last IPv4 address of `ip addr show wlan0`, as createHostFile does.
int mainui_netplay_address(const char *text, char *out, size_t size);
int mainui_netplay_receive(struct mainui_netplay_host *hosts, int *count, const char *text,
                          size_t length, uint64_t now_ms);
int mainui_netplay_expire(struct mainui_netplay_host *hosts, int *count, uint64_t now_ms);
// Returns a nonblocking IPv4 datagram descriptor, or -1. Port 0 is useful in host tests.
int mainui_netplay_listen(unsigned port);
// HTTP image retrieval through the installed wget, with no shell or inherited proxy/library
// environment. Publishes only a complete, nonempty response through a private temporary file.
// The total deadline, optional atomic cancel flag and 4 MiB cap bound work during shutdown.
// Port 80 is the stock contract; another port permits an isolated loopback HTTP fixture.
#define MAINUI_NETPLAY_COVER_MAX (4u * 1024u * 1024u)
int mainui_netplay_cover(const char *ip, const char *iconpath, unsigned port, const char *path,
                         unsigned timeout_ms, const int *cancel);
// Resolves an advertised cover's parent inside this card. Returns its device path, or -1
// for an absent/unsafe parent; the output is suitable for the stock Recent imgpath field.
int mainui_netplay_cover_path(const char *root, const char *iconpath, char *out, size_t size);
// Finds a regular local cover, including safe dot segments and links inside the card.
int mainui_netplay_cover_local(const char *root, const char *iconpath, char *out, size_t size);
// Copies a completed private download into the stock SD path without replacing any existing
// entry. Only image filename extensions are accepted for writes. Returns the regular cover's
// device path; an existing local cover wins a concurrent publication. No directory is created.
int mainui_netplay_cover_cache(const char *root, const char *iconpath, const char *source,
                              char *out, size_t size);

struct mainui_wifi_network {
    char ssid[MAINUI_SSID_MAX];
    int rssi;       // dBm
    int locked;     // WEP or WPA
    int saved_id;   // its wpa_supplicant network id, or -1
    int connected;
};

struct mainui_wifi_status {
    char state[32];              // wpa_state: COMPLETED, DISCONNECTED, SCANNING, ...
    char ssid[MAINUI_SSID_MAX];
    char ip[48];
    int id;                      // the current network id, or -1
};

// scan_result output: networks with a name, one per name (the strongest), strongest first.
// Hidden networks do not show (w1). Returns the count. Open: the official order; the two
// networks of w1 were also in scan order.
int mainui_wifi_parse_scan(const char *text, struct mainui_wifi_network *out, int max);
void mainui_wifi_parse_status(const char *text, struct mainui_wifi_status *status);
// list_network output: the saved networks; sets saved_id on the matching scan entries and
// returns the id saved for ssid, or -1.
int mainui_wifi_saved_id(const char *text, const char *ssid);
// Sets saved_id from list_network output (NULL: none saved) and connected from the status.
void mainui_wifi_mark(struct mainui_wifi_network *networks, int count, const char *list_text, const struct mainui_wifi_status *status);
// The list's signal icon, ic-setting-wifi-01..03, which depends on whether the network is in use,
// and the status bar's, ic-wifi-01..03 (static: WifiMenu and draw_nav_bar; c1 showed 3 at -43
// dBm in both).
#define MAINUI_RSSI_NONE (-200)
int mainui_wifi_level(int rssi, int connected);
int mainui_wifi_bar_level(int rssi);
// The RSSI line of wpa_cli signal_poll, or MAINUI_RSSI_NONE.
int mainui_wifi_parse_signal(const char *text);
// SSIDs in wpa_cli output are escaped (\xNN); decodes in place.
void mainui_wifi_unescape(char *ssid);

// Appends text to out as one shell word in single quotes. Returns 0, or -1 if out is too small.
int mainui_shell_quote(char *out, size_t size, const char *text);

// The commands that connect to network id with a password, or with no key management when
// password is NULL (static: WifiConnectAction, after add_network for a network not saved yet):
// set_network ssid, psk or key_mgmt NONE, select_network, enable_network, save_config. Returns
// the count, at most max, or -1 when a line does not fit.
int mainui_wifi_connect_commands(const char *ssid, const char *password, int id, char lines[][512], int max);
// The commands of a saved network's popup (static: WifiReconnectAction selects it again,
// WifiRemoveAction removes it; both save the configuration). Returns the count, or -1.
int mainui_wifi_reconnect_commands(int id, char lines[][512], int max);
int mainui_wifi_forget_commands(int id, char lines[][512], int max);

// The hotspot and Wi-Fi client commands for the Hotspot switch (static order; open: the exact
// sequence). Returns the count.
int mainui_hotspot_commands(int on, int wifi_on, const char **lines, int max);
// Rewrites hostapd.conf text with a new name (key "ssid") or password (an empty password makes
// an open network). Returns the new length, or -1 when out is too small.
int mainui_hostapd_edit(const char *text, const char *key, const char *value, char *out, size_t size);

struct mainui_bt_device {
    char addr[18];
    char name[MAINUI_BT_TEXT];
    char icon[32];               // bluez icon name: input-gaming, audio-headset, ...
    int connected;
};

// One JSON answer from trimui_btmanager. func names the request it answers ("scan",
// "connect", "remove", "getdevs", "check"). A device list fills devices; a single device fills
// devices[0]. Returns the number of devices, or -1 when the text is not an answer.
struct mainui_bt_answer {
    char func[16];
    char result[16];
    char error[64];
};
int mainui_bt_parse(const char *text, struct mainui_bt_answer *answer, struct mainui_bt_device *devices, int max);
// Finds the next whole JSON object in bytes read from the result FIFO, where answers follow one
// another (open: whether a newline ends each). Sets [start, end) and returns 1, or returns 0
// while no whole object has arrived.
int mainui_bt_next_object(const char *data, size_t length, size_t *start, size_t *end);
// The skin for a device: icon-bluetooth-headphone for an audio icon name, icon-bluetooth for an
// input one, and icon-bluetooth-gamepad for any other, empty included.
const char *mainui_bt_icon(const struct mainui_bt_device *device);

// A time zone entry of a folder of /usr/share/zoneinfo: folders first, then files, each sorted
// by name (w1 showed Africa, America, Antarctica, Arctic, Asia, Atlantic, Australia, Brazil).
struct mainui_zone {
    char name[64];
    int folder;
};
// Sorts entries in place as the Time Zone screen lists them.
void mainui_zone_sort(struct mainui_zone *zones, int count);
// The command that links a zone (relative to zoneinfo); -1 for Default (""), which runs
// nothing, and for a name that is not a plain relative path. Then the command that sets the
// date and time.
int mainui_zone_command(const char *zone, char *out, size_t size);
int mainui_date_command(int year, int month, int day, int hour, int minute, int second, char *out, size_t size);

#endif

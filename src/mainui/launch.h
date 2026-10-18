#ifndef BRICKOS_MAINUI_LAUNCH_H
#define BRICKOS_MAINUI_LAUNCH_H

#include <stddef.h>

// The launch handoff to the stock init. MainUI writes /tmp/cmd_to_run.sh and exits; the init
// then runs `/tmp/.cmdenc` through /root/gameloader when that file exists, or else the command
// file, and starts MainUI again when it returns.
//
// Measured on the official MainUI (task 1.6): a game's command is
//   chmod a+x "<launcher>";  "<launcher>" "<rompath>"
// with two spaces after the semicolon and no final newline, and MainUI exits with status 0.
// An app's command is `cd <folder>; chmod a+x ./<launch>; ./<launch> ` (a2), also exiting 0.
//
// The official MainUI puts paths between double quotes without escaping them. BrickOS escapes
// \ " $ and ` inside the quotes, so a path holding them still reaches the launcher intact; for
// every other path the command is byte-identical to the official one.

enum mainui_launch_result {
    MAINUI_LAUNCH_OK,
    MAINUI_LAUNCH_NO_LAUNCHER,    // the launcher script is missing
    MAINUI_LAUNCH_NO_TARGET,      // the ROM or app directory is missing
    MAINUI_LAUNCH_TOO_LONG,       // the command does not fit
    MAINUI_LAUNCH_WRITE_FAILED,
    MAINUI_LAUNCH_ENCODING_FAILED,
};

// ARE's stock path handoff consists of 16 independent 16-byte blocks containing
// an absolute ROM path followed by zero padding. The adapter performs the actual
// installed hardware cipher; return 0 on success. Tests supply a deterministic
// reversible double, never a substitute device key or an invented encryption scheme.
struct mainui_encoder {
    void *context;
    int (*block)(void *context, const unsigned char input[16], unsigned char output[16], int decrypt);
};
// Rejects paths over 255 bytes, adapter failures and a failed round trip. On any
// failure out is zeroed. This function does not touch hardware except via block.
enum mainui_launch_result mainui_launch_encode_path(const struct mainui_encoder *encoder,
                                                   const char *rompath, unsigned char out[256]);
// Requires a regular ROM and an executable /root/gameloader under root. Publishes
// identical binary bytes to .tmenc and .cmdenc, with .cmdenc last, and removes the
// competing plain handoff/host advertisement. No launch, process exit or init runs.
enum mainui_launch_result mainui_launch_encoded(const char *root, const char *rompath,
                                               const struct mainui_encoder *encoder);
// Stock ClientGameAction types 8..15 use the same path-only payload, without the
// host IP in it. A network-advertised ROM must resolve to a regular file inside
// this SD card before the encoder or publisher can run.
enum mainui_launch_result mainui_launch_encoded_netplay(const char *root, const char *rompath,
                                                       const struct mainui_encoder *encoder);

// Formats a game's command for a launcher and ROM path.
enum mainui_launch_result mainui_launch_game_command(char *out, size_t size, const char *launcher,
                                                     const char *rompath);
// The stock client launch: NET_PARAM="-C <IPv4>" is passed only to this launcher.
enum mainui_launch_result mainui_launch_netplay_command(char *out, size_t size, const char *launcher,
                                                       const char *rompath, const char *hostip);
// Legacy types 5 (ARCADE), 6 (NEOGEO) and 7 (FC) use the firmware RetroArch cores
// directly, with -C rather than NET_PARAM. NULL home selects the stock ROM directory;
// another-platform card supplies the ROM's parent directory instead. Stock returns
// window event 64; that event is not evidence of process exit status 64.
const char *mainui_launch_builtin_core(int type);
// Stock card identification, also used by cache removal during Factory Reset.
int mainui_launch_other_platform_card(const char *root);
enum mainui_launch_result mainui_launch_builtin_netplay_command(char *out, size_t size, int type,
                                                               const char *rompath, const char *hostip,
                                                               const char *home);
// Recent/Favorites relaunch the same core with an empty network parameter or -H.
enum mainui_launch_result mainui_launch_builtin_game_command(char *out, size_t size, int type,
                                                            const char *rompath, int hosting, const char *home);
enum mainui_launch_result mainui_launch_host_command(char *out, size_t size, const char *launcher,
                                                    const char *rompath);
// Formats an app's command from its directory and launch script. A path with a character the
// shell treats specially is quoted; a plain path stays bare, as in the official format.
enum mainui_launch_result mainui_launch_app_command(char *out, size_t size, const char *directory,
                                                    const char *script);
// Formats a port's command from its config.json "launch", the full path of a script:
// `cd <folder>; chmod a+x ./<name>; ./<name> ` with the text as written, since the config holds
// it escaped for the shell (static: loadapp_internal splits it at its last slash; p3).
enum mainui_launch_result mainui_launch_port_command(char *out, size_t size, const char *launch);

// Checks the targets under root (the device root, or a test directory), formats the command,
// and publishes it in root/tmp: a stale /tmp/.cmdenc, which would take the init's priority,
// is removed, and the command file is written through a temporary file and a rename, so the
// init never reads a partial command. On failure nothing is left behind.
enum mainui_launch_result mainui_launch_game(const char *root, const char *launcher, const char *rompath);
enum mainui_launch_result mainui_launch_netplay(const char *root, const char *launcher, const char *rompath,
                                               const char *hostip);
// With NULL home, the publisher also applies sdcardOfOtherPlatform: both MMC
// partitions and a visible regular KERNEL-prefixed SD file select the ROM's parent.
// In that case NEOGEO copies an available /mnt/P2EXT/system/neogeo.zip into the
// ROM's parent if no entry exists there. The copy is atomic, never replaces an
// entry, creates no directories and is best effort, as in the stock launch flow.
enum mainui_launch_result mainui_launch_builtin_netplay(const char *root, int type, const char *rompath,
                                                       const char *hostip, const char *home);
enum mainui_launch_result mainui_launch_builtin_game(const char *root, int type, const char *rompath,
                                                    int hosting, const char *advertisement);
// Full stock types 5..7 handoff: with an encoder, prepare a path (NEOGEO uses
// its basename before the first literal ".zip") and require an executable
// gameloader before cipher calls. A verified payload accompanies the plain core
// command and takes init priority. Cipher failure falls back to an available
// plain core; publication failure clears the entire new handoff. Without an
// encoder this retains the plain-only adapter contract. Stale files are cleared.
enum mainui_launch_result mainui_launch_builtin(const char *root, int type, const char *rompath,
                                               const char *hostip, int hosting, const char *advertisement,
                                               const struct mainui_encoder *encoder);
// The host advertisement is already formatted by the network adapter; NULL means wlan0
// has no IPv4 address. That still launches with -H, as the stock frontend does.
enum mainui_launch_result mainui_launch_host(const char *root, const char *launcher, const char *rompath,
                                            const char *advertisement);
enum mainui_launch_result mainui_launch_app(const char *root, const char *directory, const char *script);
// GameSwitcher's entry, which runs its picker over MainUI and resumes a checkpoint chosen there.
#define MAINUI_GAMESWITCHER_ENTRY "/mnt/SDCARD/BrickOS/gameswitcher/entry.sh"
// 1 when id and state are a checkpoint's names as GameSwitcher's store makes them.
int mainui_launch_gameswitcher_names(const char *id, const char *state);
// Publishes "exec sh '<entry>' resume ID STATE" as the init's command, for names of that form.
enum mainui_launch_result mainui_launch_gameswitcher_resume(const char *root, const char *id, const char *state);
// The script is checked with the shell's backslash escapes read.
enum mainui_launch_result mainui_launch_port(const char *root, const char *launch);

// Removes the command files a previous run left, so an exit without a launch runs nothing.
void mainui_launch_clear(const char *root);

#endif

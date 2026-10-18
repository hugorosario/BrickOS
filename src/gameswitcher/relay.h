#ifndef BRICKOS_GAMESWITCHER_RELAY_H
#define BRICKOS_GAMESWITCHER_RELAY_H
// While GameSwitcher grabs TRIMUI Player1, the volume keys (115 up, 114 down) reach keymon
// through the Brick's sunxi-keyboard node: a kernel node that declares exactly those two keys and
// that no program grabs. keymon reads every input node, so a key written there acts as a physical
// press. The app forwards through it, and the guard that runs after the app releases it.
// Opens the first /dev/input/event* named sunxi-keyboard whose key map has both volume keys:
// write-only, non-blocking and close-on-exec, never grabbed. -1 when there is none.
int gs_relay_open(void);
// Writes one key event and its own SYN_REPORT: 0, or -1 when the write fails.
int gs_relay_key(int fd, unsigned short code, int value);
// Releases both volume keys on fd. The kernel drops the release of a key that is not down.
int gs_relay_release(int fd);
// Releases both volume keys on the relay node, if there is one, after an app that may have ended
// with a key pressed there.
void gs_relay_release_node(void);
#endif

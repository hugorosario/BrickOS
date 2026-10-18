#ifndef BRICKOS_GAMESWITCHER_GUARD_H
#define BRICKOS_GAMESWITCHER_GUARD_H
#include <stddef.h>
struct gs_guard { int fb,disp; size_t bytes; unsigned char *map,*backup; unsigned char layer[256]; };
// Snapshot only the idle overlay slots (both channel-2 panel slots) and layer; never writes the
// game's layer/pages.
int gs_guard_open(struct gs_guard *guard);
// Restores the slots and layer, removes a stale display marker and releases the volume keys on
// keymon's relay node (relay.h), on every path.
int gs_guard_close(struct gs_guard *guard);
// Removes GameSwitcher's display marker at path when it names no running GameSwitcher, as after
// the app was killed: 1 when it removed one. Open and close do this for the real marker.
int gs_guard_clear_stale_marker(const char *path);
#endif

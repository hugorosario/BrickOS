#ifndef BRICKOS_GAMESWITCHER_CLOCK_H
#define BRICKOS_GAMESWITCHER_CLOCK_H
#include "store.h"
struct gs_clock { uint64_t started_ms, completed_ms; int active; };
void gs_clock_init(struct gs_clock *clock);
int gs_clock_start(struct gs_clock *clock,uint64_t monotonic_ms);
int gs_clock_stop(struct gs_clock *clock,uint64_t monotonic_ms);
// Persist completed intervals; caller closes the interval before save/exit/power.
int gs_clock_flush(struct gs_clock *clock,struct gs_store *store,const char *id,const char *session);
#endif

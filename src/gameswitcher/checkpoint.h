#ifndef BRICKOS_GAMESWITCHER_CHECKPOINT_H
#define BRICKOS_GAMESWITCHER_CHECKPOINT_H
#include "store.h"
#include "../tmenu/host.h"
#include <SDL.h>
// Caller must validate the live session and tmenu ABI first. Invoked only on
// the ShowMenu calling thread. 0 means durable publication; -1 forbids transition.
int gs_checkpoint_save(struct gs_store *store,const struct gs_identity *identity,
                       struct tmenu_host *host,SDL_Surface *background,unsigned timeout_ms);
// Produces complete new output while keeping slot 99 locked, without publishing
// history. The coordinator may commit only after receiving this completion.
int gs_checkpoint_prepare(struct gs_store *store,const struct gs_identity *identity,
                          struct tmenu_host *host,SDL_Surface *background,unsigned timeout_ms,struct gs_stage *stage);
void gs_checkpoint_discard(struct gs_stage *stage);
#endif

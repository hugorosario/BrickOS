#ifndef BRICKOS_GAMESWITCHER_BRIDGE_H
#define BRICKOS_GAMESWITCHER_BRIDGE_H
#include "session.h"
#include "../tmenu/host.h"
#include <SDL.h>
int gs_bridge_enabled(void);
int gs_bridge_show(struct tmenu_host *host,SDL_Surface *background,int *result);
int gs_bridge_quit(struct tmenu_host *host,SDL_Surface *background);
void gs_bridge_legacy_finished(void);
struct gs_session *gs_observer_session(void);
struct gs_store *gs_observer_store(void);
void gs_observer_menu(int active,int resume);
void gs_observer_disable(void);
int gs_observer_load(struct gs_stage *stage,unsigned char *memory,size_t bytes,
                     const struct gs_entry *entry,const char *transaction);
int gs_observer_recovery_quit(void);
#endif

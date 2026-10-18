#ifndef BRICKOS_GAMESWITCHER_APP_PROCESS_H
#define BRICKOS_GAMESWITCHER_APP_PROCESS_H
#include "channel.h"
#include <sys/types.h>
int gs_app_spawn(const char *app,const char *sd,const char *name,int owner_channel,pid_t *pid,int *channel);
// Used only for recovery; forced app exit cannot deliver an accepted selection.
int gs_app_recover(pid_t pid);
#endif

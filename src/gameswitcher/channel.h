#ifndef BRICKOS_GAMESWITCHER_CHANNEL_H
#define BRICKOS_GAMESWITCHER_CHANNEL_H
#include "store.h"
#include <stdint.h>
#define GS_CHANNEL_MAGIC UINT32_C(0x47533101)
enum gs_choice { GS_CHOICE_BACK=1, GS_CHOICE_RESUME=2 };
// Passed over a private inherited SOCK_SEQPACKET; never interpreted as a command.
struct gs_choice_message {
    uint32_t magic, action;
    char id[GS_ID], state[GS_ASSET];
};
int gs_channel_parent(int fd);
int gs_channel_send(int fd,enum gs_choice choice,const struct gs_entry *entry);
int gs_channel_read(int fd,struct gs_choice_message *message);
#endif

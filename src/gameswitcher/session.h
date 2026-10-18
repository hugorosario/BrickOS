#ifndef BRICKOS_GAMESWITCHER_SESSION_H
#define BRICKOS_GAMESWITCHER_SESSION_H
#include "store.h"
#include <sys/types.h>
#define GS_SESSION_MAGIC UINT32_C(0x47533203)
enum gs_message_type { GS_HELLO=1, GS_ACTIVE, GS_MENU_ENTER, GS_MENU_ACK, GS_SWITCH, GS_LOAD_RESULT,
                       GS_REQUEST_QUIT, GS_REQUEST_SAVE, GS_SAVE_RESULT, GS_REQUEST_LOAD, GS_LOAD_QUEUED,
                       GS_POWER_SAVE, GS_POWER_READY, GS_POWER_DONE, GS_POWER_WAIT, GS_POWER_QUIT };
struct gs_message {
    uint32_t magic, type, pid, success;
    uint64_t process_start, monotonic_ms;
    char session[GS_ID], id[GS_ID], state[GS_ASSET], transaction[GS_ID];
    uint64_t stage_deadline_ms;
    char stage_state[256],stage_thumbnail[256],stage_transaction[GS_ID];
};
struct gs_session {
    int channel;
    pid_t pid;
    uint64_t process_start;
    struct gs_identity identity;
    char sd[GS_PATH], session[GS_ID], app[GS_PATH], library[GS_PATH], append[GS_PATH], store_name[64];
};
extern const char gs_private_config[];
uint64_t gs_monotonic_ms(void);
int gs_process_start(pid_t pid,uint64_t *start);
int gs_stat_start(const char *stat,uint64_t *start);
int gs_process_matches(pid_t pid,uint64_t start,const struct gs_identity *identity,const char *append);
int gs_session_environment(struct gs_session *session,const void *library_symbol);
int gs_session_core(const struct gs_session *session,const void *core_symbol);
int gs_message_send(const struct gs_session *session,enum gs_message_type type,int success,
                    const struct gs_entry *entry,const char *transaction);
int gs_message_read(const struct gs_session *session,struct gs_message *message);
int gs_message_ready(const struct gs_session *session,const struct gs_stage *stage,const char *transaction);
int gs_message_read_stage(const struct gs_session *session,struct gs_message *message,struct gs_stage *stage);
#endif

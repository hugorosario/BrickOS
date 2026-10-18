#include "clock.h"
#include <errno.h>
#include <string.h>
void gs_clock_init(struct gs_clock *c) { memset(c,0,sizeof(*c)); }
int gs_clock_start(struct gs_clock *c,uint64_t now) {
    if (c->active) return 0;
    c->started_ms=now; c->active=1; return 0;
}
int gs_clock_stop(struct gs_clock *c,uint64_t now) {
    if (!c->active) return 0;
    c->active=0;
    if (now<c->started_ms || UINT64_MAX-c->completed_ms<now-c->started_ms) { errno=EOVERFLOW; return -1; }
    c->completed_ms+=now-c->started_ms; return 0;
}
int gs_clock_flush(struct gs_clock *c,struct gs_store *s,const char *id,const char *session) {
    if (c->active) { errno=EBUSY; return -1; }
    return gs_store_time(s,id,session,c->completed_ms);
}

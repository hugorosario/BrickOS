#include "gesture.h"
void gs_gesture_lose(struct gs_gesture *g) { g->lost=1; if (g->recording) g->fault=1; }
void gs_gesture_resync(struct gs_gesture *g,int guide,int select,int start) {
    g->guide=guide; g->select=select; g->start=start; g->lost=0;
}
void gs_gesture_event(struct gs_gesture *g,const struct input_event *e,uint64_t at) {
    if (e->type==EV_SYN && e->code==SYN_DROPPED) { gs_gesture_lose(g); return; }
    if (g->lost || e->type!=EV_KEY || (e->value!=0 && e->value!=1)) return;
    int *held=e->code==BTN_MODE?&g->guide:e->code==BTN_SELECT?&g->select:e->code==BTN_START?&g->start:0;
    if (!held || *held==e->value) return;
    if (e->code==BTN_MODE && e->value && (!g->recording || g->fault || at>=g->arbiter.opened_ms+1500)) {
        gs_arbiter_init(&g->arbiter,at,1,g->select,g->start); g->recording=1; g->fault=0;
    } else if (g->recording) gs_arbiter_event(&g->arbiter,e,at);
    *held=e->value;
}
enum gs_route gs_gesture_route(const struct gs_gesture *g,uint64_t now) {
    return g->fault || !g->recording ? GS_ROUTE_CANCEL:gs_arbiter_poll(&g->arbiter,now);
}

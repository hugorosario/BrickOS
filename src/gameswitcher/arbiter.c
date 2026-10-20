#include "arbiter.h"
#include <string.h>
static void chord(struct gs_arbiter *a) {
    if (a->start) a->chord=GS_ROUTE_OSD;
    else if (a->select && a->chord!=GS_ROUTE_OSD) a->chord=GS_ROUTE_MENU;
}
void gs_arbiter_init(struct gs_arbiter *a,uint64_t now,int guide,int select,int start) {
    memset(a,0,sizeof(*a)); a->opened_ms=now; a->guide=guide; a->select=select; a->start=start;
    a->released=!guide; chord(a);
}
void gs_arbiter_event(struct gs_arbiter *a,const struct input_event *e,uint64_t now) {
    if (e->type!=EV_KEY || (e->value!=0 && e->value!=1)) return;
    int *held=e->code==BTN_MODE?&a->guide:e->code==BTN_SELECT?&a->select:e->code==BTN_START?&a->start:0;
    if (!held || *held==e->value) return;
    if (e->code==BTN_MODE) {
        if (!e->value) a->released=1;
        else if (a->released && now>=a->opened_ms && now-a->opened_ms<=280) a->second=1;
    }
    *held=e->value;
    if (now>=a->opened_ms && now-a->opened_ms<=280) chord(a);
}
enum gs_route gs_arbiter_poll(const struct gs_arbiter *a,uint64_t now) {
    if (now<a->opened_ms || now-a->opened_ms>=1500) return GS_ROUTE_CANCEL;
    if (a->guide || a->select || a->start) return GS_ROUTE_WAIT;
    if (a->chord) return a->swapped && a->chord==GS_ROUTE_MENU?GS_ROUTE_SWITCHER:a->chord;
    if (a->single) return a->swapped?GS_ROUTE_MENU:GS_ROUTE_SWITCHER;
    if (now-a->opened_ms<280) return GS_ROUTE_WAIT;
    return a->second?GS_ROUTE_PREVIOUS:a->swapped?GS_ROUTE_MENU:GS_ROUTE_SWITCHER;
}

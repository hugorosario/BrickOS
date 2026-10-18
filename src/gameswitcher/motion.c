#define _POSIX_C_SOURCE 200809L
#include "motion.h"
#include "../mainui/appearance.h"
#include <stdio.h>
#include <string.h>

float gs_motion_progress(uint64_t start, uint64_t now, int duration) {
    if (!start || duration<=0 || now>=start+(uint64_t)duration) return 1;
    if (now<=start) return 0;
    float left=1.f-(float)(now-start)/(float)duration;
    return 1.f-left*left*left;
}
static float tween(const struct gs_tween *t, uint64_t now, int duration) {
    return t->from+(t->to-t->from)*gs_motion_progress(t->start_ms,now,duration);
}
static void retarget(struct gs_tween *t, float to, uint64_t now, int duration) {
    t->from=tween(t,now,duration); t->to=to; t->start_ms=duration>0 ? (now?now:1) : 0;
}
void gs_motion_init(struct gs_motion *m, int duration) {
    memset(m,0,sizeof(*m)); m->duration_ms=duration>0 ? duration : 0;
    m->layer.from=m->layer.to=255;
}
int gs_motion_appearance_ms(const char *sd) {
    char path[512]; static struct mainui_appearance look;
    if (snprintf(path,sizeof(path),"%s/BrickOS/mainui/brickos/appearance.json",sd)>=(int)sizeof(path)) return GS_MOTION_MS;
    (void)mainui_appearance_load(&look,path); // a missing or invalid file gives the defaults
    return (int)(GS_MOTION_MS*mainui_appearance_motion(&look)+0.5);
}
float gs_motion_shift(const struct gs_motion *m, uint64_t now) {
    return m->slide_from*(1.f-gs_motion_progress(m->slide_ms,now,m->duration_ms));
}
void gs_motion_select(struct gs_motion *m, size_t old, size_t selected, size_t count, int jump, uint64_t now) {
    if (!count || old==selected) return;
    long n=(long)count, step=(((long)selected-(long)old)%n+n)%n;
    // The ring wraps; with two entries both neighbours are the same one, and the key tells the way.
    if (step*2>n || (step*2==n && m->step<0)) step-=n;
    if (jump || m->duration_ms<=0 || (step!=1 && step!=-1)) { m->slide_from=0; m->slide_ms=0; return; }
    float from=gs_motion_shift(m,now)+(float)step;
    m->slide_from=from>2 ? 2 : from<-2 ? -2 : from;
    m->slide_ms=now?now:1;
}
void gs_motion_zoom(struct gs_motion *m, int fullscreen, uint64_t now) {
    retarget(&m->zoom,fullscreen ? 1.f : 0.f,now,m->duration_ms);
}
float gs_motion_zoom_value(const struct gs_motion *m, uint64_t now) { return tween(&m->zoom,now,m->duration_ms); }
void gs_motion_remove(struct gs_motion *m, int side, uint64_t now) {
    m->removal.from=1; m->removal.to=0; m->removal.start_ms=m->duration_ms>0 ? (now?now:1) : 0;
    if (!m->removal.start_ms) m->removal.from=0;
    m->slide_from=m->duration_ms>0 ? (float)side : 0; m->slide_ms=m->slide_from!=0 ? (now?now:1) : 0;
}
float gs_motion_removed(const struct gs_motion *m, uint64_t now) {
    return m->removal.start_ms ? tween(&m->removal,now,m->duration_ms) : 0;
}
void gs_motion_fade(struct gs_motion *m, int open, uint64_t now) {
    if (open) { m->layer.from=0; m->layer.to=255; m->layer.start_ms=m->duration_ms>0 ? (now?now:1) : 0; }
    else retarget(&m->layer,0,now,m->duration_ms);
    if (!m->layer.start_ms) m->layer.from=m->layer.to;
}
int gs_motion_alpha(const struct gs_motion *m, uint64_t now) {
    float a=tween(&m->layer,now,m->duration_ms);
    return a<=0 ? 0 : a>=255 ? 255 : (int)(a+0.5f);
}
int gs_motion_paint_ms(const struct gs_motion *m, uint64_t now) { return gs_motion_active(m,now) ? 16 : 32; }
int gs_motion_wait_ms(const struct gs_motion *m, uint64_t now, uint64_t painted) {
    uint64_t due=painted+(uint64_t)gs_motion_paint_ms(m,now);
    return due>now ? (int)(due-now) : 0;
}
int gs_motion_removal_side(size_t removed_at, size_t selected, size_t count) {
    return !count ? 0 : selected==removed_at ? 1 : -1;
}
int gs_motion_active(const struct gs_motion *m, uint64_t now) {
    int d=m->duration_ms;
    return gs_motion_shift(m,now)!=0 || gs_motion_progress(m->zoom.start_ms,now,d)<1 ||
           gs_motion_progress(m->removal.start_ms,now,d)<1 || gs_motion_progress(m->layer.start_ms,now,d)<1;
}

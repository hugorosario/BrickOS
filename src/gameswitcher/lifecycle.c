#include "lifecycle.h"
#include <string.h>
void gs_lifecycle_init(struct gs_lifecycle *s,int resume) {
    memset(s,0,sizeof(*s)); s->phase=GS_BOOTING; s->resume_pending=!!resume;
}
int gs_lifecycle_pause(struct gs_lifecycle *s) {
    if (s->phase!=GS_BOOTING && s->phase!=GS_RUNNING && s->phase!=GS_PAUSED) return -1;
    s->phase=GS_PAUSED; return 0;
}
int gs_lifecycle_active(struct gs_lifecycle *s) {
    if (s->resume_pending || (s->phase!=GS_BOOTING && s->phase!=GS_PAUSED)) return -1;
    s->phase=GS_RUNNING; return 0;
}
int gs_lifecycle_loaded(struct gs_lifecycle *s,int accepted) {
    if (!s->resume_pending) return -1;
    if (!accepted) { s->phase=GS_BLOCKED; return -1; }
    s->resume_pending=0; return 0;
}
int gs_lifecycle_exit(struct gs_lifecycle *s,int committed,uint64_t now) {
    if (s->phase!=GS_PAUSED || !committed || now>UINT64_MAX-10000) return -1;
    s->phase=GS_EXITING; s->exit_deadline=now+10000; return 0;
}
void gs_lifecycle_tick(struct gs_lifecycle *s,uint64_t now) {
    if (s->phase==GS_EXITING && now>=s->exit_deadline) s->phase=GS_BLOCKED;
}
void gs_lifecycle_finished(struct gs_lifecycle *s,int normal,int released) {
    s->normal_exit=!!normal; s->released=!!released;
    if (s->phase==GS_EXITING && normal && released && !s->resume_pending) s->phase=GS_FINISHED;
    else s->phase=GS_BLOCKED;
}
int gs_lifecycle_handoff(const struct gs_lifecycle *s) {
    return s->phase==GS_FINISHED && s->normal_exit && s->released && !s->resume_pending;
}

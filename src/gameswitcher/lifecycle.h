#ifndef BRICKOS_GAMESWITCHER_LIFECYCLE_H
#define BRICKOS_GAMESWITCHER_LIFECYCLE_H
#include <stdint.h>
enum gs_phase { GS_BOOTING,GS_RUNNING,GS_PAUSED,GS_EXITING,GS_BLOCKED,GS_FINISHED };
struct gs_lifecycle { enum gs_phase phase; uint64_t exit_deadline; int resume_pending,normal_exit,released; };
void gs_lifecycle_init(struct gs_lifecycle *lifecycle,int resume);
int gs_lifecycle_pause(struct gs_lifecycle *lifecycle);
int gs_lifecycle_active(struct gs_lifecycle *lifecycle);
int gs_lifecycle_loaded(struct gs_lifecycle *lifecycle,int accepted);
int gs_lifecycle_exit(struct gs_lifecycle *lifecycle,int committed,uint64_t now_ms);
void gs_lifecycle_tick(struct gs_lifecycle *lifecycle,uint64_t now_ms);
void gs_lifecycle_finished(struct gs_lifecycle *lifecycle,int exited_normally,int released);
int gs_lifecycle_handoff(const struct gs_lifecycle *lifecycle);
#endif

#ifndef BRICKOS_GS_STANDBY_H
#define BRICKOS_GS_STANDBY_H
#include "session.h"
enum gs_standby_phase { GS_STANDBY_IDLE, GS_STANDBY_YIELD, GS_STANDBY_ENTER, GS_STANDBY_ASLEEP, GS_STANDBY_AWAKE, GS_STANDBY_FAILED };
struct gs_standby { enum gs_standby_phase phase; uint64_t deadline; pid_t keymon; uint64_t keymon_start; };
void gs_standby_init(struct gs_standby *standby,uint64_t now);
// Pure state transitions; one confirmed ownership release authorizes one pulse.
int gs_standby_yield(struct gs_standby *standby,uint64_t now);
void gs_standby_observe(struct gs_standby *standby,uint64_t now,int marker,int brightness);
// Delegates the already-measured stock display/audio/wireless/kernel sequence
// to the live official keymon, after inputd acknowledged exclusive release.
int gs_standby_pulse(struct gs_standby *standby,const struct gs_session *session);
int gs_standby_snapshot(int *marker,int *brightness);
int gs_standby_keymon_alive(const struct gs_standby *standby);
#endif

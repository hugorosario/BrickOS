#ifndef BRICKOS_GAMESWITCHER_GESTURE_H
#define BRICKOS_GAMESWITCHER_GESTURE_H
#include "arbiter.h"
// fault: the gesture in progress was cut short, by dropped input or the controller's removal, and
// routes to CANCEL; the next GUIDE press starts a new one. lost: the held keys are unknown since,
// so events are ignored until gs_gesture_resync gives them afresh.
// single and swapped are the player's GUIDE choices, given to each gesture's arbiter.
struct gs_gesture { struct gs_arbiter arbiter; int recording,guide,select,start,fault,lost,single,swapped; };
void gs_gesture_event(struct gs_gesture *gesture,const struct input_event *event,uint64_t at);
enum gs_route gs_gesture_route(const struct gs_gesture *gesture,uint64_t now);
// Dropped input or the controller's removal: cancels the gesture in progress and ignores events
// until the held keys are read again.
void gs_gesture_lose(struct gs_gesture *gesture);
// The held keys read afresh (EVIOCGKEY): a key still held from before cannot start a gesture.
void gs_gesture_resync(struct gs_gesture *gesture,int guide,int select,int start);
// The reader finds the one TRIMUI Player1 with the stock identity (USB, 045e:028e) and GUIDE,
// SELECT and START among its keys, read-only and never grabbed. When the input daemon is replaced
// it looks for the new one every 100 ms. 0, or -1 when there is no such controller at the start.
int gs_gesture_start(void);
// The player's GUIDE choices from GameSwitcher's settings, for the gestures that follow.
void gs_gesture_configure(int double_tap,int guide_menu);
enum gs_route gs_gesture_finish(void);
// Waits only for an injected GUIDE's release, for a request already queued.
void gs_gesture_release(void);
// Where the reader looks for the controller, and its ioctl; tests replace them with fake nodes.
extern const char *gs_gesture_directory;
extern int (*gs_gesture_ioctl)(int fd,unsigned long request,void *argument);
#endif

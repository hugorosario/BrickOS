#ifndef BRICKOS_GAMESWITCHER_ARBITER_H
#define BRICKOS_GAMESWITCHER_ARBITER_H
#include <stdint.h>
#include <linux/input.h>
enum gs_route { GS_ROUTE_WAIT, GS_ROUTE_SWITCHER, GS_ROUTE_MENU, GS_ROUTE_OSD, GS_ROUTE_PREVIOUS, GS_ROUTE_CANCEL };
// The player's GUIDE choices, set after gs_arbiter_init: single, no double tap, routes a tap as
// soon as every key is up; swapped, GUIDE opening the menu, exchanges the tap's route and the
// SELECT chord's. Both 0 is the default: a tap opens GameSwitcher after the 280 ms window.
struct gs_arbiter {
    uint64_t opened_ms;
    int guide, select, start, released, second;
    enum gs_route chord;
    int single, swapped;
};
void gs_arbiter_init(struct gs_arbiter *arbiter,uint64_t now,int guide,int select,int start);
void gs_arbiter_event(struct gs_arbiter *arbiter,const struct input_event *event,uint64_t now);
enum gs_route gs_arbiter_poll(const struct gs_arbiter *arbiter,uint64_t now);
#endif

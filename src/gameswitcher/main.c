#define _GNU_SOURCE
#include "channel.h"
#include "../common/version.h"
BRICKOS_DECLARE_COMPONENT("gameswitcher", BRICKOS_VERSION_GAMESWITCHER);
#include "platform.h"
#include "render.h"
#include "policy.h"
#include "motion.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t stopped;
static void stop(int signal) { (void)signal; stopped=1; }
static uint64_t now_ms(void) {
    struct timespec ts; if (clock_gettime(CLOCK_MONOTONIC,&ts)) return 0;
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}
// What one input event or key repeat acts on.
struct turn {
    struct gs_model *model; struct gs_motion *motion; struct gs_assets *assets; struct gs_store *store;
    struct gs_platform *platform; int channel, *chosen; enum gs_choice *choice; struct gs_entry *selected;
};
// The selection moved from before by a LEFT or RIGHT step: the cards slide, and the picture coming
// in is read.
static void moved(struct turn *t,size_t before,int step) {
    t->motion->step=step;
    gs_motion_select(t->motion,before,t->model->selected,t->model->index.count,0,now_ms());
    if (t->model->selected!=t->assets->selected) gs_assets_reload(t->assets,t->store,t->model);
}
static void handle(struct turn *t,const struct input_event *event) {
    struct gs_model *model=t->model;
    size_t before=model->selected; int was_fullscreen=model->fullscreen;
    enum gs_action action=gs_model_event(model,event);
    if (action==GS_CHANGED && model->selected!=before && event->type==EV_ABS && event->code==ABS_HAT0X)
        moved(t,before,event->value<0?-1:1);
    if (model->fullscreen!=was_fullscreen) gs_motion_zoom(t->motion,model->fullscreen,now_ms());
    if (event->type==EV_KEY || event->type==EV_ABS)
        fprintf(stderr,"GameSwitcher input type=%u code=%u value=%d action=%d selected=%zu fullscreen=%d time=%d\n",
                event->type,event->code,event->value,action,model->selected,model->fullscreen,model->show_time);
    if (action==GS_CHANGED) {
        const struct gs_entry *entry=gs_model_selected(model);
        fprintf(stderr,"GameSwitcher status selected=%s played_ms=%llu total_ms=%llu time=%d\n",
            entry?entry->id:"none",(unsigned long long)(entry?entry->play_ms:0),
            (unsigned long long)gs_model_total(model),model->show_time);
    }
    if (action==GS_QUIT) *t->chosen=1;
    else if (action==GS_BRIGHTER || action==GS_DARKER) gs_platform_brightness(t->platform,action==GS_BRIGHTER?1:-1);
    else if (action==GS_RESUME) {
        const struct gs_entry *entry=gs_model_selected(model);
        if (!entry || gs_entry_resumable(t->store,entry)) snprintf(model->message,sizeof(model->message),"Checkpoint unavailable");
        else if (t->channel<0) snprintf(model->message,sizeof(model->message),"Resume requires a verified game session");
        else { *t->selected=*entry; *t->choice=GS_CHOICE_RESUME; *t->chosen=1; }
    } else if (action==GS_REMOVE) {
        const struct gs_entry *entry=gs_model_selected(model);
        if (entry && gs_store_hide(t->store,entry->id)==0) {
            // The removed card fades out while the next one comes in from the right, or the
            // previous one from the left when the last entry went.
            size_t removed_at=model->selected;
            gs_assets_keep_removed(t->assets,model);
            model->message[0]=0; (void)gs_model_reload(model,t->store); gs_assets_reload(t->assets,t->store,model);
            gs_motion_remove(t->motion,gs_motion_removal_side(removed_at,model->selected,model->index.count),now_ms());
        } else snprintf(model->message,sizeof(model->message),"Could not remove history entry");
    } else if (action==GS_CHANGED && model->selected!=t->assets->selected) gs_assets_reload(t->assets,t->store,model);
}
int main(int argc,char **argv) {
    BRICKOS_HANDLE_VERSION(argc, argv);
    const char *sd="/mnt/SDCARD", *font="/usr/trimui/res/regular.ttf", *name="gameswitcher"; int channel=-1;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--sd") && i+1<argc) sd=argv[++i];
        else if (!strcmp(argv[i],"--namespace") && i+1<argc) name=argv[++i];
        else if (!strcmp(argv[i],"--font") && i+1<argc) font=argv[++i];
        else if (!strcmp(argv[i],"--channel-fd") && i+1<argc) {
            char *end; long n=strtol(argv[++i],&end,10);
            if (*end || n<3 || n>1024) return 2;
            channel=(int)n;
        } else { fprintf(stderr,"GameSwitcher: invalid argument\n"); return 2; }
    }
    if (channel>=0 && gs_channel_parent(channel)) {
        fprintf(stderr,"GameSwitcher: invalid parent channel\n"); return 1;
    }
    signal(SIGTERM,stop); signal(SIGINT,stop); signal(SIGHUP,stop);
    struct gs_store store; struct gs_model model; struct gs_assets assets; struct gs_platform platform;
    gs_model_init(&model);
    if (gs_store_open_namespace(&store,sd,name)) { perror("GameSwitcher store"); return 1; }
    struct gs_policy policy;
    if (gs_policy_load(&store,&policy)) gs_policy_defaults(&policy);
    // The motion time follows BrickOS's Animations choice: 200, 100 or 0 ms.
    struct gs_motion motion; gs_motion_init(&motion,gs_motion_appearance_ms(sd));
    // A corrupt index is shown as an error with B available; never repair it silently.
    (void)gs_model_reload(&model,&store);
    if (TTF_Init()<0 || gs_assets_open(&assets,font)) {
        fprintf(stderr,"GameSwitcher font: %s\n",TTF_GetError()); gs_model_close(&model); gs_store_close(&store); return 1;
    }
    gs_assets_reload(&assets,&store,&model);
    if (gs_platform_open(&platform,&model)) {
        perror("GameSwitcher platform"); gs_assets_close(&assets); TTF_Quit(); gs_model_close(&model); gs_store_close(&store); return 1;
    }
    int result=0,chosen=0; enum gs_choice choice=GS_CHOICE_BACK; struct gs_entry selected={0};
    struct turn turn={&model,&motion,&assets,&store,&platform,channel,&chosen,&choice,&selected};
    uint64_t status_ms=0,paint_ms=0; int wait=0;
    // GameSwitcher fades in over the screen under it; the first present shows the layer at its start.
    gs_motion_fade(&motion,1,now_ms());
    (void)gs_platform_alpha(&platform,(unsigned char)gs_motion_alpha(&motion,now_ms()));
    while (!stopped && !chosen) {
        // Input waits only until the next frame or key repeat is due, and every event waiting is
        // handled before that frame: a press never queues behind the frames of a motion.
        int failed=0;
        for (int timeout=wait;!chosen;timeout=0) {
            struct input_event event; int n=gs_platform_input(&platform,&event,timeout);
            // An incomplete stream cannot confirm a selection or pass held input back.
            if (n<0 || (n && event.type==EV_SYN && event.code==SYN_DROPPED)) { failed=1; break; }
            if (!n) break;
            handle(&turn,&event);
        }
        if (failed) { result=1; break; }
        if (chosen) break;
        // The brightness toast scripts the chord started.
        gs_platform_reap();
        uint64_t now=now_ms();
        if (channel>=0) {
            struct pollfd peer={.fd=channel,.events=POLLIN};
            if (poll(&peer,1,0)>0 && (peer.revents&(POLLHUP|POLLERR|POLLNVAL))) { result=1; break; }
        }
        if (now-status_ms>=1000) {
            gs_platform_status(&model); status_ms=now;
            const char *warning="Battery low; connect a charger";
            if (model.battery>=0 && model.battery<policy.warning && !*model.message)
                snprintf(model.message,sizeof(model.message),"%s",warning);
            else if (model.battery>=policy.warning && !strcmp(model.message,warning)) model.message[0]=0;
        }
        // A held LEFT or RIGHT repeats as the BrickOS carousel does.
        size_t before=model.selected;
        if (gs_model_repeat(&model,now)==GS_CHANGED) {
            moved(&turn,before,model.x<0?-1:1);
            fprintf(stderr,"GameSwitcher repeat x=%d selected=%zu\n",model.x,model.selected);
        }
        // Every 16 ms while something moves, every 32 ms at rest.
        now=now_ms();
        if (now-paint_ms>=(uint64_t)gs_motion_paint_ms(&motion,now)) {
            gs_render_at(platform.screen,&assets,&model,&motion,now);
            if (gs_platform_present(&platform)) { result=1; break; }
            if (gs_platform_alpha(&platform,(unsigned char)gs_motion_alpha(&motion,now))) { result=1; break; }
            paint_ms=now;
        }
        now=now_ms();
        wait=gs_motion_wait_ms(&motion,now,paint_ms);
        int repeat=gs_model_repeat_wait(&model,now);
        if (repeat>=0 && repeat<wait) wait=repeat;
    }
    // Consume the selecting button's release before the parent resumes its controller.
    uint64_t until=now_ms()+1500;
    int held=1;
    while (chosen && !stopped && held && now_ms()<until) {
        held=model.x || model.y;
        for (size_t i=0;i<sizeof(model.held);++i) held=held || model.held[i];
        if (!held) break;
        struct input_event event; int n=gs_platform_input(&platform,&event,16);
        if (n<0) { result=1; break; }
        if (n) (void)gs_model_event(&model,&event);
    }
    if (chosen && held) result=1;
    // The layer fades out before it closes: on B, a resume or a switch.
    if (!result && !stopped && motion.duration_ms>0 && platform.visible) {
        gs_motion_fade(&motion,0,now_ms());
        for (uint64_t t=now_ms(); gs_motion_alpha(&motion,t)>0 && !stopped; t=now_ms()) {
            if (gs_platform_alpha(&platform,(unsigned char)gs_motion_alpha(&motion,t))) break;
            struct timespec wait={0,16000000}; nanosleep(&wait,NULL);
        }
    }
    if (gs_platform_close(&platform)) result=1;
    // Delivery after restoration: a parent must never hand off while this process owns display/input.
    if (!result && chosen && channel>=0 && gs_channel_send(channel,choice,choice==GS_CHOICE_RESUME?&selected:NULL)) result=1;
    if (channel>=0) close(channel);
    gs_assets_close(&assets); TTF_Quit(); gs_model_close(&model); gs_store_close(&store);
    return result;
}

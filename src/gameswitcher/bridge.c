#define _GNU_SOURCE
#include "bridge.h"
#include "gesture.h"
#include "channel.h"
#include "checkpoint.h"
#include "app_process.h"
#include "guard.h"
#include "standby.h"
#include "policy.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

static void toast(enum gs_notice notice) {
    fprintf(stderr,"GameSwitcher: %s\n",gs_notice_text(notice,0));
    (void)gs_notice(notice);
}
static int pause_ack(struct gs_session *s) {
    if (gs_message_send(s,GS_MENU_ENTER,1,NULL,NULL)) return -1;
    uint64_t until=gs_monotonic_ms()+3000;
    while (gs_monotonic_ms()<until) {
        struct gs_message message; int n=gs_message_read(s,&message);
        if (n<0) return -1;
        if (n>0) return message.type==GS_MENU_ACK && message.success ? 0:-1;
        struct pollfd p={.fd=s->channel,.events=POLLIN}; (void)poll(&p,1,10);
    }
    return -1;
}
static int find_choice(struct gs_store *s,const struct gs_choice_message *choice,struct gs_entry *entry) {
    struct gs_index index; if (gs_store_load(s,&index)) return -1;
    int result=-1;
    for (size_t i=0;i<index.count;++i) if (!strcmp(choice->id,index.entries[i].id) &&
        !strcmp(choice->state,index.entries[i].state) && !gs_entry_resumable(s,&index.entries[i])) {
        *entry=index.entries[i]; result=0; break;
    }
    gs_index_close(&index); return result;
}
static int previous(struct gs_store *s,const struct gs_identity *current,struct gs_entry *entry) {
    struct gs_index index; if (gs_store_load(s,&index)) return -1;
    int result=-1;
    for (size_t i=0;i<index.count;++i) if (strcmp(current->rom,index.entries[i].identity.rom) &&
        !gs_entry_resumable(s,&index.entries[i])) { *entry=index.entries[i]; result=0; break; }
    gs_index_close(&index); return result;
}
static char last_request[GS_ID];
static uint32_t last_type;
static int last_success;
static int legacy_menu;
void gs_bridge_legacy_finished(void) {
    if (!legacy_menu) return;
    legacy_menu=0;
    if (gs_bridge_enabled()) gs_observer_menu(0,1);
}
static int request_valid(const struct gs_message *m) {
    return strlen(m->transaction)==32 && strspn(m->transaction,"0123456789abcdef")==32 &&
           m->monotonic_ms<=gs_monotonic_ms() && gs_monotonic_ms()-m->monotonic_ms<15000;
}
static int save_request(struct gs_session *s,struct tmenu_host *host,SDL_Surface *background,const struct gs_message *request) {
    if (!request_valid(request)) return -1;
    int success;
    if (!strcmp(last_request,request->transaction)) {
        if (last_type!=request->type) return -1;
        success=last_success;
    } else {
        success=gs_checkpoint_save(gs_observer_store(),&s->identity,host,background,12000)==0;
        memcpy(last_request,request->transaction,GS_ID); last_type=request->type; last_success=success;
    }
    if (gs_message_send(s,GS_SAVE_RESULT,success,NULL,request->transaction)) return -1;
    return success?0:-1;
}
static char last_power[GS_ID];
static int power_save(struct gs_session *s,struct tmenu_host *host,SDL_Surface *background,const struct gs_message *request) {
    if (!request_valid(request) || !strcmp(last_power,request->transaction)) {
        (void)gs_message_send(s,GS_POWER_READY,0,NULL,request->transaction); return -1;
    }
    strcpy(last_power,request->transaction);
    struct gs_stage stage;
    int prepared=gs_checkpoint_prepare(gs_observer_store(),&s->identity,host,background,12000,&stage)==0;
    uint64_t until=prepared?stage.deadline_ms:gs_monotonic_ms()+3000;
    int result=-1,holding=0,usable=prepared;
    int sent=prepared?gs_message_ready(s,&stage,request->transaction):
        gs_message_send(s,GS_POWER_READY,0,NULL,request->transaction);
    // Even a failed producer consumes its matching DONE response before returning.
    // Otherwise a stale acknowledgement could steal the next user's GUIDE.
    if (!sent) {
        while (gs_monotonic_ms()<until) {
            struct gs_message m; int count=gs_message_read(s,&m);
            if (count<0) break;
            if (count>0) {
                if (m.type==GS_POWER_QUIT && m.success && usable && !strcmp(m.transaction,request->transaction)) {
                    result=1; break; // Parent already durably committed; no second save.
                }
                if (m.type==GS_POWER_DONE && !strcmp(m.transaction,request->transaction)) {
                    result=usable && m.success?0:-1; break;
                }
                if (m.type==GS_POWER_WAIT && m.success && prepared && !strcmp(m.transaction,request->transaction)) {
                    gs_checkpoint_discard(&stage); prepared=0; holding=1;
                    until=gs_monotonic_ms()+185000; continue;
                }
                // No concurrent callback can enter while this staging lock is held.
                (void)gs_message_send(s,GS_SAVE_RESULT,0,NULL,m.transaction);
            }
            struct pollfd p={.fd=s->channel,.events=POLLIN}; (void)poll(&p,1,20);
            if (p.revents&(POLLHUP|POLLERR|POLLNVAL)) break;
        }
    }
    if (prepared) gs_checkpoint_discard(&stage);
    // Parent failure cannot return into screen-off gameplay. Stock keymon still
    // owns wake; retain this callback context until the measured wake is visible.
    if (holding) {
        int marker,brightness;
        while (gs_standby_snapshot(&marker,&brightness) || marker || brightness<=0) {
            struct timespec wait={0,20000000}; nanosleep(&wait,NULL);
        }
    }
    if (result<0) toast(GS_NOTICE_POWER_COMPLETION_UNAVAILABLE);
    return result;
}
static int app(struct gs_session *s,struct tmenu_host *host,SDL_Surface *background,struct gs_choice_message *choice) {
    struct gs_guard guard; if (gs_guard_open(&guard)) return -1;
    pid_t child; int channel;
    if (gs_app_spawn(s->app,s->sd,s->store_name,s->channel,&child,&channel)) { (void)gs_guard_close(&guard); return -1; }
    int result=-1,status=0;
    for (;;) {
        pid_t ended=waitpid(child,&status,WNOHANG);
        if (ended==child) {
            if (WIFEXITED(status) && !WEXITSTATUS(status) && gs_channel_read(channel,choice)==1) result=0;
            break;
        }
        if (ended<0) break;
        struct pollfd p={.fd=s->channel,.events=POLLIN};
        int events=poll(&p,1,20);
        if (events>0 && (p.revents&(POLLHUP|POLLERR|POLLNVAL))) {
            // Parent death: close overlay before allowing RetroArch to continue.
            if (gs_app_recover(child)) {
                // Still owned by a stuck child: never return into the game beneath it.
                while (waitpid(child,&status,0)<0 && errno==EINTR) {}
            }
            break;
        }
        if (events>0 && (p.revents&POLLIN)) {
            struct gs_message request;
            if (gs_message_read(s,&request)!=1) continue;
            if (request.type==GS_POWER_SAVE) {
                if (power_save(s,host,background,&request)==1) {
                    if (!gs_app_recover(child)) result=2;
                    break;
                }
            }
            else if (request.type==GS_REQUEST_SAVE || request.type==GS_REQUEST_QUIT) {
                if (!save_request(s,host,background,&request) && request.type==GS_REQUEST_QUIT) {
                    if (!gs_app_recover(child)) result=2;
                    break;
                }
            } else (void)gs_message_send(s,GS_SAVE_RESULT,0,NULL,request.transaction);
        }
    }
    close(channel); if (gs_guard_close(&guard)) result=-1; return result;
}
int gs_bridge_quit(struct tmenu_host *host,SDL_Surface *background) {
    struct gs_session *s=gs_observer_session();
    if (!s || !gs_bridge_enabled()) { toast(GS_NOTICE_SAVE_FAILED); return -1; }
    // The save blocks the game on its paused frame; osdd keeps drawing the notice meanwhile.
    (void)gs_notice_progress(GS_NOTICE_SAVING);
    if (gs_checkpoint_save(gs_observer_store(),&s->identity,host,background,12000) ||
        gs_message_send(s,GS_SWITCH,1,NULL,NULL)) {
        toast(GS_NOTICE_SAVE_FAILED); return -1;
    }
    (void)gs_notice_close(GS_NOTICE_SAVING);
    legacy_menu=0; host->quit(); gs_observer_menu(0,0); return 0;
}
static int prepare_load(struct gs_session *s,struct gs_store *store,struct tmenu_host *host,const struct gs_message *request) {
    if (!request_valid(request)) return -1;
    struct gs_choice_message choice={.magic=GS_CHANNEL_MAGIC,.action=GS_CHOICE_RESUME};
    memcpy(choice.id,request->id,GS_ID); memcpy(choice.state,request->state,GS_ASSET);
    struct gs_entry entry;
    if (find_choice(store,&choice,&entry) || strcmp(entry.identity.rom,s->identity.rom) ||
        strcmp(entry.identity.core,s->identity.core)) return -1;
    char *thumb=host->thumb_path(99); if (!thumb) return -1;
    size_t n=strlen(thumb); char state[GS_PATH];
    if (n<4 || n+3>=GS_PATH || strcmp(thumb+n-4,".png")) { free(thumb); return -1; }
    memcpy(state,thumb,n-4); strcpy(state+n-4,".state"); struct gs_stage stage;
    int result=gs_stage_begin(store,&s->identity,state,thumb,&stage); free(thumb); if (result) return -1;
    unsigned char *memory=NULL; size_t bytes=0;
    if (gs_stage_load(store,&entry,&stage,&memory,&bytes) ||
        gs_observer_load(&stage,memory,bytes,&entry,request->transaction)) {
        free(memory); if (stage.directory>=0) unlinkat(stage.directory,stage.state,0);
        gs_stage_close(&stage); return -1;
    }
    (void)host->load_state(99);
    return gs_message_send(s,GS_LOAD_QUEUED,1,&entry,request->transaction);
}
int gs_bridge_show(struct tmenu_host *host,SDL_Surface *background,int *result) {
    if (!gs_bridge_enabled()) return 0;
    struct gs_session *s=gs_observer_session(); struct gs_store *store=gs_observer_store();
    *result=0; gs_observer_menu(1,0);
    // Queued external requests already entered this context via one checked GUIDE.
    struct gs_message request; int queued=gs_message_read(s,&request);
    while (queued>0 && request.type==GS_POWER_DONE && !strcmp(request.transaction,last_power))
        queued=gs_message_read(s,&request);
    if (queued<0) { gs_observer_disable(); return 0; }
    if (queued>0 && request.type!=GS_REQUEST_LOAD && request.type!=GS_REQUEST_SAVE && request.type!=GS_REQUEST_QUIT && request.type!=GS_POWER_SAVE) {
        toast(GS_NOTICE_UNEXPECTED_REQUEST); gs_observer_menu(0,1); return 1;
    }
    if (pause_ack(s)) { toast(GS_NOTICE_SESSION_UNAVAILABLE); gs_observer_disable(); return 0; }
    if (queued>0 && request.type==GS_REQUEST_LOAD) {
        gs_gesture_release(); // Consume injected GUIDE release before returning.
        int loaded=prepare_load(s,store,host,&request);
        if (loaded) (void)gs_message_send(s,GS_LOAD_RESULT,0,NULL,request.transaction);
        gs_observer_menu(0,0); return 1;
    }
    if (queued>0 && request.type==GS_REQUEST_QUIT && !request.success && request_valid(&request) &&
        !gs_observer_recovery_quit()) {
        gs_gesture_release();
        (void)gs_message_send(s,GS_SAVE_RESULT,1,NULL,request.transaction);
        host->quit(); gs_observer_menu(0,0); return 1;
    }
    // A queued request's injected GUIDE needs no gesture classification.
    enum gs_route selected=GS_ROUTE_SWITCHER;
    if (queued>0) gs_gesture_release(); else selected=gs_gesture_finish();
    if (selected==GS_ROUTE_MENU) { legacy_menu=1; return 0; }
    if (selected==GS_ROUTE_OSD || selected==GS_ROUTE_CANCEL) { gs_observer_menu(0,1); return 1; }
    if (queued>0 && request.type==GS_POWER_SAVE) {
        if (power_save(s,host,background,&request)==1) { host->quit(); gs_observer_menu(0,0); return 1; }
        gs_observer_menu(0,1); return 1;
    }
    if (queued>0 && (request.type==GS_REQUEST_SAVE || request.type==GS_REQUEST_QUIT)) {
        if (!save_request(s,host,background,&request) && request.type==GS_REQUEST_QUIT) {
            host->quit(); gs_observer_menu(0,0); return 1;
        }
        gs_observer_menu(0,1); return 1;
    }
    (void)gs_notice_progress(GS_NOTICE_SAVING);
    if (gs_checkpoint_save(store,&s->identity,host,background,12000)) {
        toast(GS_NOTICE_SAVE_FAILED);
        if (queued>0) (void)gs_message_send(s,GS_SAVE_RESULT,0,NULL,request.transaction);
        gs_observer_menu(0,1); return 1;
    }
    (void)gs_notice_close(GS_NOTICE_SAVING);
    struct gs_entry entry;
    if (selected==GS_ROUTE_PREVIOUS && !previous(store,&s->identity,&entry)) {
        if (!gs_message_send(s,GS_SWITCH,1,&entry,NULL)) { host->quit(); gs_observer_menu(0,0); return 1; }
        gs_observer_menu(0,1); return 1;
    }
    struct gs_choice_message choice;
    int chosen=app(s,host,background,&choice);
    if (chosen==2) { host->quit(); gs_observer_menu(0,0); return 1; }
    if (chosen) { toast(GS_NOTICE_CLOSED_WITHOUT_SELECTION); gs_observer_menu(0,1); return 1; }
    if (choice.action==GS_CHOICE_BACK) {
        if (!gs_message_send(s,GS_SWITCH,1,NULL,NULL)) { host->quit(); gs_observer_menu(0,0); return 1; }
    } else if (!find_choice(store,&choice,&entry)) {
        if (!strcmp(entry.identity.rom,s->identity.rom) && !strcmp(entry.identity.core,s->identity.core)) {
            gs_observer_menu(0,1); return 1;
        }
        if (!gs_message_send(s,GS_SWITCH,1,&entry,NULL)) { host->quit(); gs_observer_menu(0,0); return 1; }
    } else toast(GS_NOTICE_SELECTION_UNAVAILABLE);
    gs_observer_menu(0,1); return 1;
}

#define _GNU_SOURCE
#include "app_process.h"
#include "../common/version.h"
BRICKOS_DECLARE_COMPONENT("gameswitcher", BRICKOS_VERSION_GAMESWITCHER);
#include "clock.h"
#include "guard.h"
#include "lifecycle.h"
#include "session.h"
#include "power_control.h"
#include "standby.h"
#include "policy.h"
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static volatile sig_atomic_t stopped;
static struct gs_power_server power_server={.listener=-1,.client=-1};
static int power_checkpoint_probe;
static int power_drop_ready;
static int power_control_enabled;
static int power_shutdown_probe;
static int drop_hello;
static char shutdown_transaction[GS_ID];
// The entry and the checkpoint the armed shutdown published (the checkpoint's own transaction,
// not the POWER request's), for the resume record.
static char shutdown_entry[GS_ID],shutdown_checkpoint[GS_ID];
static struct gs_policy policy;
static const char *battery_capacity="/sys/class/power_supply/axp2202-battery/capacity";
static const char *battery_charging="/sys/class/power_supply/axp2202-usb/online";
static void stop(int sig) { (void)sig; stopped=1; }
static void notice(enum gs_notice notice) {
    fprintf(stderr,"GameSwitcher: %s\n",gs_notice_text(notice,0));
    (void)gs_notice(notice);
}
// A save or load blocks the game for a few seconds; osdd draws this meanwhile.
static void progress(enum gs_notice notice) {
    fprintf(stderr,"GameSwitcher: %s\n",gs_notice_text(notice,0));
    (void)gs_notice_progress(notice);
}
static int capacity(const char *path) {
    FILE *f=fopen(path,"r"); int value=-1; char extra;
    if (f) { if (fscanf(f,"%d %c",&value,&extra)!=1) value=-1; fclose(f); }
    return value;
}
static int shutdown_game(void) {
    if (power_shutdown_probe) { fprintf(stderr,"GameSwitcher shutdown-ready transaction=%s probe=1\n",shutdown_transaction); return 0; }
    if (gs_shutdown_progress("/tmp",shutdown_transaction)) { notice(GS_NOTICE_SHUTDOWN_PROGRESS_UNAVAILABLE); return -1; }
    // poweroff_flag is osdd's external-progress source: osdd draws it and never
    // runs another worker. This foreground owner dispatches exactly once.
    sync();
    pid_t child=fork();
    if (!child) {
        fprintf(stderr,"GameSwitcher poweroff-dispatch transaction=%s worker=%ld\n",shutdown_transaction,(long)getpid());
        fflush(stderr); sync();
        execlp("poweroff","poweroff",(char *)NULL); _exit(127);
    }
    if (child<0) { notice(GS_NOTICE_POWEROFF_START_FAILED); return -1; }
    int status; while (waitpid(child,&status,0)<0) { if (errno!=EINTR) return -1; }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { notice(GS_NOTICE_POWEROFF_FAILED); return -1; }
    // A returning command is not proof of power loss. Keep init's foreground
    // owner and osdd progress until shutdown, rather than starting another game.
    while (!stopped) pause();
    return 0;
}
static int runtime_valid(const char *library) {
    void *handle=dlopen(library,RTLD_NOW|RTLD_LOCAL);
    if (!handle) return -1;
    int valid=dlsym(handle,"ShowMenu")!=NULL && dlsym(handle,"dlsym")!=NULL;
    dlclose(handle); return valid?0:-1;
}
static int nonce(char out[GS_ID]) {
    unsigned char bytes[16]; int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    if (fd<0) return -1;
    ssize_t n=read(fd,bytes,sizeof(bytes)); close(fd); if (n!=sizeof(bytes)) return -1;
    for (int i=0;i<16;++i) snprintf(out+i*2,3,"%02x",bytes[i]);
    return 0;
}
static int resolve(struct gs_store *store,const char *id,const char *state,struct gs_entry *entry) {
    struct gs_index index; if (gs_store_load(store,&index)) return -1;
    int result=-1;
    for (size_t i=0;i<index.count;++i) if (!strcmp(index.entries[i].id,id) &&
        !strcmp(index.entries[i].state,state) && !gs_entry_resumable(store,&index.entries[i])) {
        *entry=index.entries[i]; result=0; break;
    }
    gs_index_close(&index); return result;
}
// A checkpoint's names as the store makes them: a 32-digit lowercase hex id, and its state
// file, the same digits and ".state". Nothing else is ever looked up.
static int hex32(const char *text) {
    for (int i=0;i<32;++i) if (!text[i] || !strchr("0123456789abcdef",text[i])) return 0;
    return 1;
}
static int store_names(const char *id,const char *state) {
    return strlen(id)==32 && hex32(id) && strlen(state)==38 && hex32(state) && !strcmp(state+32,".state");
}
static int controller(int write_access) {
    for (int i=0;i<32;++i) {
        char path[64],name[128]={0}; snprintf(path,sizeof(path),"/dev/input/event%d",i);
        int fd=open(path,(write_access?O_RDWR:O_RDONLY)|O_CLOEXEC|O_NONBLOCK); if (fd<0) continue;
        if (ioctl(fd,EVIOCGNAME(sizeof(name)),name)>=0 && !strcmp(name,"TRIMUI Player1")) return fd;
        close(fd);
    }
    return -1;
}
static int neutral(int fd) {
    unsigned long bits[(KEY_MAX+8*sizeof(unsigned long))/(8*sizeof(unsigned long))]={0};
    struct input_absinfo x={0},y={0};
    if (ioctl(fd,EVIOCGKEY(sizeof(bits)),bits)<0 || ioctl(fd,EVIOCGABS(ABS_HAT0X),&x)<0 ||
        ioctl(fd,EVIOCGABS(ABS_HAT0Y),&y)<0 || x.value || y.value) return 0;
    for (size_t i=0;i<sizeof(bits)/sizeof(bits[0]);++i) if (bits[i]) return 0;
    return 1;
}
static int emit(int fd,int value) {
    struct input_event events[2]={{.type=EV_KEY,.code=BTN_MODE,.value=value},{.type=EV_SYN,.code=SYN_REPORT}};
    return write(fd,events,sizeof(events))==sizeof(events)?0:-1;
}
static int enter(const struct gs_session *s) {
    if (gs_process_matches(s->pid,s->process_start,&s->identity,s->append)) return -1;
    int fd=controller(1); if (fd<0) return -1;
    if (!neutral(fd)) { close(fd); return -1; }
    int result=emit(fd,1);
    struct timespec wait={0,100000000}; nanosleep(&wait,NULL);
    if (emit(fd,0)) result=-1;
    close(fd);
    if (gs_process_matches(s->pid,s->process_start,&s->identity,s->append)) result=-1;
    return result;
}
static int released(pid_t group) {
    // All RA/overlay descendants share the launch's dedicated group. No member
    // may still retain a framebuffer/Mali/controller descriptor before handoff.
    errno=0; if (kill(-group,0)==0 || errno!=ESRCH) return 0;
    int fd=controller(0); if (fd<0) return 0;
    int ok=ioctl(fd,EVIOCGRAB,1)==0 && neutral(fd);
    if (ioctl(fd,EVIOCGRAB,0)<0) ok=0;
    close(fd); return ok;
}
static void recover_group(pid_t group) {
    // This launch owns a dedicated process group and this supervisor adopts
    // descendants. Recovery never authorizes handoff or reports normal exit.
    (void)kill(-group,SIGTERM);
    uint64_t until=gs_monotonic_ms()+3000; int killed=0;
    for (;;) {
        int status;
        while (waitpid(-group,&status,WNOHANG)>0) {}
        errno=0; if (kill(-group,0)<0 && errno==ESRCH) return;
        if (!killed && gs_monotonic_ms()>=until) { (void)kill(-group,SIGKILL); killed=1; }
        // A kernel-stuck owner keeps init blocked until its descriptors close.
        struct timespec wait={0,20000000}; nanosleep(&wait,NULL);
    }
}
static int app_choice(const char *app,const char *sd,const char *name,struct gs_choice_message *choice) {
    struct gs_guard guard; if (gs_guard_open(&guard)) return -1;
    pid_t child; int channel;
    if (gs_app_spawn(app,sd,name,-1,&child,&channel)) { (void)gs_guard_close(&guard); return -1; }
    int status=0,result=-1;
    for (;;) {
        pid_t ended=waitpid(child,&status,WNOHANG);
        if (ended==child) {
            if (WIFEXITED(status) && !WEXITSTATUS(status) && gs_channel_read(channel,choice)==1) result=0;
            break;
        }
        if (ended<0) break;
        if (stopped) { (void)gs_app_recover(child); break; }
        struct timespec wait={0,20000000}; nanosleep(&wait,NULL);
    }
    close(channel); if (gs_guard_close(&guard)) result=-1; return result;
}
static int emulator_prepare(const struct gs_identity *identity) {
    // Preserve the fixed stock CPU setup on every first launch and game switch.
    // Saved identities supply no shell command or arbitrary additional argument.
    const char *slash=strrchr(identity->launcher,'/'); if (!slash) return -1;
    const char *names[]={"cpufreq.sh","cpuswitch.sh"};
    for (size_t i=0;i<2;++i) {
        char path[GS_PATH]; struct stat st;
        int n=snprintf(path,sizeof(path),"%.*s/%s",(int)(slash-identity->launcher),identity->launcher,names[i]);
        if (n<0 || n>=GS_PATH) return -1;
        if (lstat(path,&st)<0) { if (errno==ENOENT) continue; return -1; }
        char *canonical=realpath(path,NULL);
        int valid=canonical && !strcmp(canonical,path) && S_ISREG(st.st_mode); free(canonical);
        if (!valid) return -1;
        pid_t child=fork(); if (child<0) return -1;
        if (!child) { execl("/bin/sh","sh",path,(char *)NULL); _exit(127); }
        int status=0; uint64_t until=gs_monotonic_ms()+5000;
        for (;;) {
            pid_t ended=waitpid(child,&status,WNOHANG);
            if (ended==child) { if (!WIFEXITED(status) || WEXITSTATUS(status)) return -1; break; }
            if (ended<0) return -1;
            if (gs_monotonic_ms()>=until) {
                if (gs_app_recover(child)) { while (waitpid(child,&status,0)<0 && errno==EINTR) {} }
                return -1;
            }
            struct timespec wait={0,20000000}; nanosleep(&wait,NULL);
        }
    }
    return 0;
}
static int spawn_game(struct gs_session *s,int resume,char directory[GS_PATH]) {
    strcpy(directory,"/tmp/brickos-gs-XXXXXX"); if (!mkdtemp(directory) || nonce(s->session)) return -1;
    int n=snprintf(s->append,GS_PATH,"%s/session.cfg",directory); if (n<0 || n>=GS_PATH) return -1;
    int fd=open(s->append,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if (fd<0) return -1;
    int result=write(fd,gs_private_config,strlen(gs_private_config))==(ssize_t)strlen(gs_private_config) && !fsync(fd)?0:-1;
    close(fd); if (result) return -1;
    int pair[2]; if (socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)) return -1;
    size_t inherited=0; while (environ[inherited]) ++inherited;
    char **environment=calloc(inherited+16,sizeof(*environment)); if (!environment) { close(pair[0]); close(pair[1]); return -1; }
    size_t count=0;
    for (size_t i=0;i<inherited;++i)
        if (strncmp(environ[i],"BRICKOS_GS_",11) && strncmp(environ[i],"LD_PRELOAD=",11) &&
            strncmp(environ[i],"LD_LIBRARY_PATH=",16) && strncmp(environ[i],"HOME=",5)) environment[count++]=environ[i];
    char home[GS_PATH+32],libraries[GS_PATH*2],preload[GS_PATH+16];
    char fields[10][GS_PATH+40];
    snprintf(home,sizeof(home),"HOME=%s/RetroArch",s->sd);
    snprintf(preload,sizeof(preload),"LD_PRELOAD=%s",s->library);
    const char *slash=strrchr(s->library,'/');
    snprintf(libraries,sizeof(libraries),"LD_LIBRARY_PATH=%.*s:/usr/trimui/lib:/usr/lib",(int)(slash-s->library),s->library);
    const char *names[]={"SD","SESSION","APP","LIBRARY","APPEND","ROM","LAUNCHER","CORE","CHANNEL_FD","STORE"};
    const char *values[]={s->sd,s->session,s->app,s->library,s->append,s->identity.rom,s->identity.launcher,s->identity.core,"3",s->store_name};
    environment[count++]=home; environment[count++]=preload; environment[count++]=libraries;
    for (int i=0;i<10;++i) { snprintf(fields[i],sizeof(fields[i]),"BRICKOS_GS_%s=%s",names[i],values[i]); environment[count++]=fields[i]; }
    if (resume) environment[count++]="BRICKOS_GS_RESUME=1";
    char config[GS_PATH+32],ra_directory[GS_PATH+16];
    snprintf(config,sizeof(config),"%s/RetroArch/retroarch.cfg",s->sd);
    snprintf(ra_directory,sizeof(ra_directory),"%s/RetroArch",s->sd);
    char *args[]={s->identity.executable,"-v","--config",config,"--appendconfig",s->append,"-L",s->identity.core,s->identity.rom,NULL};
    // This supervisor is single-threaded. The child performs only async-safe
    // setup, including the stock RetroArch working directory, before execve.
    s->pid=fork();
    if (!s->pid) {
        if (setpgid(0,0) || chdir(ra_directory) || dup2(pair[1],3)<0 || fcntl(3,F_SETFD,0)) _exit(126);
        if (pair[0]!=3) close(pair[0]);
        if (pair[1]!=3) close(pair[1]);
        execve(s->identity.executable,args,environment); _exit(127);
    }
    int error=s->pid<0?errno:0;
    free(environment);
    close(pair[1]);
    if (error) { close(pair[0]); errno=error; return -1; }
    s->channel=pair[0];
    if (gs_process_start(s->pid,&s->process_start)) {
        // No live child may be left behind when returning to init. Recovery is
        // explicitly a failed launch, never authorization for another game.
        recover_group(s->pid);
        close(s->channel); s->channel=-1; return -1;
    }
    return 0;
}
static void cleanup_session(struct gs_session *s,const char *directory) {
    if (s->channel>=0) close(s->channel);
    // Only the exact created file/directory, never recursive deletion or saved data.
    if (*s->append) (void)unlink(s->append);
    if (*directory) (void)rmdir(directory);
}
static int request(struct gs_session *s,enum gs_message_type type,const struct gs_entry *entry,
                   int recovery,char transaction[GS_ID],int paused) {
    if (nonce(transaction) || gs_process_matches(s->pid,s->process_start,&s->identity,s->append) ||
        gs_message_send(s,type,!recovery,entry,transaction)) return -1;
    return paused ? 0:enter(s);
}
static int game(struct gs_store *store,struct gs_session *s,const struct gs_entry *restore,struct gs_entry *next) {
    char id[GS_ID],directory[GS_PATH]={0};
    if (emulator_prepare(&s->identity) || gs_store_played(store,&s->identity,id) || spawn_game(s,restore!=NULL,directory)) {
        cleanup_session(s,directory); return -1;
    }
    struct gs_clock clock; gs_clock_init(&clock);
    struct gs_lifecycle lifecycle; gs_lifecycle_init(&lifecycle,restore!=NULL);
    char load_tx[GS_ID]={0},quit_tx[GS_ID]={0}; int ready=0,have_next=0,quit_sent=0,fault=0,status=0;
    uint64_t boot_deadline=gs_monotonic_ms()+15000,load_deadline=0,last_flush=gs_monotonic_ms(),stop_deadline=0;
    struct gs_power_transactions power_seen={0};
    struct gs_power_message power_current={0}; int power_pending=0; uint64_t power_deadline=0;
    struct gs_standby standby={.phase=GS_STANDBY_IDLE};
    struct gs_battery battery={0}; uint64_t battery_poll=0; int battery_exit=0,shutdown_armed=0,quit_notice=-1;
    for (;;) {
        pid_t ended=waitpid(s->pid,&status,WNOHANG); uint64_t now=gs_monotonic_ms();
        if (ended==s->pid) {
            (void)gs_clock_stop(&clock,now); (void)gs_clock_flush(&clock,store,id,s->session);
            int normal=WIFEXITED(status) && !WEXITSTATUS(status);
            int release=0;
            for (int i=0;i<50 && !release;++i) { release=released(s->pid); if (!release) { struct timespec wait={0,20000000}; nanosleep(&wait,NULL); } }
            gs_lifecycle_finished(&lifecycle,normal,release);
            int handoff=have_next && !fault && gs_lifecycle_handoff(&lifecycle);
            if (!release) { notice(GS_NOTICE_RELEASE_FAILED); recover_group(s->pid); }
            fprintf(stderr,"GameSwitcher exit pid=%ld normal=%d released=%d handoff=%d fault=%d\n",(long)s->pid,normal,release,handoff,fault);
            cleanup_session(s,directory);
            return normal && release && !fault && shutdown_armed?2:handoff?1:(normal && release && !fault?0:-1);
        }
        if (ended<0) { cleanup_session(s,directory); return -1; }
        enum gs_phase before=lifecycle.phase; gs_lifecycle_tick(&lifecycle,now);
        if (before==GS_EXITING && lifecycle.phase==GS_BLOCKED) { fault=1; have_next=0; notice(GS_NOTICE_EXIT_TIMEOUT); }
        struct gs_message m; struct gs_stage prepared;
        int count=gs_message_read_stage(s,&m,&prepared);
        if (count<0 && !fault) { fault=1; have_next=0; notice(GS_NOTICE_CHANNEL_FAILED); }
        if (count==1) {
            fprintf(stderr,"GameSwitcher event pid=%ld type=%u success=%u phase=%d monotonic=%llu\n",
                    (long)s->pid,m.type,m.success,lifecycle.phase,(unsigned long long)m.monotonic_ms);
            if (gs_process_matches(s->pid,s->process_start,&s->identity,s->append)) {
                fault=1; have_next=0; goto events_done;
            }
            uint64_t event_time=m.monotonic_ms<=now && now-m.monotonic_ms<15000?m.monotonic_ms:now;
            if (m.type==GS_HELLO && !ready && !drop_hello) {
                ready=1;
                if (restore) {
                    progress(GS_NOTICE_LOADING);
                    if (request(s,GS_REQUEST_LOAD,restore,0,load_tx,0)) { fault=1; notice(GS_NOTICE_LOAD_ENTER_FAILED); }
                    load_deadline=gs_monotonic_ms()+15000;
                }
            } else if (m.type==GS_MENU_ENTER && ready) {
                int ok=gs_lifecycle_pause(&lifecycle)==0 && !gs_clock_stop(&clock,event_time) &&
                       !gs_clock_flush(&clock,store,id,s->session);
                (void)gs_message_send(s,GS_MENU_ACK,ok,NULL,NULL);
            } else if (m.type==GS_LOAD_RESULT && restore && !strcmp(m.transaction,load_tx)) {
                int accepted=m.success && !strcmp(m.id,restore->id) && !strcmp(m.state,restore->state) && now<load_deadline;
                if (gs_lifecycle_loaded(&lifecycle,accepted)) { fault=1; notice(GS_NOTICE_LOAD_FAILED); }
                else (void)gs_notice_close(GS_NOTICE_LOADING);
                load_deadline=0;
            } else if (m.type==GS_ACTIVE && ready && !fault) {
                if (!gs_lifecycle_active(&lifecycle)) (void)gs_clock_start(&clock,event_time);
            } else if (m.type==GS_SWITCH && !fault) {
                if (!gs_lifecycle_exit(&lifecycle,m.success,now)) {
                    if (*m.id) {
                        if (resolve(store,m.id,m.state,next)) { fault=1; notice(GS_NOTICE_SELECTION_CHANGED); }
                        else have_next=1;
                    }
                } else { fault=1; have_next=0; }
            } else if (m.type==GS_POWER_READY) {
                if (power_drop_ready && m.success) {
                    power_drop_ready=0;
                    fprintf(stderr,"GameSwitcher POWER probe discarded completion transaction=%s\n",m.transaction);
                    gs_stage_close(&prepared); continue;
                }
                int success=0; char published[GS_ID]={0};
                if (power_pending && !strcmp(m.transaction,power_current.transaction) && !fault &&
                    now<power_deadline && m.success && !gs_stage_received(store,&s->identity,&prepared)) {
                    struct gs_receipt receipt={.complete=1}; memcpy(receipt.transaction,prepared.transaction,GS_ID);
                    success=gs_store_publish(store,&s->identity,&prepared,&receipt)==0;
                    if (success) memcpy(published,receipt.transaction,GS_ID);
                }
                int shutdown=success && power_control_enabled && power_current.kind==GS_POWER_LONG;
                if (shutdown) {
                    if (!gs_message_send(s,GS_POWER_QUIT,1,NULL,m.transaction) &&
                        !gs_lifecycle_exit(&lifecycle,1,gs_monotonic_ms())) {
                        shutdown_armed=1; memcpy(shutdown_transaction,m.transaction,GS_ID);
                        memcpy(shutdown_entry,id,GS_ID); memcpy(shutdown_checkpoint,published,GS_ID);
                    } else { success=0; shutdown=0; }
                }
                int wait=success && power_control_enabled && power_current.kind==GS_POWER_SHORT;
                if (wait && !gs_message_send(s,GS_POWER_WAIT,1,NULL,m.transaction))
                    gs_standby_init(&standby,gs_monotonic_ms());
                else {
                    if (wait) success=0;
                    wait=0; if (!shutdown) (void)gs_message_send(s,GS_POWER_DONE,success,NULL,m.transaction);
                }
                if (power_pending && !strcmp(m.transaction,power_current.transaction)) {
                    fprintf(stderr,"GameSwitcher POWER checkpoint transaction=%s committed=%d\n",m.transaction,success);
                    if (!wait) {
                        gs_power_transaction_finish(&power_seen,&power_current,success);
                        (void)gs_power_server_result(&power_server,&power_current,success,gs_monotonic_ms());
                        power_pending=0;
                    }
                    if (!success) notice(GS_NOTICE_POWER_SAVE_FAILED);
                    else (void)gs_notice_close(GS_NOTICE_SAVING);
                }
            } else if (m.type==GS_SAVE_RESULT && quit_sent && !strcmp(m.transaction,quit_tx)) {
                if (m.success) {
                    if (quit_notice>=0) (void)gs_notice_close((enum gs_notice)quit_notice);
                    quit_notice=-1;
                    if (fault) { lifecycle.phase=GS_PAUSED; lifecycle.resume_pending=0; }
                    (void)gs_lifecycle_exit(&lifecycle,1,now);
                } else { quit_sent=0; stopped=0; battery_exit=0; quit_notice=-1; notice(GS_NOTICE_QUIT_SAVE_FAILED); }
            }
        }
events_done:
        gs_stage_close(&prepared);
        now=gs_monotonic_ms();
        struct gs_power_message power_request;
        int power=gs_power_server_poll(&power_server,s,ready && !fault &&
            (lifecycle.phase==GS_RUNNING || lifecycle.phase==GS_PAUSED),power_pending || quit_sent || load_deadline || stopped || battery_exit,
            standby.phase==GS_STANDBY_AWAKE?2:standby.phase!=GS_STANDBY_IDLE,
            &power_request,now);
        if (power>0 && power_request.kind==GS_POWER_RECLAIMED) {
            if (power_pending && standby.phase==GS_STANDBY_AWAKE && !strcmp(power_request.transaction,power_current.transaction)) {
                fprintf(stderr,"GameSwitcher standby transaction=%s reclaimed=1 monotonic=%llu\n",power_current.transaction,(unsigned long long)now);
                (void)gs_message_send(s,GS_POWER_DONE,1,NULL,power_current.transaction);
                gs_power_transaction_finish(&power_seen,&power_current,1);
                (void)gs_power_server_result(&power_server,&power_current,1,now);
                power_pending=0; standby.phase=GS_STANDBY_IDLE;
            }
        } else if (power>0 && power_request.kind==GS_POWER_YIELDED) {
            if (power_pending && !strcmp(power_request.transaction,power_current.transaction) &&
                !gs_standby_yield(&standby,now)) {
                int ok=!gs_standby_pulse(&standby,s);
                if (!ok) standby.phase=GS_STANDBY_FAILED;
                fprintf(stderr,"GameSwitcher standby transaction=%s yielded=1 pulse=%d keymon=%ld\n",
                    power_current.transaction,ok,(long)standby.keymon);
            }
        } else if (power>0) {
            int success=0;
            int fresh=gs_power_transaction_begin(&power_seen,&power_request,&success);
            if (fresh>0) {
                fprintf(stderr,"GameSwitcher POWER probe kind=%u transaction=%s duplicate=0\n",
                    power_request.kind,power_request.transaction);
                if (power_checkpoint_probe && !power_pending && !quit_sent && !load_deadline && !stopped && !battery_exit) {
                    progress(GS_NOTICE_SAVING);
                    int was_active=clock.active;
                    if (!gs_clock_stop(&clock,now) && !gs_clock_flush(&clock,store,id,s->session) &&
                        !gs_message_send(s,GS_POWER_SAVE,1,NULL,power_request.transaction) &&
                        (lifecycle.phase==GS_PAUSED || !enter(s))) {
                        power_current=power_request; power_pending=1; power_deadline=now+15000;
                    } else {
                        if (was_active) (void)gs_clock_start(&clock,gs_monotonic_ms());
                        notice(GS_NOTICE_POWER_ENTER_FAILED);
                    }
                }
            }
            if (!power_pending || strcmp(power_current.transaction,power_request.transaction))
                (void)gs_power_server_result(&power_server,&power_request,success,now);
        }
        if (power_pending && standby.phase!=GS_STANDBY_IDLE) {
            int marker=-1,brightness=-1; enum gs_standby_phase previous=standby.phase;
            if (!gs_standby_snapshot(&marker,&brightness)) gs_standby_observe(&standby,now,marker,brightness);
            else if (now>=standby.deadline) standby.phase=GS_STANDBY_FAILED;
            if (standby.keymon && !gs_standby_keymon_alive(&standby)) standby.phase=GS_STANDBY_FAILED;
            if (standby.phase==GS_STANDBY_AWAKE && now>=standby.deadline) standby.phase=GS_STANDBY_FAILED;
            if (previous!=standby.phase) fprintf(stderr,"GameSwitcher standby transaction=%s phase=%d marker=%d brightness=%d monotonic=%llu\n",
                power_current.transaction,standby.phase,marker,brightness,(unsigned long long)now);
            if (standby.phase==GS_STANDBY_FAILED && marker==0 && brightness>0) {
                int success=0;
                (void)gs_message_send(s,GS_POWER_DONE,success,NULL,power_current.transaction);
                gs_power_transaction_finish(&power_seen,&power_current,success);
                (void)gs_power_server_result(&power_server,&power_current,success,now);
                power_pending=0; standby.phase=GS_STANDBY_IDLE;
                if (!success) notice(GS_NOTICE_STANDBY_CANCELLED);
            }
        }
        if (power_pending && standby.phase==GS_STANDBY_IDLE && now>=power_deadline) {
            (void)gs_message_send(s,GS_POWER_DONE,0,NULL,power_current.transaction);
            (void)gs_power_server_result(&power_server,&power_current,0,now);
            gs_power_transaction_finish(&power_seen,&power_current,0);
            fprintf(stderr,"GameSwitcher POWER checkpoint transaction=%s committed=0 timeout=1\n",power_current.transaction);
            power_pending=0; notice(GS_NOTICE_POWER_TIMEOUT);
        }
        if (!ready && now>=boot_deadline && !fault) { fault=1; notice(GS_NOTICE_BRIDGE_NOT_READY); }
        if (load_deadline && now>=load_deadline) { fault=1; load_deadline=0; notice(GS_NOTICE_LOAD_TIMEOUT); }
        if (fault && clock.active) {
            (void)gs_clock_stop(&clock,now); (void)gs_clock_flush(&clock,store,id,s->session);
        }
        // A missing bridge/receipt cannot leave init waiting indefinitely. Give
        // normal host quit its bounded opportunity, then recover only this
        // launch group. A dark standby stays held until the measured wake.
        if ((stopped || (fault && standby.phase==GS_STANDBY_IDLE)) && !stop_deadline) stop_deadline=now+20000;
        if (stop_deadline && now>=stop_deadline) {
            // Explicit supervisor interruption, bounded recovery only. It cannot
            // authorize a second launch or be reported as a normal host quit.
            uint64_t current;
            if (!gs_process_start(s->pid,&current) && current==s->process_start) {
                notice(GS_NOTICE_RECOVERY_TERMINATION);
                recover_group(s->pid);
            }
            cleanup_session(s,directory); return -1;
        }
        if (ready && !fault && !power_pending && !quit_sent && !load_deadline &&
            (lifecycle.phase==GS_RUNNING || lifecycle.phase==GS_PAUSED) && now-battery_poll>=1000) {
            enum gs_battery_action action=gs_battery_update(&battery,&policy,
                capacity(battery_capacity),capacity(battery_charging));
            battery_poll=now;
            if (action==GS_BATTERY_WARN) notice(GS_NOTICE_BATTERY_LOW);
            if (action==GS_BATTERY_EXIT) { battery_exit=1; progress(GS_NOTICE_BATTERY_CRITICAL); }
        }
        if ((stopped || battery_exit || (fault && restore)) && ready && !quit_sent && !power_pending && lifecycle.phase!=GS_EXITING) {
            int paused=lifecycle.phase==GS_PAUSED;
            // Only a non-recovery quit saves first, so only it shows progress. The save starts
            // in the bridge after this request, and a refused request is retried each pass.
            if (!request(s,GS_REQUEST_QUIT,NULL,fault,quit_tx,paused)) {
                quit_sent=1; quit_notice=fault?-1:battery_exit?GS_NOTICE_BATTERY_CRITICAL:GS_NOTICE_SAVING;
                if (quit_notice==GS_NOTICE_SAVING) progress(GS_NOTICE_SAVING);
            }
            else if (battery_exit) { battery_exit=0; notice(GS_NOTICE_BATTERY_SAVE_FAILED); }
        }
        if (clock.active && now-last_flush>=5000) {
            if (!gs_clock_stop(&clock,now)) {
                if (gs_clock_flush(&clock,store,id,s->session)) notice(GS_NOTICE_PLAY_TIME_FAILED);
                (void)gs_clock_start(&clock,now);
            }
            last_flush=now;
        }
        struct pollfd p={.fd=s->channel,.events=POLLIN}; (void)poll(&p,1,20);
        if (p.revents&(POLLHUP|POLLERR|POLLNVAL)) { struct timespec wait={0,20000000}; nanosleep(&wait,NULL); }
    }
}
int main(int argc,char **argv) {
    BRICKOS_HANDLE_VERSION(argc, argv);
    const char *sd="/mnt/SDCARD",*rom=NULL,*launcher=NULL,*core=NULL,*name="gameswitcher",*power_directory=NULL; int apps=0,boot=0,check=0,check_runtime=0,pick=0;
    const char *resume_id=NULL,*resume_state=NULL;
    char probe_capacity[GS_PATH],probe_charging[GS_PATH];
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--sd") && i+1<argc) sd=argv[++i];
        else if (!strcmp(argv[i],"--namespace") && i+1<argc) name=argv[++i];
        else if (!strcmp(argv[i],"--probe-power") && i+1<argc) power_directory=argv[++i];
        else if (!strcmp(argv[i],"--probe-power-checkpoint") && i+1<argc) { power_directory=argv[++i]; power_checkpoint_probe=1; }
        else if (!strcmp(argv[i],"--power-control") && i+1<argc) {
            power_directory=argv[++i]; power_checkpoint_probe=1; power_control_enabled=1;
        }
        else if (!strcmp(argv[i],"--probe-power-shutdown") && i+1<argc) {
            power_directory=argv[++i]; power_checkpoint_probe=1; power_control_enabled=1; power_shutdown_probe=1;
        }
        else if (!strcmp(argv[i],"--probe-drop-power-ready")) power_drop_ready=1;
        else if (!strcmp(argv[i],"--probe-drop-hello")) drop_hello=1;
        else if (!strcmp(argv[i],"--probe-battery") && i+1<argc) {
            const char *directory=argv[++i];
            if (snprintf(probe_capacity,sizeof(probe_capacity),"%s/capacity",directory)>=GS_PATH ||
                snprintf(probe_charging,sizeof(probe_charging),"%s/charging",directory)>=GS_PATH) return 2;
            battery_capacity=probe_capacity; battery_charging=probe_charging;
        }
        else if (!strcmp(argv[i],"--apps")) apps=1;
        else if (!strcmp(argv[i],"--pick")) pick=1;
        else if (!strcmp(argv[i],"--resume") && i+2<argc) { resume_id=argv[++i]; resume_state=argv[++i]; }
        else if (!strcmp(argv[i],"--boot")) boot=1;
        else if (!strcmp(argv[i],"--check-runtime")) check_runtime=1;
        else if (!strcmp(argv[i],"--check-launch") && i+3<argc) { launcher=argv[++i]; rom=argv[++i]; core=argv[++i]; check=1; }
        else if (!strcmp(argv[i],"--launch") && i+3<argc) { launcher=argv[++i]; rom=argv[++i]; core=argv[++i]; }
        else { fprintf(stderr,"usage: gs-session [--sd ROOT] --apps | --pick | --resume ID STATE | --launch LAUNCHER ROM CORE\n"); return 2; }
    }
    if (apps+pick+!!resume_id+boot+!!rom+check_runtime!=1 || (power_drop_ready && !power_checkpoint_probe)) return 2;
    signal(SIGTERM,stop); signal(SIGINT,stop); signal(SIGHUP,stop);
    if (prctl(PR_SET_CHILD_SUBREAPER,1)<0) { perror("GameSwitcher process ownership"); return 1; }
    if (check || check_runtime) {
        char self[GS_PATH],library[GS_PATH],exe[GS_PATH]; struct gs_identity identity;
        ssize_t length=readlink("/proc/self/exe",self,sizeof(self)-1); if (length<0) return 1; self[length]=0;
        char *slash=strrchr(self,'/'); if (!slash) return 1; *slash=0;
        if (snprintf(library,sizeof(library),"%s/libtmenu.so",self)>=GS_PATH || runtime_valid(library)) return 1;
        if (check && (snprintf(exe,sizeof(exe),"%s/RetroArch/ra64.trimui",sd)>=GS_PATH ||
            gs_identity_make(sd,rom,launcher,core,exe,&identity))) return 1;
        printf("GameSwitcher runtime session_protocol=%08x verified=1\n",GS_SESSION_MAGIC); return 0;
    }
    struct gs_store store; if (gs_store_open_namespace(&store,sd,name)) { perror("GameSwitcher store"); return 1; }
    if (gs_policy_load(&store,&policy)) { notice(GS_NOTICE_INVALID_SETTINGS); gs_policy_defaults(&policy); }
    struct gs_session session={.channel=-1}; snprintf(session.sd,GS_PATH,"%s",store.sd);
    snprintf(session.store_name,sizeof(session.store_name),"%s",name);
    char self[GS_PATH]; ssize_t n=readlink("/proc/self/exe",self,sizeof(self)-1);
    if (n<0) { gs_store_close(&store); return 1; } self[n]=0;
    char *slash=strrchr(self,'/'); if (!slash) { gs_store_close(&store); return 1; } *slash=0;
    if (snprintf(session.app,GS_PATH,"%s/GameSwitcher",self)>=GS_PATH ||
        snprintf(session.library,GS_PATH,"%s/libtmenu.so",self)>=GS_PATH ||
        access(session.app,X_OK) || access(session.library,R_OK)) { gs_store_close(&store); return 1; }
    struct gs_entry restore={0},next; int has_restore=0;
    if (power_directory && gs_power_server_open(&power_server,power_directory)) {
        perror("GameSwitcher POWER endpoint"); gs_store_close(&store); return 1;
    }
    if (boot) {
        // Only a game-aware shutdown's record resumes, once: it goes before anything is decided,
        // also with auto_resume off, so no later boot acts on it.
        struct gs_resume resume; int malformed=0;
        int taken=gs_store_resume_take(&store,&resume,&malformed);
        int target=taken>0 && policy.auto_resume?gs_boot_target(&store,&resume,&restore):0;
        if (taken<0) fprintf(stderr,"GameSwitcher boot: resume record unreadable: %s\n",strerror(errno));
        else if (malformed) fprintf(stderr,"GameSwitcher boot: malformed resume record removed\n");
        else if (!taken) fprintf(stderr,"GameSwitcher boot: no resume record\n");
        else if (!policy.auto_resume) fprintf(stderr,"GameSwitcher boot: resume record removed; auto_resume is off\n");
        else if (!target) fprintf(stderr,"GameSwitcher boot: stale resume record id=%s transaction=%s\n",resume.id,resume.transaction);
        else if (target>0) fprintf(stderr,"GameSwitcher boot: resuming id=%s transaction=%s\n",resume.id,resume.transaction);
        if (target<=0) { if (target<0) notice(GS_NOTICE_LAST_GAME_UNAVAILABLE); gs_power_server_close(&power_server); gs_store_close(&store); return 0; }
        session.identity=restore.identity; has_restore=1;
    } else if (apps || pick) {
        // Every exit closes the POWER endpoint: a socket left behind makes the next
        // launch take its ordinary launcher, without GameSwitcher.
        struct gs_choice_message choice;
        if (app_choice(session.app,session.sd,session.store_name,&choice)) {
            gs_power_server_close(&power_server); gs_store_close(&store); return 1;
        }
        if (choice.action==GS_CHOICE_BACK) { gs_power_server_close(&power_server); gs_store_close(&store); return 0; }
        if (resolve(&store,choice.id,choice.state,&restore)) {
            gs_power_server_close(&power_server); gs_store_close(&store); return 1;
        }
        // The picker MainUI runs names the choice and starts nothing: MainUI hands the init the
        // resume, which starts the game there.
        if (pick) {
            printf("resume %s %s\n",restore.id,restore.state); fflush(stdout);
            gs_power_server_close(&power_server); gs_store_close(&store); return 0;
        }
        session.identity=restore.identity; has_restore=1;
    } else if (resume_id) {
        if (!store_names(resume_id,resume_state) || resolve(&store,resume_id,resume_state,&restore)) {
            fprintf(stderr,"GameSwitcher: no resumable checkpoint %.40s %.40s\n",resume_id,resume_state);
            gs_power_server_close(&power_server); gs_store_close(&store); return 1;
        }
        session.identity=restore.identity; has_restore=1;
    } else {
        char exe[GS_PATH];
        if (snprintf(exe,GS_PATH,"%s/RetroArch/ra64.trimui",store.sd)>=GS_PATH ||
            gs_identity_make(store.sd,rom,launcher,core,exe,&session.identity)) {
            gs_power_server_close(&power_server); gs_store_close(&store); return 1;
        }
    }
    int result=0;
    while (!stopped) {
        int switched=game(&store,&session,has_restore?&restore:NULL,&next);
        if (switched==2) {
            // The next boot resumes this shutdown's checkpoint. The probe never powers off, and a
            // failed poweroff leaves the Brick on, so neither leaves the record behind.
            struct gs_resume resume={0}; int recorded=!power_shutdown_probe;
            memcpy(resume.id,shutdown_entry,GS_ID); memcpy(resume.transaction,shutdown_checkpoint,GS_ID);
            if (recorded && gs_store_resume_write(&store,&resume)) {
                fprintf(stderr,"GameSwitcher: resume record not written: %s\n",strerror(errno)); recorded=0;
            }
            result=shutdown_game()?1:0;
            if (result && recorded && gs_store_resume_remove(&store)) fprintf(stderr,"GameSwitcher: resume record not removed\n");
            break;
        }
        if (switched!=1) { result=switched<0?1:0; break; }
        restore=next; session.identity=restore.identity; has_restore=1;
    }
    gs_power_server_close(&power_server); gs_store_close(&store); return result;
}

#define _GNU_SOURCE
#include "policy.h"
#include "../mainui/json.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
void gs_policy_defaults(struct gs_policy *p) { *p=(struct gs_policy){15,4,1}; }
static int integer(const struct mainui_json *json,const char *key,int *value) {
    const struct mainui_json_member *m=mainui_json_get(json,key);
    if (!m) return 0;
    if (m->type!=MAINUI_JSON_NUMBER || m->number<0 || m->number>100 || (int)m->number!=m->number) return -1;
    *value=(int)m->number; return 0;
}
int gs_policy_load(struct gs_store *s,struct gs_policy *p) {
    gs_policy_defaults(p);
    int fd=openat(s->directory,"settings.json",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return errno==ENOENT?0:-1;
    struct stat st; char text[4096]; ssize_t n=read(fd,text,sizeof(text));
    int regular=!fstat(fd,&st) && S_ISREG(st.st_mode); close(fd);
    struct mainui_json *json=malloc(sizeof(*json)); if (!json) return -1;
    struct gs_policy next=*p;
    int result=!regular || n<=0 || n==(ssize_t)sizeof(text) || mainui_json_parse(json,text,(size_t)n) ||
        integer(json,"warning_percent",&next.warning) || integer(json,"critical_percent",&next.critical) ||
        integer(json,"auto_resume",&next.auto_resume) || next.warning<2 || next.warning>99 ||
        next.critical<1 || next.critical>=next.warning || next.auto_resume>1;
    free(json); if (result) return -1; *p=next; return 0;
}
enum gs_battery_action gs_battery_update(struct gs_battery *s,const struct gs_policy *p,int capacity,int charging) {
    if (capacity<0 || capacity>100 || charging<0) return GS_BATTERY_NONE;
    if (capacity>=p->warning+2 || charging) s->warned=0;
    if (capacity>=p->critical+2 || charging) s->critical_latched=0;
    if (charging) return GS_BATTERY_NONE;
    if (capacity<=p->critical && !s->critical_latched) {
        s->critical_latched=1; s->warned=1; return GS_BATTERY_EXIT;
    }
    if (capacity<p->warning && !s->warned) { s->warned=1; return GS_BATTERY_WARN; }
    return GS_BATTERY_NONE;
}
int gs_boot_target(struct gs_store *s,const struct gs_resume *resume,struct gs_entry *entry) {
    if (!resume || !*resume->id) return 0;
    struct gs_index index; if (gs_store_load(s,&index)) return -1;
    int result=0;
    for (size_t i=0;i<index.count;++i) if (!index.entries[i].hidden && *index.entries[i].state) {
        // Only the checkpoint the shutdown saved; a newer one means the player went on.
        if (strcmp(index.entries[i].id,resume->id) || strcmp(index.entries[i].transaction,resume->transaction)) break;
        result=gs_entry_resumable(s,&index.entries[i])? -1:1;
        if (result>0) *entry=index.entries[i];
        break;
    }
    gs_index_close(&index); return result;
}
int gs_toast_record(char *out,size_t size,const char *message,int duration_ms) {
    char escaped[1024];
    if (!message || !*message || duration_ms<1 ||
        mainui_json_escape(escaped,sizeof(escaped),message,strlen(message))<0) return -1;
    // The wide bottom box of MainUI's own notices: size class 2 holds about 40 characters,
    // where the default top-right box clips anything longer than the progress texts.
    int n=snprintf(out,size,"{\"type\":\"default\",\"id\":\"com.brickos.gameswitcher\",\"duration\":%d,"
                   "\"size\":2,\"x\":212,\"y\":550,\"w\":600,\"h\":80,\"message\":%s}",duration_ms,escaped);
    return n<0 || (size_t)n>=size?-1:n;
}
static int toast_for(const char *message,int duration_ms) {
    char record[1280]; int n=gs_toast_record(record,sizeof(record),message,duration_ms);
    if (n<0) return -1;
    int fd=open("/tmp/trimui_osd/osd_toast_msg",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0644);
    if (fd<0) return -1;
    int result=write(fd,record,(size_t)n)==n?0:-1; close(fd); return result;
}
int gs_toast(const char *message) { return toast_for(message,4000); }

// At most GS_NOTICE_CHARACTERS characters each, so the text fits the toast box.
// The English battery, save-failed, link, recovery and auto-resume texts are read
// in logs by the integration driver and the guide: change both together.
static const char *const notice_text[GS_NOTICE_COUNT][2]={
    [GS_NOTICE_SAVING]={"Saving game...","A guardar o jogo..."},
    [GS_NOTICE_LOADING]={"Loading game...","A carregar o jogo..."},
    [GS_NOTICE_POWER_COMPLETION_UNAVAILABLE]={"Save incomplete; power cancelled","Jogo não guardado; energia cancelada"},
    [GS_NOTICE_SAVE_FAILED]={"Save failed; game continues","Falha ao guardar; o jogo continua"},
    [GS_NOTICE_UNEXPECTED_REQUEST]={"Unexpected GameSwitcher request","Pedido inesperado do GameSwitcher"},
    [GS_NOTICE_SESSION_UNAVAILABLE]={"No GameSwitcher; opening game menu","Sem GameSwitcher; a abrir o menu"},
    [GS_NOTICE_CLOSED_WITHOUT_SELECTION]={"GameSwitcher closed; no game chosen","GameSwitcher fechado sem escolha"},
    [GS_NOTICE_SELECTION_UNAVAILABLE]={"Selected game unavailable","Jogo escolhido indisponível"},
    [GS_NOTICE_SHUTDOWN_PROGRESS_UNAVAILABLE]={"Cannot power off; game saved","Não desligou; o jogo está guardado"},
    [GS_NOTICE_POWEROFF_START_FAILED]={"Cannot power off; game saved","Não desligou; o jogo está guardado"},
    [GS_NOTICE_POWEROFF_FAILED]={"Power off failed; game saved","Falha ao desligar; jogo guardado"},
    [GS_NOTICE_RELEASE_FAILED]={"Game did not release; recovering","O jogo não libertou; a recuperar"},
    [GS_NOTICE_EXIT_TIMEOUT]={"Game did not close; switch blocked","O jogo não fechou; troca cancelada"},
    [GS_NOTICE_CHANNEL_FAILED]={"Game session link failed","Falha na ligação ao jogo"},
    [GS_NOTICE_LOAD_ENTER_FAILED]={"Cannot start loading the game","Não foi possível carregar o jogo"},
    [GS_NOTICE_LOAD_FAILED]={"Load failed; returning to menu","Falha ao carregar; a voltar ao menu"},
    [GS_NOTICE_SELECTION_CHANGED]={"Saved game changed; switch blocked","O jogo guardado mudou; troca cancelada"},
    [GS_NOTICE_POWER_SAVE_FAILED]={"Save failed; power cancelled","Falha ao guardar; energia cancelada"},
    [GS_NOTICE_QUIT_SAVE_FAILED]={"Save failed; game continues","Falha ao guardar; o jogo continua"},
    [GS_NOTICE_POWER_ENTER_FAILED]={"Cannot save; power cancelled","Não guardou; energia cancelada"},
    [GS_NOTICE_STANDBY_CANCELLED]={"Standby cancelled; game continues","Suspensão cancelada; o jogo continua"},
    [GS_NOTICE_POWER_TIMEOUT]={"Save timed out; power cancelled","Tempo esgotado; energia cancelada"},
    [GS_NOTICE_BRIDGE_NOT_READY]={"Game link not ready","A ligação ao jogo não ficou pronta"},
    [GS_NOTICE_LOAD_TIMEOUT]={"Load timed out","Tempo esgotado a carregar o jogo"},
    [GS_NOTICE_RECOVERY_TERMINATION]={"Game stopped to recover; no switch","Jogo parado para recuperar"},
    [GS_NOTICE_BATTERY_LOW]={"Battery low; connect a charger","Bateria fraca; liga o carregador"},
    [GS_NOTICE_BATTERY_CRITICAL]={"Battery critical; saving game...","Bateria crítica; a guardar o jogo..."},
    [GS_NOTICE_BATTERY_SAVE_FAILED]={"Low-battery save failed","Falha ao guardar com bateria fraca"},
    [GS_NOTICE_PLAY_TIME_FAILED]={"Play time not stored","Tempo de jogo não guardado"},
    [GS_NOTICE_INVALID_SETTINGS]={"Invalid settings; using defaults","Definições inválidas; valores padrão"},
    [GS_NOTICE_LAST_GAME_UNAVAILABLE]={"Last game missing; auto-resume skipped","Último jogo em falta; retoma ignorada"},
};
const char *gs_notice_text(enum gs_notice notice,int portuguese) {
    if ((unsigned)notice>=GS_NOTICE_COUNT) return NULL;
    return notice_text[notice][portuguese?1:0];
}
int gs_notice_portuguese(const char *path) {
    int fd=open(path,O_RDONLY|O_CLOEXEC); if (fd<0) return 0;
    char text[16384]; ssize_t n=read(fd,text,sizeof(text)); close(fd);
    if (n<=0 || n==(ssize_t)sizeof(text)) return 0;
    struct mainui_json *json=malloc(sizeof(*json)); if (!json) return 0;
    int result=0;
    if (!mainui_json_parse_fitting(json,text,(size_t)n)) {
        const struct mainui_json_member *m=mainui_json_get(json,"language");
        result=m && m->type==MAINUI_JSON_STRING && !strcasecmp(m->text,"pt.lang");
    }
    free(json); return result;
}
static const char *localized(enum gs_notice notice) {
    return gs_notice_text(notice,gs_notice_portuguese(GS_SYSTEM_JSON));
}
int gs_notice(enum gs_notice notice) { return toast_for(localized(notice),4000); }
int gs_notice_progress(enum gs_notice notice) { return toast_for(localized(notice),15000); }
// The official OSD ignores a replacement with 0 ms or an empty message; this one closes it.
int gs_notice_close(enum gs_notice notice) { return toast_for(localized(notice),1); }
int gs_shutdown_progress(const char *directory,const char *transaction) {
    if (!directory || strnlen(transaction,GS_ID)!=32 || strspn(transaction,"0123456789abcdef")!=32) return -1;
    int parent=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW); if (parent<0) return -1;
    int fd=openat(parent,"poweroff_flag",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) { close(parent); return -1; }
    int result=write(fd,transaction,32)==32 && !fsync(fd) && !fsync(parent)?0:-1;
    close(fd); if (result) unlinkat(parent,"poweroff_flag",0); close(parent); return result;
}

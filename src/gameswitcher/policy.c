#define _GNU_SOURCE
#include "policy.h"
#include "history.h"
#include "../mainui/json.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
void gs_policy_defaults(struct gs_policy *p) { *p=(struct gs_policy){15,4,1,1,0,0}; }
static const char *const keys[]={"warning_percent","critical_percent","auto_resume","double_tap","guide_menu","history_limit"};
#define KEYS (sizeof(keys)/sizeof(keys[0]))
static int *field(struct gs_policy *p,size_t key) {
    int *fields[KEYS]={&p->warning,&p->critical,&p->auto_resume,&p->double_tap,&p->guide_menu,&p->history_limit};
    return fields[key];
}
static int key_index(const char *key) {
    for (size_t i=0;i<KEYS;++i) if (!strcasecmp(key,keys[i])) return (int)i;
    return -1;
}
static int valid(const struct gs_policy *p) {
    return p->warning>=2 && p->warning<=99 && p->critical>=1 && p->critical<p->warning &&
           p->auto_resume<=1 && p->double_tap<=1 && p->guide_menu<=1 && p->history_limit>=0 && p->history_limit<=100;
}
static int integer(const struct mainui_json_member *m,int *value) {
    if (m->type!=MAINUI_JSON_NUMBER || m->number<0 || m->number>100 || (int)m->number!=m->number) return -1;
    *value=(int)m->number; return 0;
}
// The file's text and its members; json->count is 0 without a file. -1 when it cannot be read or
// is not a flat object that fits.
static int read_settings(struct gs_store *s,struct mainui_json *json) {
    json->count=0;
    int fd=openat(s->directory,"settings.json",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return errno==ENOENT?0:-1;
    struct stat st; char text[4096]; ssize_t n=read(fd,text,sizeof(text));
    int regular=!fstat(fd,&st) && S_ISREG(st.st_mode); close(fd);
    if (!regular || n<=0 || n==(ssize_t)sizeof(text) || mainui_json_parse(json,text,(size_t)n)) { errno=EINVAL; return -1; }
    return 0;
}
static int apply(const struct mainui_json *json,struct gs_policy *p) {
    for (size_t i=0;i<KEYS;++i) {
        const struct mainui_json_member *m=mainui_json_get(json,keys[i]);
        if (m && integer(m,field(p,i))) return -1;
    }
    return valid(p)?0:-1;
}
int gs_policy_load(struct gs_store *s,struct gs_policy *p) {
    gs_policy_defaults(p);
    struct mainui_json *json=malloc(sizeof(*json)); if (!json) return -1;
    struct gs_policy next=*p;
    int result=read_settings(s,json) || apply(json,&next);
    free(json); if (result) return -1; *p=next; return 0;
}
static int write_member(FILE *file,int *first,const char *key,const char *value) {
    char escaped[MAINUI_JSON_KEY*6+3];
    if (mainui_json_escape(escaped,sizeof(escaped),key,strlen(key))<0) return -1;
    int n=fprintf(file,"%s%s:%s",*first?"":",",escaped,value); *first=0; return n<0?-1:0;
}
int gs_policy_set(struct gs_store *s,const char *key,int value) {
    int index=key_index(key);
    if (index<0) { errno=EINVAL; return -1; }
    struct mainui_json *json=malloc(sizeof(*json)); if (!json) return -1;
    // The values in use, as a session reads them: a file that does not parse keeps no member.
    struct gs_policy p; (void)gs_policy_load(s,&p);
    if (read_settings(s,json)) json->count=0;
    *field(&p,(size_t)index)=value;
    if (!valid(&p)) { free(json); errno=EINVAL; return -1; }
    int result=-1,first=1,written=0; char number[32];
    (void)unlinkat(s->directory,"settings.json.brickos-tmp",0);
    int fd=openat(s->directory,"settings.json.brickos-tmp",O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
    FILE *file=fd<0?NULL:fdopen(fd,"w");
    if (!file) { if (fd>=0) close(fd); free(json); return -1; }
    if (fputc('{',file)==EOF) goto done;
    for (size_t i=0;i<json->count;++i) {
        const struct mainui_json_member *m=&json->members[i];
        int known=key_index(m->key);
        char value_text[MAINUI_JSON_TEXT*6+3];
        if (known>=0) {
            snprintf(number,sizeof(number),"%d",*field(&p,(size_t)known));
            if (write_member(file,&first,keys[known],number)) goto done;
            if (known==index) written=1;
            continue;
        }
        if (m->type==MAINUI_JSON_STRING) {
            if (mainui_json_escape(value_text,sizeof(value_text),m->text,m->text_length)<0) goto done;
        } else if (m->type==MAINUI_JSON_NUMBER) {
            if (m->number>-1e15 && m->number<1e15 && m->number==(double)(long long)m->number)
                snprintf(value_text,sizeof(value_text),"%lld",(long long)m->number);
            else snprintf(value_text,sizeof(value_text),"%.17g",m->number);
        } else snprintf(value_text,sizeof(value_text),"%s",m->text);
        if (write_member(file,&first,m->key,value_text)) goto done;
    }
    snprintf(number,sizeof(number),"%d",value);
    if (!written && write_member(file,&first,keys[index],number)) goto done;
    if (fputs("}\n",file)==EOF || fflush(file) || fsync(fileno(file))) goto done;
    result=0;
done:
    if (fclose(file)) result=-1;
    if (!result && (renameat(s->directory,"settings.json.brickos-tmp",s->directory,"settings.json") || fsync(s->directory))) result=-1;
    if (result) (void)unlinkat(s->directory,"settings.json.brickos-tmp",0);
    free(json); return result;
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
void gs_policy_trim_history(struct gs_store *s) {
    struct gs_policy p; size_t removed=0;
    if (gs_policy_load(s,&p) || !p.history_limit) return;
    if (gs_history_trim(s,(unsigned)p.history_limit,&removed))
        fprintf(stderr,"GameSwitcher history limit %d failed: %s\n",p.history_limit,strerror(errno));
    else if (removed) fprintf(stderr,"GameSwitcher history limit %d removed=%zu\n",p.history_limit,removed);
}
int gs_boot_target(struct gs_store *s,const struct gs_resume *resume,struct gs_entry *entry) {
    if (!resume || !*resume->id) return 0;
    struct gs_excluded excluded; struct gs_index index;
    if (gs_excluded_load(s,&excluded) || gs_store_load(s,&index)) return -1;
    int result=0;
    for (size_t i=0;i<index.count;++i) if (!index.entries[i].hidden && *index.entries[i].state) {
        // Only the checkpoint the shutdown saved; a newer one means the player went on.
        if (strcmp(index.entries[i].id,resume->id) || strcmp(index.entries[i].transaction,resume->transaction)) break;
        // A system the player excluded since starts at Home.
        if (gs_excluded_entry(s,&excluded,&index.entries[i])) break;
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

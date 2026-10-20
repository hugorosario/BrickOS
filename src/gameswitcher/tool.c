#define _GNU_SOURCE
#include "tool.h"
#include "history.h"
#include "policy.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(const char *text,int *value) {
    if (!*text || strlen(text)>3 || strspn(text,"0123456789")!=strlen(text)) return -1;
    *value=atoi(text); return 0;
}
static int run(struct gs_store *store,int argc,char **argv) {
    const char *command=argc>0?argv[0]:"";
    if (!strcmp(command,"get") && argc==1) {
        struct gs_policy p; int valid=!gs_policy_load(store,&p);
        if (!valid) gs_policy_defaults(&p);
        printf("warning_percent=%d\ncritical_percent=%d\nauto_resume=%d\ndouble_tap=%d\nguide_menu=%d\nhistory_limit=%d\nvalid=%d\n",
               p.warning,p.critical,p.auto_resume,p.double_tap,p.guide_menu,p.history_limit,valid);
        return 0;
    }
    if (!strcmp(command,"set") && argc==3) {
        int value;
        if (number(argv[2],&value)) return 2;
        if (gs_policy_set(store,argv[1],value)) {
            if (errno==EINVAL) { printf("refused=%s\n",argv[1]); return 1; }
            printf("failed=%s\n",strerror(errno)); return 1;
        }
        if (!strcmp(argv[1],"history_limit")) {
            size_t removed=0;
            if (gs_history_trim(store,(unsigned)value,&removed)) { printf("failed=%s\n",strerror(errno)); return 1; }
            printf("removed=%zu\n",removed);
        }
        return 0;
    }
    if (!strcmp(command,"info") && argc==1) {
        struct gs_history_info info;
        if (gs_history_info(store,&info)) { printf("failed=%s\n",strerror(errno)); return 1; }
        printf("games=%zu\nbytes=%" PRIu64 "\nplay_ms=%" PRIu64 "\n",info.games,info.bytes,info.play_ms);
        return 0;
    }
    if (!strcmp(command,"clear") && argc==1) {
        size_t cleared;
        if (gs_history_clear(store,&cleared)) { printf("failed=%s\n",strerror(errno)); return 1; }
        printf("cleared=%zu\n",cleared); return 0;
    }
    if (!strcmp(command,"reset-time") && argc==1) {
        size_t reset;
        if (gs_history_reset_time(store,&reset)) { printf("failed=%s\n",strerror(errno)); return 1; }
        printf("reset=%zu\n",reset); return 0;
    }
    return 2;
}
int gs_settings_command(const char *sd,const char *name,int argc,char **argv) {
    struct gs_store store;
    if (gs_store_open_namespace(&store,sd,name)) { printf("failed=%s\n",strerror(errno)); return 1; }
    int result=run(&store,argc,argv);
    if (result==2) fprintf(stderr,"usage: gs-session [--sd ROOT] --settings get | set KEY VALUE | info | clear | reset-time\n");
    gs_store_close(&store); return result;
}

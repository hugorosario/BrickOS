#define _GNU_SOURCE
#include "app_process.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
int gs_app_spawn(const char *app,const char *sd,const char *name,int owner,pid_t *child,int *channel) {
    int pair[2]; if (socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)) return -1;
    size_t count=0; while (environ[count]) ++count;
    char **environment=calloc(count+2,sizeof(*environment));
    if (!environment) { close(pair[0]); close(pair[1]); return -1; }
    size_t used=0;
    for (size_t i=0;i<count;++i)
        if (strncmp(environ[i],"LD_PRELOAD=",11) && strncmp(environ[i],"LD_LIBRARY_PATH=",16)) environment[used++]=environ[i];
    environment[used]="LD_LIBRARY_PATH=/usr/trimui/lib:/usr/lib";
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    int error=posix_spawn_file_actions_adddup2(&actions,pair[1],3);
    if (pair[0]!=3) error|=posix_spawn_file_actions_addclose(&actions,pair[0]);
    if (pair[1]!=3) error|=posix_spawn_file_actions_addclose(&actions,pair[1]);
    if (owner>=0 && owner!=3 && owner!=pair[0] && owner!=pair[1]) error|=posix_spawn_file_actions_addclose(&actions,owner);
    char *args[]={(char *)app,"--sd",(char *)sd,"--namespace",(char *)name,"--channel-fd","3",NULL};
    if (!error) error=posix_spawn(child,app,&actions,NULL,args,environment);
    posix_spawn_file_actions_destroy(&actions); free(environment);
    if (error) { close(pair[0]); close(pair[1]); errno=error; return -1; }
    close(pair[1]); *channel=pair[0]; return 0;
}
int gs_app_recover(pid_t pid) {
    (void)kill(pid,SIGTERM);
    for (int i=0;i<150;++i) {
        int status; pid_t ended=waitpid(pid,&status,WNOHANG);
        if (ended==pid || (ended<0 && errno==ECHILD)) return 0;
        struct timespec wait={0,20000000}; nanosleep(&wait,NULL);
    }
    (void)kill(pid,SIGKILL); // Recovery only, never accepted as a normal selection.
    for (int i=0;i<100;++i) {
        int status; pid_t ended=waitpid(pid,&status,WNOHANG);
        if (ended==pid || (ended<0 && errno==ECHILD)) return 0;
        struct timespec wait={0,20000000}; nanosleep(&wait,NULL);
    }
    return -1;
}

#define _GNU_SOURCE
#include "session.h"
#include "channel.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char gs_private_config[]=
    "network_cmd_enable = \"false\"\nstdin_cmd_enable = \"false\"\n"
    "savestate_auto_save = \"false\"\nsavestate_auto_load = \"false\"\n"
    "savestate_file_compression = \"false\"\nsavestate_thumbnail_enable = \"false\"\n"
    "config_save_on_exit = \"false\"\n";
uint64_t gs_monotonic_ms(void) {
    struct timespec ts; if (clock_gettime(CLOCK_MONOTONIC,&ts)) return UINT64_MAX;
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}
int gs_stat_start(const char *line,uint64_t *start) {
    const char *p=strrchr(line,')');
    if (!p || p[1]!=' ' || !p[2] || p[3]!=' ' || p[2]=='Z' || p[2]=='X') return -1;
    p+=4;
    for (int field=4;field<=22;++field) {
        while (*p==' ') ++p;
        if (!*p || *p=='\n') return -1;
        const char *end=strchr(p,' ');
        if (field==22) {
            char *tail; errno=0;
            if (*p<'0' || *p>'9') return -1;
            unsigned long long value=strtoull(p,&tail,10);
            if (errno || tail==p || (*tail && *tail!=' ' && *tail!='\n') || !value) return -1;
            *start=(uint64_t)value; return 0;
        }
        if (!end) return -1;
        p=end+1;
    }
    return -1;
}
int gs_process_start(pid_t pid,uint64_t *start) {
    char path[64],line[4096]; snprintf(path,sizeof(path),"/proc/%ld/stat",(long)pid);
    FILE *file=fopen(path,"r"); if (!file) return -1;
    int result=fgets(line,sizeof(line),file)?gs_stat_start(line,start):-1;
    fclose(file); return result;
}
static int canonical_equal(const char *path,const char *expected) {
    char *resolved=realpath(path,NULL); int result=resolved && !strcmp(resolved,expected);
    free(resolved); return result;
}
int gs_process_matches(pid_t pid,uint64_t expected,const struct gs_identity *identity,const char *append) {
    uint64_t start; if (gs_process_start(pid,&start) || start!=expected) return -1;
    char path[64],exe[GS_PATH]; snprintf(path,sizeof(path),"/proc/%ld/exe",(long)pid);
    ssize_t n=readlink(path,exe,sizeof(exe)-1); if (n<0) return -1; exe[n]=0;
    if (strcmp(exe,identity->executable)) return -1;
    snprintf(path,sizeof(path),"/proc/%ld/cmdline",(long)pid);
    int fd=open(path,O_RDONLY|O_CLOEXEC); if (fd<0) return -1;
    char args[32768]; n=read(fd,args,sizeof(args)); close(fd);
    if (n<=0 || n==(ssize_t)sizeof(args) || args[n-1]) return -1;
    int core=0,rom=0,config=0;
    for (size_t at=0;at<(size_t)n;) {
        const char *arg=args+at; size_t size=strlen(arg)+1; at+=size;
        if (!strcmp(arg,"-L") || !strcmp(arg,"--libretro")) {
            if (at>=(size_t)n || core++ || !canonical_equal(args+at,identity->core)) return -1;
            at+=strlen(args+at)+1;
        } else if (!strcmp(arg,"--appendconfig")) {
            if (at>=(size_t)n || config++ || !canonical_equal(args+at,append)) return -1;
            at+=strlen(args+at)+1;
        } else if (!strcmp(arg,identity->rom)) ++rom;
    }
    return core==1 && rom==1 && config==1 ? 0:-1;
}
static int environment(const char *name,char *out,size_t size) {
    const char *value=getenv(name);
    if (!value || !*value || strlen(value)>=size) return -1;
    strcpy(out,value); return 0;
}
static int hex(const char *id) { return strnlen(id,GS_ID)==32 && strspn(id,"0123456789abcdef")==32; }
int gs_session_environment(struct gs_session *s,const void *symbol) {
    memset(s,0,sizeof(*s)); s->channel=-1;
    const char *value=getenv("BRICKOS_GS_CHANNEL_FD"); char *end;
    if (!value || !*value) return -1;
    long fd=strtol(value,&end,10); if (*end || fd<3 || fd>1024 || gs_channel_parent((int)fd)) return -1;
    s->channel=(int)fd; s->pid=getpid();
    if (environment("BRICKOS_GS_SD",s->sd,GS_PATH) ||
        environment("BRICKOS_GS_STORE",s->store_name,sizeof(s->store_name)) || !gs_store_name_valid(s->store_name) ||
        environment("BRICKOS_GS_SESSION",s->session,GS_ID) || !hex(s->session) ||
        environment("BRICKOS_GS_APP",s->app,GS_PATH) ||
        environment("BRICKOS_GS_LIBRARY",s->library,GS_PATH) ||
        environment("BRICKOS_GS_APPEND",s->append,GS_PATH) ||
        gs_process_start(s->pid,&s->process_start)) return -1;
    const char *rom=getenv("BRICKOS_GS_ROM"),*launcher=getenv("BRICKOS_GS_LAUNCHER"),*core=getenv("BRICKOS_GS_CORE");
    char executable[GS_PATH]; int n=snprintf(executable,sizeof(executable),"%s/RetroArch/ra64.trimui",s->sd);
    if (n<0 || n>=GS_PATH || gs_identity_make(s->sd,rom,launcher,core,executable,&s->identity)) return -1;
    Dl_info info;
    if (!dladdr(symbol,&info) || !canonical_equal(info.dli_fname,s->library) ||
        gs_process_matches(s->pid,s->process_start,&s->identity,s->append)) return -1;
    // Private immutable append config, never a global file or symlinked directory.
    const char *slash=strrchr(s->append,'/'); char parent[GS_PATH]; struct stat st;
    if (!slash || strcmp(slash+1,"session.cfg")) return -1;
    memcpy(parent,s->append,(size_t)(slash-s->append)); parent[slash-s->append]=0;
    if (lstat(parent,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0777)!=0700) return -1;
    fd=open(s->append,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return -1;
    char config[sizeof(gs_private_config)]; ssize_t count=read((int)fd,config,sizeof(config));
    int ok=fstat((int)fd,&st)==0 && S_ISREG(st.st_mode) && count==(ssize_t)sizeof(gs_private_config)-1 &&
           !memcmp(config,gs_private_config,sizeof(gs_private_config)-1);
    close((int)fd);
    return ok?0:-1;
}
int gs_session_core(const struct gs_session *s,const void *symbol) {
    Dl_info info; return dladdr(symbol,&info) && canonical_equal(info.dli_fname,s->identity.core)?0:-1;
}
int gs_message_send(const struct gs_session *s,enum gs_message_type type,int success,
                    const struct gs_entry *entry,const char *transaction) {
    struct gs_message m={.magic=GS_SESSION_MAGIC,.type=(uint32_t)type,.pid=(uint32_t)s->pid,
                         .success=!!success,.process_start=s->process_start,.monotonic_ms=gs_monotonic_ms()};
    memcpy(m.session,s->session,GS_ID);
    if (entry) { memcpy(m.id,entry->id,GS_ID); memcpy(m.state,entry->state,GS_ASSET); }
    if (transaction && hex(transaction)) memcpy(m.transaction,transaction,GS_ID);
    return send(s->channel,&m,sizeof(m),MSG_NOSIGNAL|MSG_DONTWAIT)==sizeof(m)?0:-1;
}
int gs_message_ready(const struct gs_session *s,const struct gs_stage *stage,const char *transaction) {
    if (!hex(transaction) || stage->directory<0 || stage->lock<0) return -1;
    struct gs_message m={.magic=GS_SESSION_MAGIC,.type=GS_POWER_READY,.pid=(uint32_t)s->pid,
        .success=1,.process_start=s->process_start,.monotonic_ms=gs_monotonic_ms(),.stage_deadline_ms=stage->deadline_ms};
    memcpy(m.session,s->session,GS_ID); memcpy(m.transaction,transaction,GS_ID);
    memcpy(m.stage_state,stage->state,sizeof(m.stage_state)); memcpy(m.stage_thumbnail,stage->thumbnail,sizeof(m.stage_thumbnail));
    memcpy(m.stage_transaction,stage->transaction,GS_ID);
    int descriptors[]={stage->directory,stage->lock};
    union { struct cmsghdr aligned; char bytes[CMSG_SPACE(sizeof(descriptors))]; } control={0};
    struct iovec data={.iov_base=&m,.iov_len=sizeof(m)};
    struct msghdr header={.msg_iov=&data,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    struct cmsghdr *c=CMSG_FIRSTHDR(&header); c->cmsg_level=SOL_SOCKET; c->cmsg_type=SCM_RIGHTS;
    c->cmsg_len=CMSG_LEN(sizeof(descriptors)); memcpy(CMSG_DATA(c),descriptors,sizeof(descriptors));
    return sendmsg(s->channel,&header,MSG_DONTWAIT|MSG_NOSIGNAL)==sizeof(m)?0:-1;
}
int gs_message_read_stage(const struct gs_session *s,struct gs_message *m,struct gs_stage *stage) {
    memset(stage,0,sizeof(*stage)); stage->directory=stage->lock=-1;
    union { struct cmsghdr aligned; char bytes[CMSG_SPACE(sizeof(int)*8)]; } control={0};
    struct iovec data={.iov_base=m,.iov_len=sizeof(*m)};
    struct msghdr header={.msg_iov=&data,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    ssize_t n=recvmsg(s->channel,&header,MSG_DONTWAIT|MSG_TRUNC|MSG_CMSG_CLOEXEC);
    if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
    int fds[8],count=0,bad=0;
    for (struct cmsghdr *c=CMSG_FIRSTHDR(&header);c;c=CMSG_NXTHDR(&header,c)) {
        if (c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS || c->cmsg_len<CMSG_LEN(0)) { bad=1; continue; }
        size_t bytes=c->cmsg_len-CMSG_LEN(0);
        if (bytes%sizeof(int)) bad=1;
        for (size_t i=0;i<bytes/sizeof(int);++i) {
            int fd; memcpy(&fd,(char *)CMSG_DATA(c)+i*sizeof(int),sizeof(fd));
            if (count<8) fds[count++]=fd; else { close(fd); bad=1; }
        }
    }
    if (bad || (header.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) || n!=sizeof(*m) ||
        m->magic!=GS_SESSION_MAGIC || m->type<GS_HELLO || m->type>GS_POWER_QUIT ||
        m->pid!=(uint32_t)s->pid || m->process_start!=s->process_start || m->success>1 ||
        memcmp(m->session,s->session,GS_ID) || !memchr(m->id,0,GS_ID) ||
        !memchr(m->state,0,GS_ASSET) || !memchr(m->transaction,0,GS_ID) ||
        (m->type==GS_POWER_READY && m->success ? count!=2:count!=0)) {
        for (int i=0;i<count;++i) close(fds[i]);
        return -1;
    }
    if (count==2) {
        if (!memchr(m->stage_state,0,sizeof(m->stage_state)) || !memchr(m->stage_thumbnail,0,sizeof(m->stage_thumbnail)) ||
            !hex(m->stage_transaction) || !hex(m->transaction)) { close(fds[0]); close(fds[1]); return -1; }
        stage->directory=fds[0]; stage->lock=fds[1]; stage->deadline_ms=m->stage_deadline_ms; stage->identity=s->identity;
        memcpy(stage->state,m->stage_state,sizeof(stage->state)); memcpy(stage->thumbnail,m->stage_thumbnail,sizeof(stage->thumbnail));
        memcpy(stage->transaction,m->stage_transaction,GS_ID);
    }
    return 1;
}
int gs_message_read(const struct gs_session *s,struct gs_message *m) {
    struct gs_stage stage; int result=gs_message_read_stage(s,m,&stage);
    if (stage.directory>=0) { gs_stage_close(&stage); return -1; }
    return result;
}

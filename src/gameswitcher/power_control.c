#define _GNU_SOURCE
#include "power_control.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int hex(const char *id) { return strnlen(id,GS_ID)==32 && strspn(id,"0123456789abcdef")==32; }
static int text(const char *value,size_t size) { return value[0] && memchr(value,0,size)!=NULL; }
int gs_power_transaction_begin(struct gs_power_transactions *t,const struct gs_power_message *m,int *success) {
    if (!hex(m->transaction) || (m->kind!=GS_POWER_SHORT && m->kind!=GS_POWER_LONG)) return -1;
    char *end; errno=0; uint64_t sequence=strtoull(m->transaction+16,&end,16);
    if (errno || *end || !sequence) return -1;
    unsigned i;
    for (i=0;i<t->count;++i) if (!strncmp(t->issuers[i].issuer,m->transaction,16)) break;
    if (i==t->count) {
        if (t->count==16) return -1;
        memcpy(t->issuers[i].issuer,m->transaction,16); t->issuers[i].issuer[16]=0; ++t->count;
    }
    if (sequence<=t->issuers[i].sequence) {
        if (sequence==t->issuers[i].sequence && t->issuers[i].kind!=m->kind) return -1;
        *success=sequence==t->issuers[i].sequence && t->issuers[i].success; return 0;
    }
    t->issuers[i].sequence=sequence; t->issuers[i].kind=m->kind; t->issuers[i].success=0;
    *success=0; return 1;
}
void gs_power_transaction_finish(struct gs_power_transactions *t,const struct gs_power_message *m,int success) {
    for (unsigned i=0;i<t->count;++i) if (!strncmp(t->issuers[i].issuer,m->transaction,16) &&
        t->issuers[i].sequence==strtoull(m->transaction+16,NULL,16) && t->issuers[i].kind==m->kind)
        t->issuers[i].success=!!success;
}
static int path(char out[108],const char *directory) {
    if (!directory || directory[0]!='/' || strchr(directory,'\n')) return -1;
    int n=snprintf(out,108,"%s/control",directory);
    return n>0 && n<108?0:-1;
}
static int private_directory(const char *directory) {
    struct stat st; char *canonical=realpath(directory,NULL);
    int ok=canonical && !strcmp(canonical,directory) && !lstat(directory,&st) && S_ISDIR(st.st_mode) &&
           st.st_uid==geteuid() && (st.st_mode&0777)==0700;
    free(canonical); return ok?0:-1;
}
static int peer(int fd,pid_t *pid,uint64_t *start) {
    struct ucred cred; socklen_t size=sizeof(cred);
    if (getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&size) || size!=sizeof(cred) ||
        cred.uid!=geteuid() || cred.pid<=0 || gs_process_start(cred.pid,start)) return -1;
    *pid=cred.pid; return 0;
}
int gs_power_wire_valid(const struct gs_power_message *m,uint64_t now) {
    const struct gs_session *s=&m->session;
    if (m->magic!=GS_POWER_MAGIC || m->kind<GS_POWER_LEASE || m->kind>GS_POWER_RECLAIMED ||
        m->standby>2 || m->busy>1 || m->success>1 || !m->coordinator_pid || !m->coordinator_start ||
        m->issued_ms>now || now-m->issued_ms>(m->kind<=GS_POWER_LONG?GS_POWER_LEASE_MS:GS_POWER_RECEIPT_MS) ||
        s->pid<=0 || !s->process_start ||
        s->channel!=-1 || !hex(s->session) || !text(s->sd,GS_PATH) || !text(s->app,GS_PATH) ||
        !text(s->library,GS_PATH) || !text(s->append,GS_PATH) || !text(s->store_name,sizeof(s->store_name)) ||
        !text(s->identity.rom,GS_PATH) || !text(s->identity.core,GS_PATH) ||
        !text(s->identity.launcher,GS_PATH) || !text(s->identity.executable,GS_PATH)) return -1;
    if (m->kind==GS_POWER_LEASE) return m->transaction[0]? -1:0;
    return hex(m->transaction)?0:-1;
}
int gs_power_receive(int fd,struct gs_power_message *m) {
    ssize_t n=recv(fd,m,sizeof(*m),MSG_DONTWAIT|MSG_TRUNC);
    if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
    // A peer can send while the caller audits launch identity. Comparing that
    // packet with the caller's earlier scan time incorrectly rejects the live
    // peer as sending from the future and loses its pending transaction.
    return n==sizeof(*m) && !gs_power_wire_valid(m,gs_monotonic_ms())?1:-1;
}
static int mapped(pid_t pid,const char *expected) {
    char path[64],line[8192]; snprintf(path,sizeof(path),"/proc/%ld/maps",(long)pid);
    FILE *f=fopen(path,"r"); if (!f) return -1;
    int found=0;
    while (fgets(line,sizeof(line),f)) {
        char *name=strchr(line,'/'); if (!name) continue;
        name[strcspn(name,"\r\n")]=0;
        if (!strcmp(name,expected)) { found=1; break; }
    }
    fclose(f); return found?0:-1;
}
int gs_power_lease_valid(const struct gs_power_message *m) {
    if (gs_power_wire_valid(m,gs_monotonic_ms())) return -1;
    const struct gs_session *s=&m->session; struct gs_identity validated;
    uint64_t start;
    if (!gs_store_name_valid(s->store_name) || gs_process_start((pid_t)m->coordinator_pid,&start) ||
        start!=m->coordinator_start || gs_identity_make(s->sd,s->identity.rom,s->identity.launcher,
        s->identity.core,s->identity.executable,&validated) ||
        strcmp(validated.rom,s->identity.rom) || strcmp(validated.launcher,s->identity.launcher) ||
        strcmp(validated.core,s->identity.core) || strcmp(validated.executable,s->identity.executable) ||
        gs_process_matches(s->pid,s->process_start,&s->identity,s->append) ||
        mapped(s->pid,s->library) || mapped(s->pid,s->identity.core)) return -1;
    char proc[64],exe[GS_PATH],expected[GS_PATH];
    const char *slash=strrchr(s->library,'/');
    if (!slash || strcmp(slash+1,"libtmenu.so")) return -1;
    int n=snprintf(expected,sizeof(expected),"%.*s/gs-session",(int)(slash-s->library),s->library);
    if (n<0 || n>=GS_PATH) return -1;
    snprintf(proc,sizeof(proc),"/proc/%u/exe",m->coordinator_pid);
    ssize_t length=readlink(proc,exe,sizeof(exe)-1); if (length<0) return -1; exe[length]=0;
    if (strcmp(exe,expected)) return -1;
    char *canonical=realpath(s->library,NULL);
    int ok=canonical && !strcmp(canonical,s->library); free(canonical);
    if (!ok) return -1;
    // A launch-private config is part of eligibility, never a global config.
    slash=strrchr(s->append,'/'); if (!slash || strcmp(slash+1,"session.cfg")) return -1;
    memcpy(expected,s->append,(size_t)(slash-s->append)); expected[slash-s->append]=0;
    if (private_directory(expected)) return -1;
    int fd=open(s->append,O_RDONLY|O_NOFOLLOW|O_CLOEXEC); if (fd<0) return -1;
    char config[512]; struct stat st; ssize_t count=read(fd,config,sizeof(config));
    ok=!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==geteuid() &&
        count==(ssize_t)strlen(gs_private_config) && !memcmp(config,gs_private_config,(size_t)count);
    close(fd); return ok?0:-1;
}
static int send_message(int fd,const struct gs_power_message *m) {
    return send(fd,m,sizeof(*m),MSG_DONTWAIT|MSG_NOSIGNAL)==sizeof(*m)?0:-1;
}
static void drop_peer(struct gs_power_server *s) {
    if (s->client>=0) close(s->client);
    s->client=-1; s->client_pid=0; s->client_start=0; s->sent_ms=0;
}
int gs_power_server_open(struct gs_power_server *s,const char *directory) {
    memset(s,0,sizeof(*s)); s->listener=s->client=-1;
    if (path(s->socket_path,directory)) return -1;
    if (mkdir(directory,0700) && errno!=EEXIST) return -1;
    if (private_directory(directory) || gs_process_start(getpid(),&s->process_start)) return -1;
    s->listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if (s->listener<0) return -1;
    struct sockaddr_un address={.sun_family=AF_UNIX}; strcpy(address.sun_path,s->socket_path);
    // An occupied endpoint is never replaced: another launch may still own it.
    if (bind(s->listener,(struct sockaddr *)&address,sizeof(address))) {
        close(s->listener); s->listener=-1; s->socket_path[0]=0; return -1;
    }
    if (chmod(s->socket_path,0600) || listen(s->listener,1)) { gs_power_server_close(s); return -1; }
    return 0;
}
int gs_power_server_poll(struct gs_power_server *s,const struct gs_session *session,
                         int eligible,int busy,int standby,struct gs_power_message *request,uint64_t now) {
    if (s->listener<0) return 0;
    if (!eligible) { drop_peer(s); return 0; }
    int incoming=accept4(s->listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);
    if (incoming>=0) {
        pid_t pid; uint64_t start;
        if (s->client>=0 || peer(incoming,&pid,&start)) close(incoming);
        else { s->client=incoming; s->client_pid=pid; s->client_start=start; }
    }
    if (s->client<0) return 0;
    uint64_t start;
    if (gs_process_start(s->client_pid,&start) || start!=s->client_start) { drop_peer(s); return -1; }
    struct gs_power_message lease={.magic=GS_POWER_MAGIC,.kind=GS_POWER_LEASE,.busy=!!busy,.standby=(uint32_t)standby,
        .issued_ms=now,.coordinator_start=s->process_start,.coordinator_pid=(uint32_t)getpid(),.session=*session};
    lease.session.channel=-1;
    // Eligibility is checked again by inputd, independently of the coordinator.
    if (gs_power_wire_valid(&lease,now) || gs_power_lease_valid(&lease)) { drop_peer(s); return -1; }
    if (!s->sent_ms || now-s->sent_ms>=100) {
        lease.issued_ms=gs_monotonic_ms();
        if (send_message(s->client,&lease)) { drop_peer(s); return -1; }
        s->sent_ms=lease.issued_ms;
    }
    int received=gs_power_receive(s->client,request);
    if (!received) return 0;
    if (received<0 ||
        (request->kind!=GS_POWER_SHORT && request->kind!=GS_POWER_LONG &&
         !(request->kind==GS_POWER_YIELDED && request->standby==1) &&
         !(request->kind==GS_POWER_RECLAIMED && request->standby==2)) ||
        request->coordinator_pid!=(uint32_t)getpid() || request->coordinator_start!=s->process_start ||
        memcmp(&request->session,&lease.session,sizeof(lease.session))) { drop_peer(s); return -1; }
    return 1;
}
int gs_power_server_result(struct gs_power_server *s,const struct gs_power_message *request,int success,uint64_t now) {
    if (s->client<0) return -1;
    (void)now;
    struct gs_power_message result=*request;
    result.kind=GS_POWER_RESULT; result.success=!!success; result.issued_ms=gs_monotonic_ms();
    return send_message(s->client,&result);
}
void gs_power_server_close(struct gs_power_server *s) {
    drop_peer(s);
    if (s->listener>=0) { close(s->listener); if (*s->socket_path) (void)unlink(s->socket_path); }
    s->listener=-1; s->socket_path[0]=0;
}
int gs_power_client_init(struct gs_power_client *c,const char *directory) {
    memset(c,0,sizeof(*c)); c->fd=-1;
    return path(c->path,directory);
}
void gs_power_client_close(struct gs_power_client *c) {
    if (c->fd>=0) close(c->fd);
    c->fd=-1; c->valid=0; c->pending=0; c->sent_ms=0;
}
int gs_power_client_poll(struct gs_power_client *c,uint64_t now) {
    if (c->fd<0) {
        char directory[108]; strcpy(directory,c->path); char *slash=strrchr(directory,'/');
        if (!slash) return 0;
        *slash=0;
        struct stat st;
        if (private_directory(directory) || lstat(c->path,&st) || !S_ISSOCK(st.st_mode) ||
            st.st_uid!=geteuid() || (st.st_mode&0777)!=0600) return 0;
        c->fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0); if (c->fd<0) return 0;
        struct sockaddr_un address={.sun_family=AF_UNIX}; strcpy(address.sun_path,c->path);
        if (connect(c->fd,(struct sockaddr *)&address,sizeof(address)) ||
            peer(c->fd,&c->coordinator_pid,&c->coordinator_start)) { gs_power_client_close(c); return 0; }
    }
    struct gs_power_message m;
    for (int i=0;i<16;++i) {
        int received=gs_power_receive(c->fd,&m);
        if (!received) break;
        if (received<0 || m.coordinator_pid!=(uint32_t)c->coordinator_pid ||
            m.coordinator_start!=c->coordinator_start || gs_power_lease_valid(&m)) { gs_power_client_close(c); return 0; }
        if (m.kind==GS_POWER_LEASE) {
            if (c->pending && memcmp(&m.session,&c->request.session,sizeof(m.session))) { gs_power_client_close(c); return 0; }
            c->lease=m; c->valid=1;
        } else if (m.kind==GS_POWER_RESULT && !strcmp(m.transaction,c->request.transaction) &&
                   !memcmp(&m.session,&c->request.session,sizeof(m.session))) c->pending=0;
        else { gs_power_client_close(c); return 0; }
    }
    // Identity is checked independently every poll, including peer PID/start
    // and the full game launch. The bounded lease covers durable SD flushes of
    // both checkpoint and ordinary time accounting without exposing POWER to
    // keymon between writes. Peer death/disconnection still revokes immediately.
    now=gs_monotonic_ms();
    if (c->valid && (gs_power_wire_valid(&c->lease,now) || gs_power_lease_valid(&c->lease))) {
        gs_power_client_close(c); return 0;
    }
    if (c->pending && !c->lease.standby && now-c->sent_ms>=200) {
        // Retransmission keeps the same transaction identity and launch binding.
        c->request.issued_ms=now;
        if (send_message(c->fd,&c->request)) { gs_power_client_close(c); return 0; }
        c->sent_ms=now;
    }
    return c->valid;
}
int gs_power_client_request(struct gs_power_client *c,enum gs_power_kind kind,const char *transaction,uint64_t now) {
    now=gs_monotonic_ms();
    if (!c->valid || c->pending || c->lease.busy || c->lease.standby || gs_power_wire_valid(&c->lease,now) ||
        gs_power_lease_valid(&c->lease) || (kind!=GS_POWER_SHORT && kind!=GS_POWER_LONG) || !hex(transaction)) return -1;
    c->request=c->lease; c->request.kind=kind; c->request.issued_ms=gs_monotonic_ms();
    strcpy(c->request.transaction,transaction);
    if (send_message(c->fd,&c->request)) { gs_power_client_close(c); return -1; }
    c->pending=1; c->sent_ms=now; return 0;
}
static int ownership(struct gs_power_client *c,uint64_t now,enum gs_power_kind kind,uint32_t phase) {
    now=gs_monotonic_ms();
    if (!c->valid || !c->pending || c->lease.standby!=phase || now-c->sent_ms<200) return 0;
    if (gs_power_wire_valid(&c->lease,now) || gs_power_lease_valid(&c->lease)) return -1;
    struct gs_power_message m=c->lease; m.kind=(uint32_t)kind; m.issued_ms=gs_monotonic_ms();
    strcpy(m.transaction,c->request.transaction);
    if (send_message(c->fd,&m)) { gs_power_client_close(c); return -1; }
    c->sent_ms=now; return 0;
}
int gs_power_client_yielded(struct gs_power_client *c,uint64_t now) { return ownership(c,now,GS_POWER_YIELDED,1); }
int gs_power_client_reclaimed(struct gs_power_client *c,uint64_t now) { return ownership(c,now,GS_POWER_RECLAIMED,2); }

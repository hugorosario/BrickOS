#define _GNU_SOURCE
#include "channel.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int gs_channel_parent(int fd) {
    int type=0; socklen_t size=sizeof(type); struct ucred peer; socklen_t n=sizeof(peer);
    return fd>=0 && !getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size) && type==SOCK_SEQPACKET &&
           !getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&n) && peer.uid==geteuid() && peer.pid==getppid() ? 0 : -1;
}
int gs_channel_send(int fd,enum gs_choice choice,const struct gs_entry *entry) {
    struct gs_choice_message m={.magic=GS_CHANNEL_MAGIC,.action=(uint32_t)choice};
    if (choice==GS_CHOICE_RESUME) {
        if (!entry) { errno=EINVAL; return -1; }
        memcpy(m.id,entry->id,GS_ID); memcpy(m.state,entry->state,GS_ASSET);
    } else if (choice!=GS_CHOICE_BACK) { errno=EINVAL; return -1; }
    return send(fd,&m,sizeof(m),MSG_NOSIGNAL)==sizeof(m)?0:-1;
}
static int hex_id(const char *id) {
    if (id[32]) return 0;
    for (int i=0;i<32;++i) if (!strchr("0123456789abcdef",id[i]) || !id[i]) return 0;
    return 1;
}
int gs_channel_read(int fd,struct gs_choice_message *m) {
    ssize_t n=recv(fd,m,sizeof(*m),MSG_DONTWAIT|MSG_TRUNC);
    if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
    if (n!=sizeof(*m) || m->magic!=GS_CHANNEL_MAGIC) return -1;
    if (m->action==GS_CHOICE_BACK) return *m->id || *m->state ? -1 : 1;
    if (m->action!=GS_CHOICE_RESUME || !hex_id(m->id) ||
        strnlen(m->state,GS_ASSET)!=38 || strcmp(m->state+32,".state")) return -1;
    char id[GS_ID]; memcpy(id,m->state,32); id[32]=0;
    return hex_id(id)?1:-1;
}

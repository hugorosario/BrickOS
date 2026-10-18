#define _GNU_SOURCE
#include "gesture.h"
#include "session.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#define LONGS ((KEY_MAX+8*sizeof(unsigned long))/(8*sizeof(unsigned long)))
#define HAS(bits,key) (!!((bits)[(key)/(8*sizeof(unsigned long))] & (1UL<<((key)%(8*sizeof(unsigned long))))))
static struct gs_gesture gesture;
static int input=-1;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int device_ioctl(int fd,unsigned long request,void *argument) { return ioctl(fd,request,argument); }
const char *gs_gesture_directory="/dev/input";
int (*gs_gesture_ioctl)(int,unsigned long,void *)=device_ioctl;
// TRIMUI Player1 as both input daemons publish it: the name, the stock identity and the keys read.
static int controller(int fd) {
    char name[128]={0}; struct input_id id; unsigned long keys[LONGS]={0};
    return gs_gesture_ioctl(fd,EVIOCGNAME(sizeof(name)-1),name)>=0 && !strcmp(name,"TRIMUI Player1") &&
        gs_gesture_ioctl(fd,EVIOCGID,&id)>=0 && id.bustype==BUS_USB && id.vendor==0x045e && id.product==0x028e &&
        gs_gesture_ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(keys)),keys)>=0 &&
        HAS(keys,BTN_MODE) && HAS(keys,BTN_SELECT) && HAS(keys,BTN_START);
}
static int resync(int fd) {
    unsigned long bits[LONGS]={0};
    if (gs_gesture_ioctl(fd,EVIOCGKEY(sizeof(bits)),bits)<0) return -1;
    pthread_mutex_lock(&lock);
    gs_gesture_resync(&gesture,HAS(bits,BTN_MODE),HAS(bits,BTN_SELECT),HAS(bits,BTN_START));
    pthread_mutex_unlock(&lock); return 0;
}
// The one controller among the event nodes, with monotonic event times and its held keys read;
// -1 while there is none, or more than one, as during a change of input daemon.
static int attach(void) {
    int found=-1;
    for (int i=0;i<32;++i) {
        char path[256];
        if (snprintf(path,sizeof(path),"%s/event%d",gs_gesture_directory,i)>=(int)sizeof(path)) return -1;
        int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC); if (fd<0) continue;
        if (!controller(fd)) { close(fd); continue; }
        if (found>=0) { close(fd); close(found); return -1; }
        found=fd;
    }
    int clock=CLOCK_MONOTONIC;
    if (found>=0 && (gs_gesture_ioctl(found,EVIOCSCLOCKID,&clock)<0 || resync(found))) { close(found); found=-1; }
    return found;
}
// The controller went: the gesture in progress is cancelled, and the reader looks for it again.
static void detach(void) {
    fprintf(stderr,"GameSwitcher gesture: TRIMUI Player1 removed; looking for it again\n");
    pthread_mutex_lock(&lock); gs_gesture_lose(&gesture); pthread_mutex_unlock(&lock);
    close(input); input=-1;
}
static void *reader(void *unused) {
    (void)unused;
    for (;;) {
        if (input<0) {
            struct timespec wait={0,100000000}; nanosleep(&wait,NULL);
            if ((input=attach())<0) continue;
            fprintf(stderr,"GameSwitcher gesture: TRIMUI Player1 found again\n");
        }
        struct pollfd p={.fd=input,.events=POLLIN};
        int ready=poll(&p,1,100);
        if (ready<0 && errno==EINTR) continue;
        if (!ready) continue;
        // A removed node still hands over what it queued, then fails its reads.
        struct input_event e; ssize_t n=-1;
        if (ready>0) {
            n=read(input,&e,sizeof(e));
            if (n<0 && (errno==EAGAIN || errno==EINTR) && !(p.revents&(POLLHUP|POLLERR|POLLNVAL))) continue;
        }
        if (n!=sizeof(e)) { detach(); continue; }
        uint64_t at=(uint64_t)e.time.tv_sec*1000+(uint64_t)e.time.tv_usec/1000;
        uint64_t now=gs_monotonic_ms(); if (!at || at>now) at=now;
        pthread_mutex_lock(&lock); gs_gesture_event(&gesture,&e,at); int lost=gesture.lost; pthread_mutex_unlock(&lock);
        // After dropped input the held keys are read again at the report that ends it, as evdev asks.
        if (lost && e.type==EV_SYN && e.code==SYN_REPORT && resync(input)) detach();
    }
    return NULL;
}
int gs_gesture_start(void) {
    if ((input=attach())<0) return -1;
    pthread_t thread;
    if (pthread_create(&thread,NULL,reader,NULL)) { close(input); input=-1; return -1; }
    pthread_detach(thread); return 0;
}
// A queued request enters ShowMenu through one injected GUIDE, so only its release matters:
// skip the 280 ms window that tells GUIDE, double GUIDE and the chords apart.
void gs_gesture_release(void) {
    uint64_t start=gs_monotonic_ms();
    for (;;) {
        pthread_mutex_lock(&lock);
        int seen=gesture.recording && !gesture.fault,held=gesture.guide||gesture.select||gesture.start;
        uint64_t elapsed=gs_monotonic_ms()-start;
        // The press may still be in the reader's queue; give it 300 ms to arrive. Without the
        // controller there is no release to wait for.
        if (gesture.lost || (seen && !held) || (!seen && elapsed>=300) || elapsed>=1600) {
            gesture.recording=0; pthread_mutex_unlock(&lock); return;
        }
        pthread_mutex_unlock(&lock);
        struct timespec wait={0,10000000}; nanosleep(&wait,NULL);
    }
}
enum gs_route gs_gesture_finish(void) {
    uint64_t until=gs_monotonic_ms()+1600;
    enum gs_route selected=GS_ROUTE_WAIT;
    while (selected==GS_ROUTE_WAIT && gs_monotonic_ms()<until) {
        pthread_mutex_lock(&lock);
        selected=gs_gesture_route(&gesture,gs_monotonic_ms());
        if (selected!=GS_ROUTE_WAIT) gesture.recording=0;
        pthread_mutex_unlock(&lock);
        if (selected==GS_ROUTE_WAIT) { struct timespec wait={0,10000000}; nanosleep(&wait,NULL); }
    }
    fprintf(stderr,"GameSwitcher gesture route=%d\n",selected);
    return selected==GS_ROUTE_WAIT?GS_ROUTE_CANCEL:selected;
}

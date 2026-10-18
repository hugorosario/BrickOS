#define _GNU_SOURCE
#include "platform.h"
#include "relay.h"
#include "../mainui/settings.h"
#include "../osdd/display.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t clock_ms(void) {
    struct timespec ts; if (clock_gettime(CLOCK_MONOTONIC,&ts)) return 0;
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}
size_t gs_platform_slot(int slot) { return slot ? OSDD_PANEL_SLOT0_OFFSET : OSDD_PANEL_SLOT1_OFFSET; }

static int layer_ioctl(int fd,unsigned long request,unsigned char bytes[256]) {
    unsigned long args[3]={0,(unsigned long)bytes,1}; return ioctl(fd,request,args);
}
int (*gs_platform_layer)(int,unsigned long,unsigned char[256])=layer_ioctl;
static int layer(int fd,unsigned long request,unsigned char bytes[256]) { return gs_platform_layer(fd,request,bytes); }
const char *gs_platform_marker=OSDD_GAMESWITCHER_DISPLAY_MARKER;
// Written under a temporary name and renamed, so the OSD never reads half a pid.
static void marker_write(void) {
    char temporary[300],text[32];
    if (snprintf(temporary,sizeof(temporary),"%s.%d",gs_platform_marker,(int)getpid())>=(int)sizeof(temporary)) return;
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644); if (fd<0) return;
    int n=snprintf(text,sizeof(text),"%d\n",(int)getpid());
    int ok=write(fd,text,(size_t)n)==n; if (close(fd)) ok=0;
    if (!ok || rename(temporary,gs_platform_marker)) unlink(temporary);
}
// Only this process's marker is removed.
static void marker_remove(void) {
    char text[32]={0}; int fd=open(gs_platform_marker,O_RDONLY|O_CLOEXEC|O_NOFOLLOW); if (fd<0) return;
    ssize_t n=read(fd,text,sizeof(text)-1); close(fd);
    if (n>0 && atoi(text)==(int)getpid()) unlink(gs_platform_marker);
}
static int controller(void) {
    for (int i=0;i<32;++i) {
        char path[64],name[128]={0}; snprintf(path,sizeof(path),"/dev/input/event%d",i);
        int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
        if (fd<0) continue;
        if (ioctl(fd,EVIOCGNAME(sizeof(name)),name)>=0 && !strcmp(name,"TRIMUI Player1") &&
            ioctl(fd,EVIOCGRAB,1)==0) return fd;
        close(fd);
    }
    return -1;
}
// One volume key to keymon through the relay; a failed write is logged and dropped.
static void relay_key(struct gs_platform *p,unsigned short code,int value) {
    unsigned bit=code==KEY_VOLUMEUP?GS_RELAY_UP:GS_RELAY_DOWN;
    if (p->relay<0) return;
    if (gs_relay_key(p->relay,code,value)) { fprintf(stderr,"GameSwitcher: volume relay write: %s\n",strerror(errno)); return; }
    if (value) p->relay_down|=bit; else p->relay_down&=~bit;
}
void gs_platform_block(struct gs_platform *p,struct gs_model *m,unsigned char held[KEY_MAX+1],int x,int y) {
    relay_key(p,KEY_VOLUMEUP,held[KEY_VOLUMEUP]); relay_key(p,KEY_VOLUMEDOWN,held[KEY_VOLUMEDOWN]);
    held[KEY_VOLUMEUP]=held[KEY_VOLUMEDOWN]=0;
    gs_model_block(m,held,x,y);
}
static int block_input(struct gs_platform *p,struct gs_model *m) {
    unsigned long bits[(KEY_MAX+8*sizeof(unsigned long))/(8*sizeof(unsigned long))]={0};
    struct input_absinfo x={0},y={0}; unsigned char held[KEY_MAX+1]={0};
    if (ioctl(p->input,EVIOCGKEY(sizeof(bits)),bits)<0 || ioctl(p->input,EVIOCGABS(ABS_HAT0X),&x)<0 ||
        ioctl(p->input,EVIOCGABS(ABS_HAT0Y),&y)<0) return -1;
    for (int i=0;i<=KEY_MAX;++i) held[i]=!!(bits[i/(8*sizeof(unsigned long))] & (1UL<<(i%(8*sizeof(unsigned long)))));
    gs_platform_block(p,m,held,x.value,y.value); return 0;
}
int gs_platform_open(struct gs_platform *p,struct gs_model *m) {
    memset(p,0,sizeof(*p)); p->fb=p->disp=p->input=p->relay=-1;
    p->fb=open("/dev/fb0",O_RDWR|O_CLOEXEC); p->disp=open("/dev/disp",O_RDWR|O_CLOEXEC);
    struct fb_var_screeninfo var; struct fb_fix_screeninfo fix;
    if (p->fb<0 || p->disp<0 || ioctl(p->fb,FBIOGET_VSCREENINFO,&var)<0 ||
        ioctl(p->fb,FBIOGET_FSCREENINFO,&fix)<0 || var.xres!=1024 || var.yres!=768 ||
        var.bits_per_pixel!=32 || fix.line_length!=4096 ||
        fix.smem_len<OSDD_PANEL_SLOT1_OFFSET+OSDD_FRAME_BYTES || !fix.smem_start) goto fail;
    uint32_t channel=2; memcpy(p->saved+0xac,&channel,4);
    if (layer(p->disp,0x48,p->saved)<0 || p->saved[0x06]!=0) goto fail;
    p->map_bytes=fix.smem_len;
    p->mapping=mmap(NULL,p->map_bytes,PROT_READ|PROT_WRITE,MAP_SHARED,p->fb,0);
    if (p->mapping==MAP_FAILED) { p->mapping=NULL; goto fail; }
    for (int slot=0;slot<2;++slot) {
        p->backup[slot]=malloc(OSDD_FRAME_BYTES);
        if (!p->backup[slot]) goto fail;
        memcpy(p->backup[slot],p->mapping+gs_platform_slot(slot),OSDD_FRAME_BYTES);
    }
    p->screen=SDL_CreateRGBSurfaceWithFormat(0,1024,768,32,SDL_PIXELFORMAT_ARGB8888);
    if (!p->screen) goto fail;
    p->input=controller(); if (p->input<0) goto fail;
    // The grab holds the volume keys too: they go on to keymon through the relay.
    p->relay=gs_relay_open();
    if (p->relay<0) fprintf(stderr,"GameSwitcher: no volume relay; volume keys stay with GameSwitcher\n");
    if (block_input(p,m)) goto fail;
    p->base=(uint64_t)fix.smem_start; p->front=0; p->address=p->base+gs_platform_slot(0); p->alpha=255;
    osdd_display_layer_config_channel(p->shown,2,p->address,255);
    gs_platform_attach_keymon(p);
    gs_platform_status(m); return 0;
fail:
    (void)gs_platform_close(p); return -1;
}
int gs_platform_present(struct gs_platform *p) {
    if (!p->screen || p->screen->pitch!=4096) return -1;
    if (!p->visible) {
        // The layer is hidden: the first frame goes into slot 0 and the layer is shown on it,
        // once the marker tells the OSD that channel 2 is taken.
        marker_write();
        memcpy(p->mapping+gs_platform_slot(p->front),p->screen->pixels,OSDD_FRAME_BYTES);
        // Even a failed SET may have changed the driver: cleanup must restore it.
        p->visible=1;
        if (layer(p->disp,0x47,p->shown)<0) return -1;
        unsigned char verify[256]={0}; uint32_t channel=2; memcpy(verify+0xac,&channel,4);
        if (layer(p->disp,0x48,verify)<0 || memcmp(verify,p->shown,256)) return -1;
        return 0;
    }
    // Later frames go into the slot not shown, once the display has let go of it, and the layer
    // then switches to that slot at the next refresh.
    int back=1-p->front;
    for (uint64_t now=clock_ms(); now<p->switched_ms+GS_SLOT_RETIRE_MS; now=clock_ms()) {
        uint64_t left=p->switched_ms+GS_SLOT_RETIRE_MS-now;
        struct timespec wait={0,(long)left*1000000L}; nanosleep(&wait,NULL);
    }
    memcpy(p->mapping+gs_platform_slot(back),p->screen->pixels,OSDD_FRAME_BYTES);
    uint64_t address=p->base+gs_platform_slot(back);
    osdd_display_layer_config_channel(p->shown,2,address,p->alpha);
    if (layer(p->disp,0x47,p->shown)<0) return -1;
    p->front=back; p->address=address; p->switched_ms=clock_ms();
    return 0;
}
int gs_platform_alpha(struct gs_platform *p,unsigned char alpha) {
    if (alpha==p->alpha) return 0;
    p->alpha=alpha; osdd_display_layer_config_channel(p->shown,2,p->address,alpha);
    // As the OSD's panel fades: the global alpha changes without staging another frame.
    return p->visible && layer(p->disp,0x47,p->shown)<0 ? -1 : 0;
}
int gs_platform_input(struct gs_platform *p,struct input_event *e,int timeout_ms) {
    for (int wait=timeout_ms>0?timeout_ms:0;;wait=0) {
        struct pollfd fd={.fd=p->input,.events=POLLIN};
        int n=poll(&fd,1,wait);
        if (n<0) return errno==EINTR?0:-1;
        if (!n) return 0;
        if (fd.revents&(POLLHUP|POLLERR|POLLNVAL)) return -1;
        ssize_t bytes=read(p->input,e,sizeof(*e));
        if (bytes<0 && (errno==EAGAIN || errno==EINTR)) return 0;
        if (bytes!=(ssize_t)sizeof(*e)) return -1;
        if (e->type!=EV_KEY || (e->code!=KEY_VOLUMEUP && e->code!=KEY_VOLUMEDOWN)) return 1;
        // keymon repeats a held key by itself, so a repeat (2) is not passed on.
        if (e->value==0 || e->value==1) relay_key(p,e->code,e->value);
    }
}
// keymon's shared memory, which it creates at start: 23 integers, the brightness level at index 1.
static int *keymon_attach(void) {
    int id=shmget(0x4e4f4d4b,23*sizeof(int),0); if (id<0) return NULL;
    void *memory=shmat(id,NULL,0); return memory==(void *)-1?NULL:memory;
}
static void keymon_detach(int *memory) { shmdt(memory); }
// keymon's backlight for levels 0 to 10, level*172/10+4: MainUI's mainui_lcd_brightness.
static const int lcd_levels[11]={4,21,38,55,72,90,107,124,141,158,176};
static int lcd(int disp,int value) {
    unsigned long args[4]={0,(unsigned long)value,0,0}; return ioctl(disp,0x102,args)<0?-1:0;
}
static int toast(int level) {
    char text[8]; snprintf(text,sizeof(text),"%d",level);
    pid_t pid=fork();
    if (!pid) { execl("/bin/sh","sh","/usr/trimui/osd/show_backlight_msg.sh",text,(char *)NULL); _exit(127); }
    return pid<0?-1:(int)pid;
}
// keymon does not step the brightness while the screen is off for standby: the marker exists or
// the live backlight reads 0. A backlight that cannot be read counts as on.
static int screen_on(void) {
    struct stat st; if (!lstat("/tmp/system_suspend",&st)) return 0;
    int fd=open("/sys/devices/virtual/disp/disp/attr/sys",O_RDONLY|O_CLOEXEC); if (fd<0) return 1;
    char text[8192]; ssize_t n=read(fd,text,sizeof(text)-1); close(fd); if (n<=0) return 1; text[n]=0;
    char *p=strstr(text,"backlight("); return !p || atoi(p+10)>0;
}
int *(*gs_platform_keymon_attach)(void)=keymon_attach;
void (*gs_platform_keymon_detach)(int *)=keymon_detach;
int (*gs_platform_lcd)(int,int)=lcd;
int (*gs_platform_toast)(int)=toast;
int (*gs_platform_screen_on)(void)=screen_on;
const char *gs_platform_settings="/mnt/UDISK/system.json";
void gs_platform_attach_keymon(struct gs_platform *p) {
    p->keymon=gs_platform_keymon_attach();
    if (!p->keymon) fprintf(stderr,"GameSwitcher: no keymon memory; brightness chord unavailable\n");
}
void gs_platform_brightness(struct gs_platform *p,int step) {
    if (!p->keymon || !gs_platform_screen_on()) return;
    int level=p->keymon[1]<0?0:p->keymon[1]>10?10:p->keymon[1], next=level+step;
    // keymon's order: the level, the screen and the toast, then the file.
    if (next<0 || next>10) { if (gs_platform_toast(level)<0) fprintf(stderr,"GameSwitcher: brightness toast failed\n"); return; }
    p->keymon[1]=next;
    if (gs_platform_lcd(p->disp,lcd_levels[next])) fprintf(stderr,"GameSwitcher: brightness %d: %s\n",next,strerror(errno));
    if (gs_platform_toast(next)<0) fprintf(stderr,"GameSwitcher: brightness toast failed\n");
    struct mainui_settings *settings=malloc(sizeof(*settings));
    if (!settings) { fprintf(stderr,"GameSwitcher: brightness not saved\n"); return; }
    mainui_settings_load(settings,gs_platform_settings);
    if (mainui_settings_set(settings,MAINUI_SET_BRIGHTNESS,next) || mainui_settings_save(settings,gs_platform_settings))
        fprintf(stderr,"GameSwitcher: brightness not saved in %s\n",gs_platform_settings);
    free(settings);
    fprintf(stderr,"GameSwitcher brightness level=%d\n",next);
}
void gs_platform_reap(void) { while (waitpid(-1,NULL,WNOHANG)>0) {} }
void gs_platform_status(struct gs_model *m) {
    FILE *file=fopen("/sys/class/power_supply/axp2202-battery/capacity","r");
    int battery=-1;
    if (file) { if (fscanf(file,"%d",&battery)!=1) battery=-1; fclose(file); }
    m->battery=battery>=0 && battery<=100?battery:-1;
}
int gs_platform_close(struct gs_platform *p) {
    int result=0;
    if (p->visible && layer(p->disp,0x47,p->saved)<0) result=-1;
    for (int slot=0;slot<2;++slot)
        if (p->mapping && p->backup[slot]) memcpy(p->mapping+gs_platform_slot(slot),p->backup[slot],OSDD_FRAME_BYTES);
    if (p->visible) {
        unsigned char verify[256]={0}; uint32_t channel=2; memcpy(verify+0xac,&channel,4);
        if (layer(p->disp,0x48,verify)<0 || memcmp(verify,p->saved,256)) result=-1;
        marker_remove();
    }
    // A volume key still pressed on the relay is released before the grab ends, so keymon stops
    // repeating it; the player's later release on the controller then changes nothing.
    if (p->relay_down&GS_RELAY_UP) relay_key(p,KEY_VOLUMEUP,0);
    if (p->relay_down&GS_RELAY_DOWN) relay_key(p,KEY_VOLUMEDOWN,0);
    if (p->relay>=0) close(p->relay);
    if (p->input>=0) { if (ioctl(p->input,EVIOCGRAB,0)<0) result=-1; close(p->input); }
    SDL_FreeSurface(p->screen); free(p->backup[0]); free(p->backup[1]);
    if (p->mapping) munmap(p->mapping,p->map_bytes);
    if (p->keymon) gs_platform_keymon_detach(p->keymon);
    if (p->fb>=0) close(p->fb);
    if (p->disp>=0) close(p->disp);
    memset(p,0,sizeof(*p)); p->fb=p->disp=p->input=p->relay=-1;
    return result;
}

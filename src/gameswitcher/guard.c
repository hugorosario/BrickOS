#define _GNU_SOURCE
#include "guard.h"
#include "relay.h"
#include "../osdd/display.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
static int layer(int fd,unsigned long op,unsigned char *config) {
    unsigned long args[3]={0,(unsigned long)config,1}; return ioctl(fd,op,args);
}
int gs_guard_clear_stale_marker(const char *path) {
    struct stat st;
    if (lstat(path,&st) || osdd_display_gameswitcher_marker(path)) return 0;
    return unlink(path)==0;
}
int gs_guard_open(struct gs_guard *g) {
    memset(g,0,sizeof(*g)); g->fb=g->disp=-1;
    (void)gs_guard_clear_stale_marker(OSDD_GAMESWITCHER_DISPLAY_MARKER);
    g->fb=open("/dev/fb0",O_RDWR|O_CLOEXEC); g->disp=open("/dev/disp",O_RDWR|O_CLOEXEC);
    struct fb_fix_screeninfo fix; struct fb_var_screeninfo var;
    uint32_t channel=2; memcpy(g->layer+0xac,&channel,4);
    if (g->fb<0 || g->disp<0 || ioctl(g->fb,FBIOGET_FSCREENINFO,&fix)<0 ||
        ioctl(g->fb,FBIOGET_VSCREENINFO,&var)<0 || var.xres!=1024 || var.yres!=768 ||
        var.bits_per_pixel!=32 || fix.line_length!=4096 || fix.smem_len<OSDD_PANEL_SLOT1_OFFSET+OSDD_FRAME_BYTES ||
        layer(g->disp,0x48,g->layer)<0 || g->layer[0x06]!=0) goto fail;
    g->bytes=fix.smem_len;
    g->map=mmap(NULL,g->bytes,PROT_READ|PROT_WRITE,MAP_SHARED,g->fb,0);
    if (g->map==MAP_FAILED) { g->map=NULL; goto fail; }
    // GameSwitcher shows its panel from both channel-2 slots.
    g->backup=malloc(2*(size_t)OSDD_FRAME_BYTES); if (!g->backup) goto fail;
    memcpy(g->backup,g->map+OSDD_PANEL_SLOT1_OFFSET,OSDD_FRAME_BYTES);
    memcpy(g->backup+OSDD_FRAME_BYTES,g->map+OSDD_PANEL_SLOT0_OFFSET,OSDD_FRAME_BYTES); return 0;
fail:
    (void)gs_guard_close(g); return -1;
}
int gs_guard_close(struct gs_guard *g) {
    int result=0;
    if (g->backup && g->map) {
        if (layer(g->disp,0x47,g->layer)<0) result=-1;
        memcpy(g->map+OSDD_PANEL_SLOT1_OFFSET,g->backup,OSDD_FRAME_BYTES);
        memcpy(g->map+OSDD_PANEL_SLOT0_OFFSET,g->backup+OSDD_FRAME_BYTES,OSDD_FRAME_BYTES);
        unsigned char verify[256]={0}; uint32_t channel=2; memcpy(verify+0xac,&channel,4);
        if (layer(g->disp,0x48,verify)<0 || memcmp(g->layer,verify,256)) result=-1;
    }
    if (g->map) munmap(g->map,g->bytes);
    if (g->fb>=0) close(g->fb);
    if (g->disp>=0) close(g->disp);
    free(g->backup); memset(g,0,sizeof(*g)); g->fb=g->disp=-1;
    // An app that ended without its close leaves its marker; the OSD may use channel 2 again.
    (void)gs_guard_clear_stale_marker(OSDD_GAMESWITCHER_DISPLAY_MARKER);
    // It may also have left a volume key pressed on the relay, which keymon would repeat. The
    // kernel drops the release of a key that is not down there, so a key the player holds on the
    // controller is never touched. Silent without a relay node.
    gs_relay_release_node();
    return result;
}

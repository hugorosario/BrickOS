#define _GNU_SOURCE
#include "checkpoint.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static uint64_t now_ms(void) {
    struct timespec ts; if (clock_gettime(CLOCK_MONOTONIC,&ts)) return UINT64_MAX;
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}
// Half size, 24 bits: about 0.6 MB instead of 3 MB. The SD is mounted with sync, so the bytes
// written are most of a checkpoint's time. Half of 1024x768 still holds the native resolution
// of the supported systems, and the app scales the image to its card or full screen.
static SDL_Surface *half_size(SDL_Surface *background) {
    int w=background->w>1?background->w/2:1,h=background->h>1?background->h/2:1;
    SDL_Surface *half=SDL_CreateRGBSurfaceWithFormat(0,w,h,background->format->BitsPerPixel,background->format->format);
    if (!half) return NULL;
    SDL_BlendMode mode; int blended=!SDL_GetSurfaceBlendMode(background,&mode);
    if (blended) (void)SDL_SetSurfaceBlendMode(background,SDL_BLENDMODE_NONE);
    SDL_Surface *packed=SDL_BlitScaled(background,NULL,half,NULL)?NULL:SDL_ConvertSurfaceFormat(half,SDL_PIXELFORMAT_BGR24,0);
    if (blended) (void)SDL_SetSurfaceBlendMode(background,mode);
    SDL_FreeSurface(half); return packed;
}
// The BMP is built in memory and written with one write(): on the sync-mounted card each
// write() is synchronous, so stdio's small blocks cost far more than the bytes themselves.
static int thumbnail(struct gs_stage *stage,SDL_Surface *background) {
    SDL_Surface *image=half_size(background);
    if (!image) return -1;
    size_t capacity=138+(size_t)image->w*4*(size_t)image->h;
    unsigned char *bytes=malloc(capacity);
    SDL_RWops *rw=bytes?SDL_RWFromMem(bytes,(int)capacity):NULL;
    Sint64 size=rw && !SDL_SaveBMP_RW(image,rw,0)?SDL_RWtell(rw):-1;
    if (rw) SDL_RWclose(rw);
    SDL_FreeSurface(image);
    int fd=size>0?openat(stage->directory,stage->thumbnail,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600):-1;
    int result=fd<0?-1:0;
    for (Sint64 done=0; !result && done<size;) {
        ssize_t n=write(fd,bytes+done,(size_t)(size-done));
        if (n<0 && errno==EINTR) continue;
        if (n<=0) result=-1; else done+=n;
    }
    if (fd>=0 && (fsync(fd) || close(fd))) result=-1;
    free(bytes); return result;
}
static void remove_regular(struct gs_stage *stage,const char *name) {
    struct stat st;
    if (!fstatat(stage->directory,name,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode))
        (void)unlinkat(stage->directory,name,0);
}
void gs_checkpoint_discard(struct gs_stage *stage) {
    if (stage->directory>=0) {
        remove_regular(stage,stage->state); remove_regular(stage,stage->thumbnail);
        (void)fsync(stage->directory);
    }
    gs_stage_close(stage);
}
int gs_checkpoint_prepare(struct gs_store *s,const struct gs_identity *identity,
                          struct tmenu_host *host,SDL_Surface *background,unsigned timeout_ms,struct gs_stage *stage) {
    memset(stage,0,sizeof(*stage)); stage->directory=stage->lock=-1;
    if (!host || !host->save_state || !host->thumb_path || !background ||
        !background->pixels || !timeout_ms || timeout_ms>12000) { errno=EINVAL; return -1; }
    char *thumb=host->thumb_path(99);
    if (!thumb) return -1;
    size_t n=strlen(thumb); char state[GS_PATH];
    if (n<4 || n+3>=GS_PATH || strcmp(thumb+n-4,".png")) { free(thumb); errno=EINVAL; return -1; }
    memcpy(state,thumb,n-4); strcpy(state+n-4,".state");
    int result=gs_stage_begin(s,identity,state,thumb,stage); free(thumb);
    if (result) return -1;
    uint64_t started=now_ms(),until=started+timeout_ms;
    // Producer gets timeout_ms; keep a further bounded interval for the owner
    // to receive completion and durably publish the prepared files.
    // The host's constant callback return is not evidence of output completion.
    (void)host->save_state(99);
    int ready=0;
    while (now_ms()<until && now_ms()<stage->deadline_ms) {
        if (!gs_stage_state_ready(stage)) { ready=1; break; }
        struct timespec wait={0,20000000}; nanosleep(&wait,NULL);
    }
    result=-1;
    uint64_t state_done=now_ms();
    if (ready && now_ms()<until && !thumbnail(stage,background) && now_ms()<until) {
        fprintf(stderr,"GameSwitcher checkpoint state_ms=%llu thumbnail_ms=%llu\n",
                (unsigned long long)(state_done-started),(unsigned long long)(now_ms()-state_done));
        return 0;
    }
    // An unfinished asynchronous producer may still write: leave its occupied
    // staging slot for explicit recovery, instead of accepting it in a later save.
    if (ready) {
        gs_checkpoint_discard(stage);
    }
    gs_stage_close(stage); return result;
}
int gs_checkpoint_save(struct gs_store *s,const struct gs_identity *identity,
                       struct tmenu_host *host,SDL_Surface *background,unsigned timeout_ms) {
    struct gs_stage stage;
    if (gs_checkpoint_prepare(s,identity,host,background,timeout_ms,&stage)) return -1;
    struct gs_receipt receipt={.complete=1}; memcpy(receipt.transaction,stage.transaction,GS_ID);
    uint64_t started=now_ms();
    int result=gs_store_publish(s,identity,&stage,&receipt);
    fprintf(stderr,"GameSwitcher checkpoint publish_ms=%llu result=%d\n",(unsigned long long)(now_ms()-started),result);
    gs_checkpoint_discard(&stage); return result;
}

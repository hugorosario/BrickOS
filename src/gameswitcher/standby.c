#define _GNU_SOURCE
#include "standby.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
void gs_standby_init(struct gs_standby *s,uint64_t now) {
    memset(s,0,sizeof(*s)); s->phase=GS_STANDBY_YIELD; s->deadline=now+5000;
}
int gs_standby_yield(struct gs_standby *s,uint64_t now) {
    if (s->phase!=GS_STANDBY_YIELD || now>=s->deadline) return -1;
    s->phase=GS_STANDBY_ENTER; s->deadline=now+5000; return 0;
}
void gs_standby_observe(struct gs_standby *s,uint64_t now,int marker,int brightness) {
    if (s->phase==GS_STANDBY_ENTER && marker==1 && brightness==0) {
        s->phase=GS_STANDBY_ASLEEP; s->deadline=now+180000;
    } else if (s->phase==GS_STANDBY_ASLEEP && marker==0 && brightness>0) {
        s->phase=GS_STANDBY_AWAKE; s->deadline=now+5000;
    }
    else if ((s->phase==GS_STANDBY_YIELD || s->phase==GS_STANDBY_ENTER || s->phase==GS_STANDBY_ASLEEP) &&
             now>=s->deadline) s->phase=GS_STANDBY_FAILED;
}
int gs_standby_snapshot(int *marker,int *brightness) {
    struct stat st; int exists=lstat("/tmp/system_suspend",&st)==0;
    if (!exists && errno!=ENOENT) return -1;
    if (exists && !S_ISREG(st.st_mode)) return -1;
    int fd=open("/sys/devices/virtual/disp/disp/attr/sys",O_RDONLY|O_CLOEXEC); if (fd<0) return -1;
    char text[8192]; ssize_t n=read(fd,text,sizeof(text)-1); close(fd); if (n<=0) return -1; text[n]=0;
    char *p=strstr(text,"backlight("); if (!p) return -1;
    char *end; long value=strtol(p+10,&end,10);
    if (end==p+10 || *end!=')' || value<0 || value>255) return -1;
    *marker=exists; *brightness=(int)value; return 0;
}
static int keymon_exe(pid_t pid) {
    char path[64],exe[GS_PATH]; snprintf(path,sizeof(path),"/proc/%ld/exe",(long)pid);
    ssize_t n=readlink(path,exe,sizeof(exe)-1); if (n<0) return 0; exe[n]=0;
    return !strcmp(exe,"/usr/trimui/bin/keymon");
}
int gs_standby_keymon_alive(const struct gs_standby *s) {
    uint64_t start; return s->keymon>0 && keymon_exe(s->keymon) &&
        !gs_process_start(s->keymon,&start) && start==s->keymon_start;
}
static int holds(pid_t pid,const struct stat *device) {
    char root[64],path[128]; snprintf(root,sizeof(root),"/proc/%ld/fd",(long)pid);
    DIR *directory=opendir(root); if (!directory) return 0;
    struct dirent *entry; int found=0;
    while ((entry=readdir(directory))) {
        if (strspn(entry->d_name,"0123456789")!=strlen(entry->d_name)) continue;
        int n=snprintf(path,sizeof(path),"%s/%s",root,entry->d_name); struct stat st;
        if (n>0 && n<(int)sizeof(path) && !stat(path,&st) && S_ISCHR(st.st_mode) &&
            st.st_rdev==device->st_rdev && st.st_ino==device->st_ino) { found=1; break; }
    }
    closedir(directory); return found;
}
static int keymon(struct gs_standby *s,int device) {
    struct stat st; if (fstat(device,&st)) return -1;
    DIR *directory=opendir("/proc"); if (!directory) return -1;
    struct dirent *entry; pid_t found=0;
    while ((entry=readdir(directory))) {
        if (!*entry->d_name || strspn(entry->d_name,"0123456789")!=strlen(entry->d_name)) continue;
        pid_t pid=(pid_t)strtol(entry->d_name,NULL,10);
        if (!keymon_exe(pid)) continue;
        if (found || !holds(pid,&st)) { found=0; break; }
        found=pid;
    }
    closedir(directory); s->keymon=found;
    return found && !gs_process_start(found,&s->keymon_start)?0:-1;
}
static int emit(int fd,int value) {
    struct input_event events[2]={{.type=EV_KEY,.code=KEY_POWER,.value=value},{.type=EV_SYN,.code=SYN_REPORT}};
    return write(fd,events,sizeof(events))==sizeof(events)?0:-1;
}
int gs_standby_pulse(struct gs_standby *s,const struct gs_session *session) {
    if (s->phase!=GS_STANDBY_ENTER || gs_process_matches(session->pid,session->process_start,&session->identity,session->append)) return -1;
    int marker,brightness;
    if (gs_standby_snapshot(&marker,&brightness) || marker || brightness<=0) return -1;
    int fd=-1;
    for (int i=0;i<32;++i) {
        char path[64],name[128]={0}; snprintf(path,sizeof(path),"/dev/input/event%d",i);
        int candidate=open(path,O_RDWR|O_NONBLOCK|O_CLOEXEC); if (candidate<0) continue;
        if (ioctl(candidate,EVIOCGNAME(sizeof(name)),name)>=0 && !strcmp(name,"axp2202-pek")) { fd=candidate; break; }
        close(candidate);
    }
    if (fd<0) return -1;
    unsigned long bits[(KEY_MAX+8*sizeof(unsigned long))/(8*sizeof(unsigned long))]={0};
    int valid=!keymon(s,fd) && ioctl(fd,EVIOCGKEY(sizeof(bits)),bits)>=0 &&
        !(bits[KEY_POWER/(8*sizeof(unsigned long))] & (1UL<<(KEY_POWER%(8*sizeof(unsigned long))))) &&
        ioctl(fd,EVIOCGRAB,1)==0;
    // Test that inputd has yielded; immediately return the source to keymon.
    if (valid && ioctl(fd,EVIOCGRAB,0)<0) valid=0;
    int result=-1;
    if (valid && gs_standby_keymon_alive(s)) {
        int pressed=emit(fd,1);
        if (!pressed) {
            struct timespec duration={0,150000000};
            while (nanosleep(&duration,&duration)<0 && errno==EINTR) {}
        }
        int released=emit(fd,0); result=pressed || released?-1:0;
    }
    (void)ioctl(fd,EVIOCGRAB,0); close(fd); return result;
}

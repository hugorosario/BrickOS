#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "update.h"
#include "versions.h"
#include "../common/semver.h"
#include "../common/version.h"
BRICKOS_DECLARE_COMPONENT("updater", BRICKOS_VERSION_UPDATER);
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char state[UPDATE_PATH],sd[UPDATE_PATH],temporary[UPDATE_PATH];
static int file_path(char *out,const char *file) { return update_path(out,"%s/%s",state,file); }
static int put(const char *file,const char *text) {
    char path[UPDATE_PATH];return file_path(path,file) || update_write(path,text,strlen(text));
}
static int exists(const char *file) {
    char path[UPDATE_PATH];return !file_path(path,file) && update_regular(path);
}
static void get(const char *file,char *out,size_t size) {
    char path[UPDATE_PATH];size_t length;out[0]=0;
    if(file_path(path,file))return;
    unsigned char *bytes=update_read(path,size-1,&length);if(!bytes)return;
    memcpy(out,bytes,length);out[length]=0;free(bytes);out[strcspn(out,"\r\n")]=0;
}
static int remove_file(const char *file) {
    char path[UPDATE_PATH];if(file_path(path,file) || update_safe_path(path))return -1;
    return unlink(path) && errno!=ENOENT ? -1 : 0;
}
static void installed(char build[13]) {
    char path[UPDATE_PATH];size_t length;build[0]=0;
    if(update_path(path,"%s/BrickOS/enabled",sd))return;
    unsigned char *bytes=update_read(path,32,&length);if(!bytes)return;
    if(length==13 && bytes[12]=='\n'){bytes[12]=0;if(update_hex((char *)bytes,12))memcpy(build,bytes,13);}
    free(bytes);
}
static int set_state(const char *value) { return put("state",value); }
static int run_curl(const char *url,const char *output,uint64_t limit,int package) {
    char maximum[32];snprintf(maximum,sizeof(maximum),"%llu",(unsigned long long)limit);
    int response[2];if(pipe(response))return -1;
    pid_t pid=fork();if(pid<0){close(response[0]);close(response[1]);return -1;}
    if(pid==0) {
        close(response[0]);dup2(response[1],STDOUT_FILENO);close(response[1]);
        struct rlimit bounds={(rlim_t)limit,(rlim_t)limit};if(setrlimit(RLIMIT_FSIZE,&bounds))_exit(126);
        const char *argv[40];int n=0;
        const char *const common[]={"curl","--disable","--fail","--silent","--show-error","--location",
            "--proto","=https","--proto-redir","=https","--tlsv1.2","--connect-timeout","15",
            "--max-filesize",maximum,"--user-agent","BrickOS-Updater/1"};
        for(size_t i=0;i<sizeof(common)/sizeof(*common);++i)argv[n++]=common[i];
        // The firmware's curl has no default CA bundle and fails every verification; its own
        // bundle is named, so TLS verification stays on.
        static const char *const bundles[]={"/etc/ssl/certs/ca-certificates.crt","/etc/ssl/cert.pem"};
        for(size_t i=0;i<sizeof(bundles)/sizeof(*bundles);++i) {
            struct stat st;if(stat(bundles[i],&st) || !S_ISREG(st.st_mode))continue;
            argv[n++]="--cacert";argv[n++]=bundles[i];break;
        }
        argv[n++]="--max-time";argv[n++]=package?"1200":"30";
        // The API's headers go only to the API: the package download follows redirects to
        // GitHub's asset storage, which must not receive them.
        if(!package) {
            argv[n++]="--header";argv[n++]="Accept: application/vnd.github+json";
            argv[n++]="--header";argv[n++]="X-GitHub-Api-Version: 2022-11-28";
        }
        argv[n++]="--write-out";argv[n++]="%{http_code}";argv[n++]="--output";argv[n++]=output;argv[n++]=url;argv[n]=NULL;
        execvp("curl",(char *const *)argv);
        _exit(127);
    }
    close(response[1]);char code[16]={0};size_t at=0;
    while(at<sizeof(code)-1) {
        ssize_t n=read(response[0],code+at,sizeof(code)-1-at);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)break;
        at+=(size_t)n;
    }
    close(response[0]);int status;while(waitpid(pid,&status,0)<0){if(errno!=EINTR)return -1;}
    if(!package && WIFEXITED(status) && WEXITSTATUS(status)==22 && !strcmp(code,"404"))return 404;
    return WIFEXITED(status) && WEXITSTATUS(status)==0 ? 0 : update_error("HTTPS request failed; check Wi-Fi, curl and CA certificates");
}
static int lock_file(const char *file) {
    if(update_safe_path(file))return -1;
    int fd=open(file,O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd>=0 && flock(fd,LOCK_EX|LOCK_NB)){close(fd);return -1;}return fd;
}
static int operation_lock(void) {
    char path[UPDATE_PATH];if(file_path(path,"operation.lock"))return -1;
    int fd=lock_file(path);if(fd<0)update_error("Another update operation is running");return fd;
}
// A prepared update counts while the build it was prepared from is still the enabled one and its
// stage is there. A package installed by hand since then, or a stage gone, makes it stale: with
// DROP it is removed and the status says so.
static int pending_current(int drop) {
    char pending[32],from[32],current[13],path[UPDATE_PATH];
    get("pending",pending,sizeof(pending));if(!exists("pending"))return 0;
    get("pending-from",from,sizeof(from));installed(current);
    if(strlen(pending)==12 && update_hex(pending,12) && (!from[0] || !strcmp(from,current)) &&
        !update_path(path,"%s/packages/%s",state,pending) && access(path,F_OK)==0)return 1;
    if(drop) {
        remove_file("pending-from");remove_file("pending");
        set_state("Prepared update dropped: another build is installed");
    }
    return 0;
}
// The installed component metadata counts only when the enabled build's own package carries
// it: a package installed by hand without metadata leaves an older BrickOS/versions.json behind.
// PATH gets BrickOS/versions.json; returns 1 when it counts, 0 for a legacy install.
static int current_metadata(char *path) {
    char build[13],package[UPDATE_PATH];installed(build);
    if(!build[0] || update_path(path,"%s/BrickOS/versions.json",sd))return 0;
    if(!update_path(package,"%s/BrickOS/package/%s/versions.json",sd,build) && update_regular(package))return 1;
    return !update_path(package,"%s/packages/%s/BrickOS/package/%s/versions.json",state,build,build) && update_regular(package);
}
static int latest(struct update_release *release,int notify) {
    if(pending_current(1)){puts("An update is already prepared for the next boot.");return 0;}
    char path[UPDATE_PATH];if(file_path(path,"release.tmp-XXXXXX"))return -1;
    int fd=mkstemp(path);if(fd<0)return -1;close(fd);
    int result=run_curl(UPDATE_API,path,512*1024,0);
    if(result==404){unlink(path);set_state("No published stable release");puts("No published stable release.");return 0;}
    if(!result)result=update_parse_release(path,release);
    if(result){unlink(path);set_state("Check failed; retry when online");return -1;}
    char installed_metadata[UPDATE_PATH];struct update_versions current_versions;int versioned=0;
    struct stat metadata_stat;int counted=current_metadata(installed_metadata);
    if(counted && update_safe_path(installed_metadata)){unlink(path);return -1;}
    if(counted && !lstat(installed_metadata,&metadata_stat)) {
        if(!S_ISREG(metadata_stat.st_mode)){unlink(path);return update_error("Installed component metadata is not a regular file");}
        if(update_versions_load(installed_metadata,&current_versions)){unlink(path);return -1;}
        versioned=1;int order;
        if(brickos_semver_compare(release->version,current_versions.release,&order)){unlink(path);return -1;}
        if(order<=0) {
            unlink(path);char message[160];snprintf(message,sizeof(message),order<0?"Latest release is older than installed %s":"Up to date: %s",current_versions.release);
            set_state(message);puts(message);return 0;
        }
    }
    char id[32];snprintf(id,sizeof(id),"%llu",(unsigned long long)release->id);
    char previous_id[32];get("installed-asset",previous_id,sizeof(previous_id));
    char *end=NULL;unsigned long long previous=strtoull(previous_id,&end,10);
    if(!versioned && previous_id[0] && end && *end==0 && release->id<previous) {
        unlink(path);set_state("Latest release is older than the installed update");return 0;
    }
    size_t length;unsigned char *json=update_read(path,512*1024,&length);unlink(path);
    char target[UPDATE_PATH];if(!json || file_path(target,"release.json")){free(json);return -1;}
    result=update_write(target,json,length);free(json);if(result)return -1;
    char current[13];installed(current);
    int available=strcmp(current,release->build)!=0;
    char message[160];snprintf(message,sizeof(message),available?"Update available: %s":"Up to date: %s",release->tag);
    if(set_state(message))return -1;
    puts(message);
    if(notify && available) {
        char notified[32];get("notified-asset",notified,sizeof(notified));
        if(strcmp(id,notified)) {
            // The regular OSD toast file; no firmware code or configuration is changed.
            char toast[UPDATE_PATH];struct stat st;
            if(!update_path(toast,"%s/trimui_osd",temporary) && !update_safe_path(toast) && !stat(toast,&st) && S_ISDIR(st.st_mode) &&
                !update_path(toast,"%s/trimui_osd/osd_toast_msg",temporary)) {
                // Every field the OSD draws from: it starts a toast at zero duration and size,
                // so a record without them is never shown. MainUI's wide bottom box, as
                // GameSwitcher's notices use it.
                static const char message_json[]="{\"type\":\"default\",\"id\":\"com.brickos.update\",\"duration\":4000,"
                    "\"size\":2,\"x\":212,\"y\":550,\"w\":600,\"h\":80,\"message\":\"Update available in Settings > Updates\"}\n";
                if(!update_write(toast,message_json,sizeof(message_json)-1))put("notified-asset",id);
            }
        }
    }
    return available;
}
static int stage_paths(const char *build,char *directory,char *zip,char *metadata) {
    if(!update_hex(build,12))return update_error("Invalid build identity");
    if(file_path(directory,"packages") || update_path(directory,"%s/packages/%s",state,build))return -1;
    return update_path(zip,"%s/release.zip",directory) || update_path(metadata,"%s/release.json",directory);
}
static int verify(const char *build) {
    char directory[UPDATE_PATH],zip[UPDATE_PATH],metadata[UPDATE_PATH];struct update_release release;
    if(stage_paths(build,directory,zip,metadata) || update_parse_release(metadata,&release) || strcmp(build,release.build))return -1;
    if(update_stage(zip,directory,&release,1))return -1;
    char installed_metadata[UPDATE_PATH];
    return update_versions_stage(directory,current_metadata(installed_metadata)?installed_metadata:NULL,&release);
}
// Only used on reserved, updater-owned staging/package directories, never on user data.
static int remove_tree(const char *path) {
    if(update_safe_path(path))return -1;
    struct stat st;if(lstat(path,&st))return errno==ENOENT?0:-1;
    if(!S_ISDIR(st.st_mode))return unlink(path);
    DIR *dir=opendir(path);if(!dir)return -1;struct dirent *entry;int bad=0;
    while((entry=readdir(dir))) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        char child[UPDATE_PATH];if(update_path(child,"%s/%s",path,entry->d_name) || remove_tree(child)){bad=1;break;}
    }
    closedir(dir);return bad?-1:rmdir(path);
}
static int prepare(void) {
    struct update_release release;int result=latest(&release,0);if(result<=0)return result;
    printf("confirm:Download %s and install it at the next boot?\n",release.tag);fflush(stdout);
    char answer[16];if(!fgets(answer,sizeof(answer),stdin) || strcmp(answer,"A\n")){puts("Update cancelled.");return 0;}
    char directory[UPDATE_PATH],zip[UPDATE_PATH],metadata[UPDATE_PATH];
    if(stage_paths(release.build,directory,zip,metadata))return -1;
    if(access(directory,F_OK)==0 && update_stage(zip,directory,&release,1) && remove_tree(directory))return -1;
    if(access(directory,F_OK)!=0) {
        // Cancelled downloads may leave an incomplete private stage. The operation lock makes
        // these reserved names safe to reclaim before a new, confirmed download.
        DIR *dir=opendir(state);struct dirent *entry;
        if(dir) {
            while((entry=readdir(dir)))if(!strncmp(entry->d_name,".stage-",7)) {
                char abandoned[UPDATE_PATH];if(!file_path(abandoned,entry->d_name))remove_tree(abandoned);
            }
            closedir(dir);
        }
        struct statvfs space;if(statvfs(state,&space) || (uint64_t)space.f_bavail*space.f_frsize<release.size+UPDATE_MAX_EXPANDED+16*1024*1024)
            return update_error("Not enough free SD-card space to download and stage the update");
        char stage[UPDATE_PATH],download[UPDATE_PATH];
        if(file_path(stage,".stage-XXXXXX") || !mkdtemp(stage) || update_path(download,"%s/release.zip",stage))return -1;
        puts("Downloading the release package...");fflush(stdout);
        result=run_curl(release.url,download,release.size,1);
        if(!result){puts("Verifying package checksums and contents...");fflush(stdout);result=update_stage(download,stage,&release,0);}
        char source[UPDATE_PATH],dest[UPDATE_PATH];size_t length=0;
        unsigned char *json=NULL;
        if(!result && !file_path(source,"release.json"))json=update_read(source,512*1024,&length);
        if(!result && (!json || update_path(dest,"%s/release.json",stage) || update_write(dest,json,length)))result=-1;
        free(json);
        if(!result) {
            char packages[UPDATE_PATH];
            if(file_path(packages,"packages") || update_mkdir(packages) || update_safe_path(directory) || rename(stage,directory) ||
                update_sync_dir(packages) || update_sync_dir(state))result=-1;
        }
        if(result){remove_tree(stage);set_state("Download or validation failed; installed build unchanged");return -1;}
    }
    char installed_metadata[UPDATE_PATH];
    if(update_versions_stage(directory,current_metadata(installed_metadata)?installed_metadata:NULL,&release)) {
        // A refused package does not stay on the card, and the status says why it was refused.
        remove_tree(directory);set_state("Update refused by its versions; installed build unchanged");return -1;
    }
    // A staged identical build may be re-uploaded as a newer GitHub asset. Retain the metadata
    // just confirmed, rather than leaving the installed asset ID bound to an older upload.
    char source[UPDATE_PATH];size_t length=0;
    if(file_path(source,"release.json"))return -1;
    unsigned char *json=update_read(source,512*1024,&length);
    if(!json)return -1;
    result=update_write(metadata,json,length);free(json);if(result)return -1;
    // The pending update belongs to the build enabled now: a package installed by hand before
    // the next boot wins over it (pending_current, update-boot.sh).
    char pending[16],from[16],current[13];installed(current);
    snprintf(pending,sizeof(pending),"%s\n",release.build);snprintf(from,sizeof(from),"%s\n",current);
    if(put("pending-from",from) || put("pending",pending) || set_state("Update prepared; restart to install"))return -1;
    puts("Update verified and prepared. Restart when convenient to install it.");return 0;
}
static int commit(const char *build) {
    char pending[32],record_path[UPDATE_PATH];get("pending",pending,sizeof(pending));
    if(strcmp(pending,build) || update_path(record_path,"%s/BrickOS/setup-record",sd))return update_error("No matching confirmed update");
    size_t record_length;unsigned char *record=update_read(record_path,4096,&record_length);
    char expected[32];snprintf(expected,sizeof(expected),"build=%s\n",build);
    int installed_ok=record && record_length>=strlen(expected) && !memcmp(record,expected,strlen(expected));free(record);
    if(!installed_ok)return update_error("The package installer has not completed this build");
    if(verify(build))return -1;
    char directory[UPDATE_PATH],zip[UPDATE_PATH],metadata[UPDATE_PATH];struct update_release release;
    if(stage_paths(build,directory,zip,metadata) || update_parse_release(metadata,&release))return -1;
    char path[UPDATE_PATH];size_t length;
    if(update_path(path,"%s/" UPDATE_BOOT,directory))return -1;
    unsigned char *bytes=update_read(path,256*1024,&length);if(!bytes)return -1;
    char target[UPDATE_PATH];int bad=update_path(target,"%s/" UPDATE_BOOT,sd) || update_write(target,bytes,length);free(bytes);
    if(bad || chmod(target,0755))return -1;
    char old[13];installed(old);
    if(old[0] && strcmp(old,build) && put("previous-build",old))return -1;
    char line[80];snprintf(line,sizeof(line),"%s\n",build);
    if(update_path(target,"%s/BrickOS/enabled",sd) || update_write(target,line,strlen(line)))return -1;
    snprintf(line,sizeof(line),"%llu\n",(unsigned long long)release.id);
    if(put("installed-asset",line) || set_state("Update installed") || remove_file("pending-from") || remove_file("pending"))return -1;
    // Keep the current online package and its predecessor; unrelated folders are never removed.
    char packages[UPDATE_PATH],previous[13];get("previous-build",previous,sizeof(previous));
    if(!file_path(packages,"packages")) {
        DIR *dir=opendir(packages);struct dirent *entry;
        if(dir) {
            while((entry=readdir(dir)))if(update_hex(entry->d_name,12) && strcmp(entry->d_name,build) && strcmp(entry->d_name,previous)) {
                if(!update_path(path,"%s/%s",packages,entry->d_name))remove_tree(path);
            }
            closedir(dir);
        }
    }
    return update_sync_dir(state);
}
static int status(void) {
    char message[160],current[13];get("state",message,sizeof(message));installed(current);
    if(pending_current(0))strcpy(message,"Update prepared; restart to install");
    printf("{{update_status=%s}}\n{{update_installed=%s}}\n{{update_auto=%d}}\n",message[0]?message:"Not checked yet",current[0]?current:"Unknown",!exists("disabled"));
    char path[UPDATE_PATH];struct update_versions versions;int known=0;
    if(current_metadata(path) && update_regular(path) && !update_versions_load(path,&versions))known=1;
    printf("{{update_release=%s}}\n",known?versions.release:"Legacy / unknown");
    for(int i=0;i<UPDATE_COMPONENTS;++i)printf("{{version_%s=%s}}\n",update_component_ids[i],known?versions.version[i]:"Unknown");
    return 0;
}
static int monitor(void) {
    char path[UPDATE_PATH];if(update_path(path,"%s/brickos-update-monitor.lock",temporary))return -1;
    int singleton=lock_file(path);if(singleton<0)return 0;
    setsid();time_t due=0;int failures=0;
    for(;;) {
        char enabled[UPDATE_PATH],runtime[UPDATE_PATH];
        if(update_path(enabled,"%s/BrickOS/enabled",sd) || !update_regular(enabled) ||
            update_path(runtime,"%s/BrickOS/settings/Updates/brickos-update",sd) || !update_regular(runtime))break;
        time_t now=time(NULL);
        if(!exists("disabled") && !pending_current(1) && (now>=due || due-now>6*60*60)) {
            int lock=operation_lock();
            if(lock>=0) {
                struct update_release release;int result=latest(&release,1);
                if(result<0)++failures;else failures=0;
                // The frontend starts the monitor at boot, often before Wi-Fi has its address:
                // a failed check is retried after a minute, five times, then every fifteen minutes.
                due=now+(result<0?(failures<=5?60:15*60):6*60*60);close(lock);
            }
        }
        sleep(30);
    }
    close(singleton);return 0;
}
int main(int argc,char **argv) {
    BRICKOS_HANDLE_VERSION(argc, argv);
    setvbuf(stdout,NULL,_IOLBF,0);umask(022);
    const char *root=getenv("BRICKOS_ROOT");if(!root)root="";
    if(root[0] && (root[0]!='/' || strstr(root,"/../") || strstr(root,"/./")))return 2;
    char enabled[UPDATE_PATH];
    if(update_path(sd,"%s/mnt/SDCARD",root) || update_path(temporary,"%s/tmp",root) ||
        update_path(state,"%s/BrickOS/updates",sd) || update_path(enabled,"%s/BrickOS/enabled",sd))return 1;
    if(!update_regular(enabled)){update_error("BrickOS is not enabled on the SD card");return 1;}
    if(update_mkdir(temporary) || update_mkdir(state))return 1;
    if(argc==2 && !strcmp(argv[1],"status"))return status();
    if(argc==2 && !strcmp(argv[1],"versions")) {
        char path[UPDATE_PATH];struct update_versions versions;size_t length;
        if(update_path(path,"%s/BrickOS/versions.json",sd) || update_versions_load(path,&versions))return 1;
        unsigned char *bytes=update_read(path,512*1024,&length);if(!bytes)return 1;
        fwrite(bytes,1,length,stdout);free(bytes);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"monitor"))return monitor()?1:0;
    if(argc==3 && !strcmp(argv[1],"auto")) {
        if(!strcmp(argv[2],"0"))return put("disabled","1\n")?1:0;
        if(!strcmp(argv[2],"1"))return remove_file("disabled")?1:0;
        return 2;
    }
    int lock=operation_lock();if(lock<0)return 1;int result=-1;
    if(argc==2 && !strcmp(argv[1],"check")){struct update_release release;result=latest(&release,0)<0?-1:0;}
    else if(argc==2 && !strcmp(argv[1],"prepare"))result=prepare();
    else if(argc==3 && !strcmp(argv[1],"verify"))result=verify(argv[2]);
    else if(argc==3 && !strcmp(argv[1],"commit"))result=commit(argv[2]);
    else if(argc==2 && !strcmp(argv[1],"failed")){result=set_state("Installation failed; previous build retained") || remove_file("pending-from") || remove_file("pending");}
    else if(argc==2 && !strcmp(argv[1],"discard")){result=set_state("Prepared update dropped: another build is installed") || remove_file("pending-from") || remove_file("pending");}
    else update_error("usage: brickos-update check|prepare|status|monitor|auto 0|1|verify BUILD|commit BUILD|failed|discard");
    close(lock);return result?1:0;
}

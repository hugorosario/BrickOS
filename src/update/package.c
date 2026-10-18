#define _POSIX_C_SOURCE 200809L
#include "update.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

struct entry {
    char name[256], hash[65];
    uint32_t offset, compressed, size, crc, mode;
    unsigned method;
    int folder, listed;
};
static unsigned u16(const unsigned char *p) { return p[0] | (unsigned)p[1]<<8; }
static uint32_t u32(const unsigned char *p) { return u16(p) | (uint32_t)u16(p+2)<<16; }
static int valid_name(const char *name) {
    size_t at=0;
    while(name[at]) {
        size_t start=at;
        while(name[at] && name[at]!='/') {
            unsigned char c=(unsigned char)name[at++];
            if(!isalnum(c) && c!='.' && c!='-' && c!='_' && c!=' ')return 0;
        }
        size_t n=at-start;
        if(!n || (n==1 && name[start]=='.') || (n==2 && !strncmp(name+start,"..",2)) ||
            name[at-1]=='.' || name[at-1]==' ')return 0;
        if(name[at])++at;
    }
    return at>0;
}
static int allowed(const char *name, const char *build, int folder) {
    char prefix[80];snprintf(prefix,sizeof(prefix),"BrickOS/package/%s/",build);
    if(!strncmp(name,prefix,strlen(prefix)))return 1;
    if(!folder)return !strcmp(name,"BrickOS/enabled") || !strcmp(name,UPDATE_BOOT);
    return !strcmp(name,"BrickOS/") || !strcmp(name,"BrickOS/package/") ||
        !strcmp(name,"System/") || !strcmp(name,"System/starts/");
}
static unsigned char *expand(const unsigned char *zip, const struct entry *e) {
    unsigned char *bytes=malloc((size_t)e->size+1);if(!bytes)return NULL;
    if(e->method==0) {
        if(e->compressed!=e->size){free(bytes);return NULL;}
        memcpy(bytes,zip+e->offset,e->size);
    } else {
        z_stream s={0};s.next_in=(Bytef *)zip+e->offset;s.avail_in=e->compressed;
        s.next_out=bytes;s.avail_out=e->size+1;
        if(inflateInit2(&s,-MAX_WBITS)!=Z_OK){free(bytes);return NULL;}
        int result=inflate(&s,Z_FINISH);
        int valid=result==Z_STREAM_END && s.total_in==e->compressed && s.total_out==e->size;
        inflateEnd(&s);if(!valid){free(bytes);return NULL;}
    }
    if((uint32_t)crc32(0,bytes,e->size)!=e->crc){free(bytes);return NULL;}
    bytes[e->size]=0;return bytes;
}
static int manifest_check(char *text, size_t length, struct entry *entries, unsigned count, const char *build) {
    char hash[65],manifest_name[100];update_sha(text,length,hash);
    if(strncmp(hash,build,12) || !length || text[length-1]!='\n' || memchr(text,0,length))return -1;
    snprintf(manifest_name,sizeof(manifest_name),"BrickOS/package/%s/manifest.txt",build);
    for(char *line=text;*line;) {
        char *end=strchr(line,'\n');if(!end)return -1;*end=0;
        unsigned long long size;int start=0;
        if(sscanf(line,"%64s %llu %n",hash,&size,&start)!=2 || !start || !update_hex(hash,64))return -1;
        const char *path=line+start;char name[256];
        if(!strcmp(path,"../../../" UPDATE_BOOT))snprintf(name,sizeof(name),"%s",UPDATE_BOOT);
        else {
            if(!valid_name(path) || strlen(path)>180)return -1;
            snprintf(name,sizeof(name),"BrickOS/package/%s/%s",build,path);
        }
        unsigned i;for(i=0;i<count;++i)if(!strcmp(entries[i].name,name))break;
        if(i==count || entries[i].folder || entries[i].listed || !strcmp(name,manifest_name) ||
            !strcmp(name,"BrickOS/enabled") || entries[i].size!=size || strcmp(entries[i].hash,hash))return -1;
        entries[i].listed=1;line=end+1;
    }
    int setup=0,updater=0,boot=0;
    for(unsigned i=0;i<count;++i) {
        struct entry *e=&entries[i];
        if(!e->folder && !e->listed && strcmp(e->name,manifest_name) && strcmp(e->name,"BrickOS/enabled"))return -1;
        const char *suffix=e->name;
        if(!strncmp(suffix,"BrickOS/package/",16) && strlen(suffix)>29)suffix+=29;
        if(!strcmp(suffix,"setup.sh"))setup=1;
        if(!strcmp(suffix,"updater/brickos-update"))updater=1;
        if(!strcmp(e->name,UPDATE_BOOT))boot=1;
    }
    return setup && updater && boot ? 0 : -1;
}

// Accept only the deterministic, non-ZIP64 archive emitted by release-package.mjs. It is
// completely checked before any entry is extracted: paths, types, sizes, CRC, SHA and manifest.
// verify=1 compares every extracted file with the retained, digest-checked ZIP before boot.
int update_stage(const char *path, const char *directory, const struct update_release *release, int verify) {
    size_t length=0;unsigned char *zip=update_read(path,UPDATE_MAX_ZIP,&length);
    struct entry *entries=NULL;char *manifest=NULL;int result=-1;unsigned count=0;
    if(!zip || length<22)goto done;
    char digest[65];update_sha(zip,length,digest);
    if(length!=release->size || strcmp(digest,release->digest))goto done;
    const unsigned char *end=zip+length-22;
    if(u32(end)!=0x06054b50 || u16(end+4) || u16(end+6) || u16(end+20))goto done;
    count=u16(end+10);size_t central=u32(end+16),central_bytes=u32(end+12);
    if(!count || count>2048 || u16(end+8)!=count || central>length-22 || central_bytes!=length-22-central)goto done;
    entries=calloc(count,sizeof(*entries));if(!entries)goto done;
    size_t at=central,local_end=0;uint64_t total=0;size_t manifest_length=0;int marker=0;
    for(unsigned i=0;i<count;++i) {
        if(at>length-22 || length-22-at<46 || u32(zip+at)!=0x02014b50)goto done;
        const unsigned char *p=zip+at;unsigned name=u16(p+28),extra=u16(p+30),comment=u16(p+32);
        if(!name || name>=sizeof(entries[i].name) || (size_t)46+name+extra+comment>length-22-at ||
            u16(p+8)&~0x800 || u16(p+34))goto done;
        struct entry *e=&entries[i];memcpy(e->name,p+46,name);
        if(memchr(e->name,0,name) || !valid_name(e->name))goto done;
        e->folder=e->name[name-1]=='/';e->mode=u32(p+38)>>16;
        if((e->mode&0170000)!=(e->folder?0040000:0100000) || (e->mode&07000) ||
            !allowed(e->name,release->build,e->folder))goto done;
        for(unsigned j=0;j<i;++j) {
            if(!strcasecmp(e->name,entries[j].name))goto done;
            size_t n=strlen(entries[j].name),m=strlen(e->name);
            if(!entries[j].folder && m>n && !strncasecmp(e->name,entries[j].name,n) && e->name[n]=='/')goto done;
            if(!e->folder && n>m && !strncasecmp(e->name,entries[j].name,m) && entries[j].name[m]=='/')goto done;
        }
        e->method=u16(p+10);e->crc=u32(p+16);e->compressed=u32(p+20);e->size=u32(p+24);
        size_t local=u32(p+42);if(local!=local_end || local>central || central-local<30)goto done;
        const unsigned char *q=zip+local;size_t header=30u+u16(q+26)+u16(q+28);
        if(u32(q)!=0x04034b50 || u16(q+6)!=u16(p+8) || u16(q+8)!=e->method ||
            u32(q+14)!=e->crc || u32(q+18)!=e->compressed || u32(q+22)!=e->size ||
            u16(q+26)!=name || header>central-local || memcmp(q+30,e->name,name) ||
            e->compressed>central-local-header || e->size>32u*1024u*1024u ||
            (e->method!=0 && e->method!=8))goto done;
        e->offset=(uint32_t)(local+header);local_end=e->offset+e->compressed;
        total+=e->size;if(total>UPDATE_MAX_EXPANDED || (e->folder && e->size))goto done;
        unsigned char *bytes=expand(zip,e);if(!bytes)goto done;
        update_sha(bytes,e->size,e->hash);
        char name_manifest[100];snprintf(name_manifest,sizeof(name_manifest),"BrickOS/package/%s/manifest.txt",release->build);
        if(!strcmp(e->name,name_manifest)){manifest=(char *)bytes;manifest_length=e->size;}
        else {
            if(!strcmp(e->name,"BrickOS/enabled")) {
                marker=e->size==13 && !memcmp(bytes,release->build,12) && bytes[12]=='\n';
            }
            free(bytes);
        }
        at+=46u+name+extra+comment;
    }
    if(at!=length-22 || local_end!=central || !marker || !manifest ||
        manifest_check(manifest,manifest_length,entries,count,release->build))goto done;
    if(update_mkdir(directory))goto done;
    for(unsigned i=0;i<count;++i) {
        struct entry *e=&entries[i];char target[UPDATE_PATH];
        if(update_path(target,"%s/%s",directory,e->name) || update_safe_path(target))goto done;
        if(e->folder){if(update_mkdir(target))goto done;continue;}
        if(verify) {
            char hash[65];if(update_hash_file(target,hash) || strcmp(hash,e->hash))goto done;
        } else {
            if(access(target,F_OK)==0)goto done;
            unsigned char *bytes=expand(zip,e);if(!bytes)goto done;
            int bad=update_write(target,bytes,e->size);free(bytes);
            if(bad || chmod(target,e->mode&0777))goto done;
        }
    }
    result=0;
done:
    free(manifest);free(entries);free(zip);
    return result ? update_error("Package rejected: checksum, ZIP layout or manifest validation failed") : 0;
}

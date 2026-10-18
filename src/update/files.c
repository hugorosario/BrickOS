#define _POSIX_C_SOURCE 200809L
#include "update.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int update_error(const char *format, ...) {
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args);
    fputc('\n', stderr); return -1;
}
int update_path(char *out, const char *format, ...) {
    va_list args; va_start(args, format); int n = vsnprintf(out, UPDATE_PATH, format, args); va_end(args);
    return n < 0 || n >= UPDATE_PATH ? update_error("Path is too long") : 0;
}
int update_hex(const char *text, size_t length) {
    if (strlen(text) != length) return 0;
    return strspn(text, "0123456789abcdef") == length;
}
// Every existing component, including the last, must be a real directory/file. No symlinks.
int update_safe_path(const char *path) {
    char copy[UPDATE_PATH]; if (update_path(copy, "%s", path)) return -1;
    for (char *p = copy + 1;; ++p) {
        if (*p && *p != '/') continue;
        char saved = *p; *p = 0; struct stat st;
        if (lstat(copy, &st) == 0) {
            if (S_ISLNK(st.st_mode) || (saved && !S_ISDIR(st.st_mode))) return update_error("Unsafe path: %s", copy);
        } else if (errno != ENOENT) return update_error("Cannot inspect %s: %s", copy, strerror(errno));
        *p = saved; if (!saved) break;
    }
    return 0;
}
int update_mkdir(const char *path) {
    char copy[UPDATE_PATH]; if (update_path(copy, "%s", path) || update_safe_path(copy)) return -1;
    for (char *p = copy + 1;; ++p) {
        if (*p && *p != '/') continue;
        char saved = *p; *p = 0;
        int made=mkdir(copy,0755)==0;
        if (!made && errno != EEXIST) return update_error("Cannot create %s", copy);
        struct stat st; if (lstat(copy, &st) || !S_ISDIR(st.st_mode)) return -1;
        if(made) {
            char *last=strrchr(copy,'/');
            if(last && last!=copy){*last=0;int bad=update_sync_dir(copy);*last='/';if(bad)return -1;}
        }
        *p = saved; if (!saved) break;
    }
    return 0;
}
int update_sync_dir(const char *path) {
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    int bad=fsync(fd);close(fd);return bad?-1:0;
}
int update_regular(const char *path) {
    struct stat st; return !update_safe_path(path) && !lstat(path, &st) && S_ISREG(st.st_mode);
}
unsigned char *update_read(const char *path, size_t limit, size_t *length) {
    if (!update_regular(path)) return NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); struct stat st;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || st.st_size < 0 || (uint64_t)st.st_size > limit) { close(fd); return NULL; }
    size_t size = (size_t)st.st_size, at = 0; unsigned char *bytes = malloc(size + 1);
    if (!bytes) { close(fd); return NULL; }
    while (at < size) {
        ssize_t n = read(fd, bytes + at, size - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(bytes); close(fd); return NULL; }
        at += (size_t)n;
    }
    close(fd); bytes[size] = 0; *length = size; return bytes;
}
int update_write(const char *path, const void *bytes, size_t length) {
    char temporary[UPDATE_PATH], parent[UPDATE_PATH];
    if (update_safe_path(path) || update_path(parent, "%s", path)) return -1;
    char *slash = strrchr(parent, '/'); if (!slash) return -1; *slash = 0;
    if (update_mkdir(parent) || update_path(temporary, "%s.tmp-XXXXXX", path)) return -1;
    int fd = mkstemp(temporary); if (fd < 0) return -1;
    size_t at = 0; int bad = 0;
    while (at < length) {
        ssize_t n = write(fd, (const char *)bytes + at, length - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { bad = 1; break; } at += (size_t)n;
    }
    if (fsync(fd)) bad = 1;
    if (close(fd)) bad = 1;
    if (!bad && rename(temporary, path)) bad = 1;
    if (bad) unlink(temporary);
    fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { if (fsync(fd)) bad = 1; close(fd); }
    return bad ? update_error("Cannot write %s", path) : 0;
}

// SHA-256 (FIPS 180-4), independent of firmware crypto library versions.
static uint32_t rotate(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void block(uint32_t h[8], const unsigned char *p) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4*i]<<24 | (uint32_t)p[4*i+1]<<16 | (uint32_t)p[4*i+2]<<8 | p[4*i+3];
    for (int i = 16; i < 64; ++i) {
        uint32_t a=w[i-15], b=w[i-2];
        w[i]=w[i-16]+(rotate(a,7)^rotate(a,18)^(a>>3))+w[i-7]+(rotate(b,17)^rotate(b,19)^(b>>10));
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],v=h[7];
    for (int i=0;i<64;++i) {
        uint32_t t=v+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t u=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
        v=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=v;
}
void update_sha(const void *data, size_t length, char out[65]) {
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const unsigned char *bytes=data; size_t full=length/64;
    for(size_t i=0;i<full;++i) block(h,bytes+64*i);
    unsigned char tail[128]={0}; size_t left=length%64; memcpy(tail,bytes+full*64,left);tail[left]=0x80;
    size_t end=left<56?64:128;uint64_t bits=(uint64_t)length*8;
    for(int i=0;i<8;++i)tail[end-1-i]=(unsigned char)(bits>>(8*i));
    block(h,tail);if(end==128)block(h,tail+64);
    for(int i=0;i<8;++i)snprintf(out+8*i,9,"%08x",h[i]);
}
int update_hash_file(const char *path, char out[65]) {
    size_t length;unsigned char *bytes=update_read(path,UPDATE_MAX_ZIP,&length);if(!bytes)return -1;
    update_sha(bytes,length,out);free(bytes);return 0;
}

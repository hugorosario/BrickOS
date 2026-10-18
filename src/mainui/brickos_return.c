#define _POSIX_C_SOURCE 200809L
#include "brickos_return.h"

#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct sha256_state {
    uint32_t h[8];
    uint64_t bits;
    unsigned char block[64];
    size_t used;
};

static uint32_t rotr(uint32_t value, unsigned bits) { return (value >> bits) | (value << (32 - bits)); }

static void sha256_block(struct sha256_state *state, const unsigned char *block) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
    };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
               (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state->h[0], b = state->h[1], c = state->h[2], d = state->h[3];
    uint32_t e = state->h[4], f = state->h[5], g = state->h[6], h = state->h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + choice + k[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + majority;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state->h[0] += a; state->h[1] += b; state->h[2] += c; state->h[3] += d;
    state->h[4] += e; state->h[5] += f; state->h[6] += g; state->h[7] += h;
}

static void sha256_update(struct sha256_state *state, const unsigned char *bytes, size_t length) {
    state->bits += (uint64_t)length * 8;
    while (length) {
        size_t take = sizeof(state->block) - state->used;
        if (take > length) take = length;
        memcpy(state->block + state->used, bytes, take);
        state->used += take; bytes += take; length -= take;
        if (state->used == sizeof(state->block)) { sha256_block(state, state->block); state->used = 0; }
    }
}

int mainui_brickos_return_digest(const void *bytes, size_t length, char out[MAINUI_BRICKOS_RETURN_DIGEST]) {
    if ((!bytes && length) || !out) return -1;
    struct sha256_state state = {{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                  0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}, 0, {0}, 0};
    sha256_update(&state, bytes, length);
    uint64_t bits = state.bits;
    unsigned char marker = 0x80, zero = 0;
    sha256_update(&state, &marker, 1);
    while (state.used != 56) sha256_update(&state, &zero, 1);
    unsigned char length_bytes[8];
    for (int i = 0; i < 8; ++i) length_bytes[7 - i] = (unsigned char)(bits >> (i * 8));
    // Padding is excluded from the message length.
    state.bits = bits;
    sha256_update(&state, length_bytes, sizeof(length_bytes));
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        for (int b = 0; b < 4; ++b) {
            unsigned char byte = (unsigned char)(state.h[i] >> (24 - b * 8));
            out[(i * 4 + b) * 2] = hex[byte >> 4];
            out[(i * 4 + b) * 2 + 1] = hex[byte & 15];
        }
    }
    out[64] = 0;
    return 0;
}

static int safe_token(const char *token) {
    if (!token || !token[0] || strlen(token) >= MAINUI_BRICKOS_RETURN_TOKEN) return 0;
    for (const unsigned char *p = (const unsigned char *)token; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) return 0;
    return 1;
}

static int atomic_write(const char *path, const char *text, size_t length) {
    char temporary[2048];
    if (!path || !path[0] || snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >= (int)sizeof(temporary)) return -1;
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (fchmod(fd, 0600) < 0) { close(fd); unlink(temporary); return -1; }
    size_t written = 0;
    int ok = 1;
    while (written < length) {
        ssize_t n = write(fd, text + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = 0; break; }
        written += (size_t)n;
    }
    if (ok && fsync(fd) < 0) ok = 0;
    if (close(fd) < 0) ok = 0;
    if (!ok || rename(temporary, path) < 0) { unlink(temporary); return -1; }
    return 0;
}

static int valid_digest(const char *digest) {
    if (!digest || strlen(digest) != 64) return 0;
    for (int i = 0; i < 64; ++i)
        if (!((digest[i] >= '0' && digest[i] <= '9') || (digest[i] >= 'a' && digest[i] <= 'f'))) return 0;
    return 1;
}

static int publish(const char *path, const char *token, const char *digest, const char *view_digest) {
    if (!safe_token(token) || !valid_digest(digest) || (view_digest && !valid_digest(view_digest))) return -1;
    char text[512];
    int n = view_digest
        ? snprintf(text, sizeof(text), "{\"version\":1,\"token\":\"%s\",\"sha256\":\"%s\",\"view_sha256\":\"%s\"}\n",
                   token, digest, view_digest)
        : snprintf(text, sizeof(text), "{\"version\":1,\"token\":\"%s\",\"sha256\":\"%s\"}\n", token, digest);
    return n > 0 && (size_t)n < sizeof(text) ? atomic_write(path, text, (size_t)n) : -1;
}

int mainui_brickos_return_publish(const char *path, const char *token, const char *digest) {
    return publish(path, token, digest, NULL);
}

int mainui_brickos_return_publish_view(const char *path, const char *token, const char *digest,
                                      const char *view_digest) {
    return publish(path, token, digest, view_digest);
}

static int file_digest(const char *path, char out[MAINUI_BRICKOS_RETURN_DIGEST]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 1024 * 1024) {
        close(fd); return -1;
    }
    size_t length = (size_t)st.st_size;
    unsigned char *bytes = malloc(length);
    if (!bytes) { close(fd); return -1; }
    size_t used = 0;
    while (used < length) {
        ssize_t n = read(fd, bytes + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(bytes); close(fd); return -1; }
        used += (size_t)n;
    }
    close(fd);
    int result = mainui_brickos_return_digest(bytes, length, out);
    free(bytes);
    return result;
}

static int validate(const char *path, const char *pending_token, const char *digest, const char *view_digest) {
    if (!safe_token(pending_token) || !valid_digest(digest) || (view_digest && !valid_digest(view_digest))) return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size >= 1024) { close(fd); return 0; }
    char text[1024];
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, text + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return 0; }
        used += (size_t)n;
    }
    close(fd);
    struct mainui_json json;
    if (mainui_json_parse(&json, text, used) < 0) return 0;
    const struct mainui_json_member *version = mainui_json_last(&json, "version");
    const struct mainui_json_member *token = mainui_json_last(&json, "token");
    const struct mainui_json_member *sha = mainui_json_last(&json, "sha256");
    const struct mainui_json_member *view = mainui_json_last(&json, "view_sha256");
    return version && version->type == MAINUI_JSON_NUMBER && version->number == 1.0 &&
           token && token->type == MAINUI_JSON_STRING && token->text_length == strlen(pending_token) &&
           strcmp(token->text, pending_token) == 0 && sha && sha->type == MAINUI_JSON_STRING &&
           sha->text_length == 64 && memcmp(sha->text, digest, 64) == 0 &&
           (!view_digest || (view && view->type == MAINUI_JSON_STRING && view->text_length == 64 &&
                             memcmp(view->text, view_digest, 64) == 0));
}

int mainui_brickos_return_validate(const char *path, const char *pending_token, const char *digest) {
    return validate(path, pending_token, digest, NULL);
}

int mainui_brickos_return_validate_view(const char *path, const char *pending_token, const char *digest,
                                       const char *view_path) {
    char view_digest[MAINUI_BRICKOS_RETURN_DIGEST];
    return file_digest(view_path, view_digest) == 0 && validate(path, pending_token, digest, view_digest);
}

void mainui_brickos_return_consume(const char *path) { if (path) unlink(path); }

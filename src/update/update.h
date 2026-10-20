#ifndef BRICKOS_UPDATE_H
#define BRICKOS_UPDATE_H
#include <stddef.h>
#include <stdint.h>

#define UPDATE_REPO "hugorosario/BrickOS"
#define UPDATE_API "https://api.github.com/repos/" UPDATE_REPO "/releases/latest"
#define UPDATE_DOWNLOAD "https://github.com/" UPDATE_REPO "/releases/download/"
#define UPDATE_MAX_ZIP (128u * 1024u * 1024u)
#define UPDATE_MAX_EXPANDED (256u * 1024u * 1024u)
#define UPDATE_BOOT "System/starts/brickos-00-setup.sh"
#define UPDATE_PATH 1024

struct update_release {
    char build[13], tag[80], version[64], url[1024], digest[65];
    uint64_t id, size;
};

int update_error(const char *format, ...);
int update_path(char *out, const char *format, ...);
int update_hex(const char *text, size_t length);
int update_safe_path(const char *path);
int update_mkdir(const char *path);
int update_regular(const char *path);
unsigned char *update_read(const char *path, size_t limit, size_t *length);
int update_write(const char *path, const void *bytes, size_t length);
int update_sync_dir(const char *path);
void update_sha(const void *data, size_t length, char out[65]);
int update_hash_file(const char *path, char out[65]);
int update_stage(const char *zip, const char *directory, const struct update_release *release, int verify);
int update_parse_release(const char *path, struct update_release *release);

// The release notes' changelog block, which tools/changelog.mjs writes into them.
#define UPDATE_CHANGELOG_OPEN "<!-- brickos-changelog -->"
#define UPDATE_CHANGELOG_CLOSE "<!-- /brickos-changelog -->"
// The release notes ("body") of a saved release reply, or NULL; the caller frees it.
char *update_release_body(const char *path);
// The bullets of the notes' changelog block as plain "- " lines, each ending in a newline, at
// most max_lines of them and size - 1 bytes. *cut is set when something was left out. Returns
// the number of lines, 0 for a block without bullets, or -1 when the notes have no block.
int update_changelog_lines(const char *body, char *out, size_t size, int max_lines, int *cut);
#endif

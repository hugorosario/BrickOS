#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "launch.h"

#include <fcntl.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PATH_BYTES 4096

// Appends text to out at *n; returns -1 when it does not fit.
static int put(char *out, size_t size, size_t *n, const char *text) {
    size_t length = strlen(text);
    if (*n + length >= size) return -1;
    memcpy(out + *n, text, length + 1);
    *n += length;
    return 0;
}

// Appends a path between double quotes, escaping the four characters that stay special there.
static int put_quoted(char *out, size_t size, size_t *n, const char *path) {
    if (put(out, size, n, "\"") < 0) return -1;
    for (const char *p = path; *p; ++p) {
        char pair[3] = {'\\', *p, 0};
        if (put(out, size, n, strchr("\\\"$`", *p) ? pair : pair + 1) < 0) return -1;
    }
    return put(out, size, n, "\"");
}

// A path the shell reads as one plain word needs no quotes.
static int plain_word(const char *path) {
    if (!*path) return 0;
    for (const char *p = path; *p; ++p)
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/._-+,:@%", *p)) return 0;
    return 1;
}

static int put_word(char *out, size_t size, size_t *n, const char *path) {
    return plain_word(path) ? put(out, size, n, path) : put_quoted(out, size, n, path);
}

enum mainui_launch_result mainui_launch_game_command(char *out, size_t size, const char *launcher,
                                                     const char *rompath) {
    size_t n = 0;
    if (size) out[0] = 0;
    if (put(out, size, &n, "chmod a+x ") < 0 || put_quoted(out, size, &n, launcher) < 0 ||
        put(out, size, &n, ";  ") < 0 || put_quoted(out, size, &n, launcher) < 0 ||
        put(out, size, &n, " ") < 0 || put_quoted(out, size, &n, rompath) < 0)
        return MAINUI_LAUNCH_TOO_LONG;
    return MAINUI_LAUNCH_OK;
}

enum mainui_launch_result mainui_launch_netplay_command(char *out, size_t size, const char *launcher,
                                                       const char *rompath, const char *hostip) {
    size_t n = 0;
    struct in_addr address;
    if (size) out[0] = 0;
    if (!hostip || inet_pton(AF_INET, hostip, &address) != 1) return MAINUI_LAUNCH_NO_TARGET;
    if (put(out, size, &n, "chmod a+x ") < 0 || put_quoted(out, size, &n, launcher) < 0 ||
        put(out, size, &n, "; NET_PARAM=\"-C ") < 0 || put(out, size, &n, hostip) < 0 ||
        put(out, size, &n, "\" ") < 0 || put_quoted(out, size, &n, launcher) < 0 ||
        put(out, size, &n, " ") < 0 || put_quoted(out, size, &n, rompath) < 0) return MAINUI_LAUNCH_TOO_LONG;
    return MAINUI_LAUNCH_OK;
}

enum mainui_launch_result mainui_launch_host_command(char *out, size_t size, const char *launcher,
                                                    const char *rompath) {
    size_t n = 0;
    if (size) out[0] = 0;
    if (put(out, size, &n, "chmod a+x ") < 0 || put_quoted(out, size, &n, launcher) < 0 ||
        put(out, size, &n, "; NET_PARAM=-H ") < 0 || put_quoted(out, size, &n, launcher) < 0 ||
        put(out, size, &n, " ") < 0 || put_quoted(out, size, &n, rompath) < 0) return MAINUI_LAUNCH_TOO_LONG;
    return MAINUI_LAUNCH_OK;
}

const char *mainui_launch_builtin_core(int type) {
    switch (type) {
    case 5: return "/usr/trimui/bin/fbalpha2012_libretro.so";
    case 6: return "/usr/trimui/bin/fbalpha2012_neogeo_libretro.so";
    case 7: return "/usr/trimui/bin/fceumm_libretro.so";
    default: return NULL;
    }
}

static enum mainui_launch_result builtin_command(char *out, size_t size, int type,
                                                 const char *rompath, const char *hostip,
                                                 int hosting, const char *home) {
    if (size) out[0] = 0;
    const char *core = mainui_launch_builtin_core(type);
    if (!core) return MAINUI_LAUNCH_NO_LAUNCHER;
    struct in_addr address;
    if ((hostip && inet_pton(AF_INET, hostip, &address) != 1) || !rompath || !rompath[0] || (home && !home[0]))
        return MAINUI_LAUNCH_NO_TARGET;
    if (!home) home = type == 5 ? "/mnt/SDCARD/Roms/ARCADE/" : type == 6 ? "/mnt/SDCARD/Roms/NEOGEO/" : "/mnt/SDCARD/Roms/FC/";
    // Static defemu_loadrom_internal: clients have type-specific spacing; normal/host
    // commands have two spaces after HOME and no tail. Keep safe path quoting in both.
    size_t n = 0;
    if (put(out, size, &n, "cd /usr/trimui/bin/;HOME=") < 0 || put_word(out, size, &n, home) < 0 ||
        put(out, size, &n, !hostip || type == 5 ? "  /usr/trimui/bin/retroarch " : " /usr/trimui/bin/retroarch ") < 0 ||
        (hostip && (put(out, size, &n, "-C ") < 0 || put(out, size, &n, hostip) < 0)) ||
        (!hostip && hosting && put(out, size, &n, "-H") < 0) ||
        put(out, size, &n, " -L ") < 0 || put(out, size, &n, core) < 0 ||
        put(out, size, &n, " ") < 0 || put_quoted(out, size, &n, rompath) < 0 ||
        (hostip && type != 5 && put(out, size, &n, " ") < 0)) return MAINUI_LAUNCH_TOO_LONG;
    return MAINUI_LAUNCH_OK;
}

enum mainui_launch_result mainui_launch_builtin_netplay_command(char *out, size_t size, int type,
                                                               const char *rompath, const char *hostip,
                                                               const char *home) {
    if (!hostip) return MAINUI_LAUNCH_NO_TARGET;
    return builtin_command(out, size, type, rompath, hostip, 0, home);
}

enum mainui_launch_result mainui_launch_builtin_game_command(char *out, size_t size, int type,
                                                            const char *rompath, int hosting, const char *home) {
    return builtin_command(out, size, type, rompath, NULL, hosting, home);
}

enum mainui_launch_result mainui_launch_app_command(char *out, size_t size, const char *directory,
                                                    const char *script) {
    // The app's config.json "launch" runs from its folder as ./<launch> (a2).
    char relative[PATH_BYTES];
    if (snprintf(relative, sizeof(relative), "%s%s", script[0] == '/' ? "" : "./", script) >= (int)sizeof(relative))
        return MAINUI_LAUNCH_TOO_LONG;
    size_t n = 0;
    if (size) out[0] = 0;
    if (put(out, size, &n, "cd ") < 0 || put_word(out, size, &n, directory) < 0 ||
        put(out, size, &n, "; chmod a+x ") < 0 || put_word(out, size, &n, relative) < 0 ||
        put(out, size, &n, "; ") < 0 || put_word(out, size, &n, relative) < 0 || put(out, size, &n, " ") < 0)
        return MAINUI_LAUNCH_TOO_LONG;
    return MAINUI_LAUNCH_OK;
}

enum mainui_launch_result mainui_launch_port_command(char *out, size_t size, const char *launch) {
    const char *slash = strrchr(launch, '/');
    if (size) out[0] = 0;
    if (!slash || !slash[1]) return MAINUI_LAUNCH_NO_LAUNCHER;
    int written = snprintf(out, size, "cd %.*s; chmod a+x ./%s; ./%s ", (int)(slash - launch), launch, slash + 1, slash + 1);
    return written >= 0 && (size_t)written < size ? MAINUI_LAUNCH_OK : MAINUI_LAUNCH_TOO_LONG;
}

static int under(char *out, const char *root, const char *path) {
    return snprintf(out, PATH_BYTES, "%s%s", root ? root : "", path) < PATH_BYTES ? 0 : -1;
}

static int is_file(const char *root, const char *path) {
    char full[PATH_BYTES];
    struct stat st;
    return under(full, root, path) == 0 && stat(full, &st) == 0 && S_ISREG(st.st_mode);
}

static int exists(const char *root, const char *path) {
    char full[PATH_BYTES];
    return under(full, root, path) == 0 && access(full, F_OK) == 0;
}

void mainui_launch_clear(const char *root) {
    char path[PATH_BYTES];
    if (under(path, root, "/tmp/.cmdenc") == 0) unlink(path);
    if (under(path, root, "/tmp/.tmenc") == 0) unlink(path);
    if (under(path, root, "/tmp/.cmdenc.brickos") == 0) unlink(path);
    if (under(path, root, "/tmp/.tmenc.brickos") == 0) unlink(path);
    if (under(path, root, "/tmp/cmd_to_run.sh") == 0) unlink(path);
    if (under(path, root, "/tmp/cmd_to_run.sh.brickos") == 0) unlink(path);
    if (under(path, root, "/tmp/host_msg") == 0) unlink(path);
    if (under(path, root, "/tmp/host_msg.brickos") == 0) unlink(path);
}

static enum mainui_launch_result encode_text(const struct mainui_encoder *encoder,
                                             const char *text, unsigned char out[256]) {
    if (!out) return MAINUI_LAUNCH_ENCODING_FAILED;
    memset(out, 0, 256);
    if (!text || !*text) return MAINUI_LAUNCH_NO_TARGET;
    size_t length = strlen(text);
    if (length > 255) return MAINUI_LAUNCH_TOO_LONG;
    if (!encoder || !encoder->block) return MAINUI_LAUNCH_ENCODING_FAILED;
    unsigned char input[256] = {0}, encoded[256] = {0}, decoded[256] = {0};
    memcpy(input, text, length);
    for (int offset = 0; offset < 256; offset += 16)
        if (encoder->block(encoder->context, input + offset, encoded + offset, 0)) return MAINUI_LAUNCH_ENCODING_FAILED;
    for (int offset = 0; offset < 256; offset += 16)
        if (encoder->block(encoder->context, encoded + offset, decoded + offset, 1)) return MAINUI_LAUNCH_ENCODING_FAILED;
    if (memcmp(input, decoded, 256)) return MAINUI_LAUNCH_ENCODING_FAILED;
    memcpy(out, encoded, 256);
    return MAINUI_LAUNCH_OK;
}

enum mainui_launch_result mainui_launch_encode_path(const struct mainui_encoder *encoder,
                                                   const char *rompath, unsigned char out[256]) {
    if (!out) return MAINUI_LAUNCH_ENCODING_FAILED;
    if (!rompath || rompath[0] != '/') { memset(out, 0, 256); return MAINUI_LAUNCH_NO_TARGET; }
    return encode_text(encoder, rompath, out);
}

static enum mainui_launch_result encoded_consumer(const char *root) {
    char path[PATH_BYTES]; struct stat st;
    if (under(path, root, "/root/gameloader") < 0) return MAINUI_LAUNCH_TOO_LONG;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & 0111)) return MAINUI_LAUNCH_NO_LAUNCHER;
    return MAINUI_LAUNCH_OK;
}

static int stage_encoded(int directory, const char *name, const unsigned char bytes[256]) {
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int ok = 1;
    for (size_t written = 0; written < 256;) {
        ssize_t amount = write(fd, bytes + written, 256 - written);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) { ok = 0; break; }
        written += (size_t)amount;
    }
    if (fsync(fd)) ok = 0;
    if (close(fd)) ok = 0;
    if (!ok) unlinkat(directory, name, 0);
    return ok ? 0 : -1;
}

static enum mainui_launch_result publish_encoded(const char *root, const unsigned char bytes[256], int companion) {
    char path[PATH_BYTES];
    if (under(path, root, "/tmp") < 0) return MAINUI_LAUNCH_TOO_LONG;
    int directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return MAINUI_LAUNCH_WRITE_FAILED;
    int staged_path = 0, staged_command = 0, committed_path = 0, committed_command = 0;
    int ok = stage_encoded(directory, ".tmenc.brickos", bytes) == 0;
    staged_path = ok;
    if (ok) { ok = stage_encoded(directory, ".cmdenc.brickos", bytes) == 0; staged_command = ok; }
    // Nothing can take the init's encoded branch until both files are complete.
    static const char *const competing[] = {".cmdenc", "cmd_to_run.sh", "host_msg"};
    size_t competing_count = companion ? 1 : sizeof(competing) / sizeof(competing[0]);
    for (size_t i = 0; ok && i < competing_count; ++i)
        if (unlinkat(directory, competing[i], 0) && errno != ENOENT) ok = 0;
    if (ok) { ok = renameat(directory, ".tmenc.brickos", directory, ".tmenc") == 0; committed_path = ok; }
    if (ok) { ok = renameat(directory, ".cmdenc.brickos", directory, ".cmdenc") == 0; committed_command = ok; }
    if (ok && fsync(directory)) ok = 0;
    if (staged_path) unlinkat(directory, ".tmenc.brickos", 0);
    if (staged_command) unlinkat(directory, ".cmdenc.brickos", 0);
    if (!ok && committed_command) unlinkat(directory, ".cmdenc", 0);
    if (!ok && committed_path) unlinkat(directory, ".tmenc", 0);
    close(directory);
    return ok ? MAINUI_LAUNCH_OK : MAINUI_LAUNCH_WRITE_FAILED;
}

enum mainui_launch_result mainui_launch_encoded(const char *root, const char *rompath,
                                               const struct mainui_encoder *encoder) {
    enum mainui_launch_result result = encoded_consumer(root);
    if (result != MAINUI_LAUNCH_OK) return result;
    if (!rompath || rompath[0] != '/' || !is_file(root, rompath)) return MAINUI_LAUNCH_NO_TARGET;
    unsigned char bytes[256];
    result = mainui_launch_encode_path(encoder, rompath, bytes);
    return result == MAINUI_LAUNCH_OK ? publish_encoded(root, bytes, 0) : result;
}

static enum mainui_launch_result publish(const char *root, const char *command) {
    char final_path[PATH_BYTES], temporary[PATH_BYTES], encoded[PATH_BYTES];
    if (under(final_path, root, "/tmp/cmd_to_run.sh") < 0 || under(temporary, root, "/tmp/cmd_to_run.sh.brickos") < 0 ||
        under(encoded, root, "/tmp/.cmdenc") < 0)
        return MAINUI_LAUNCH_TOO_LONG;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (fd < 0) return MAINUI_LAUNCH_WRITE_FAILED;
    size_t length = strlen(command);
    int ok = write(fd, command, length) == (ssize_t)length;
    if (fsync(fd) != 0) ok = 0;
    if (close(fd) != 0) ok = 0;
    // The init prefers an encoded command; one left from an earlier run would replace this one.
    if (ok && unlink(encoded) != 0 && access(encoded, F_OK) == 0) ok = 0;
    if (!ok || rename(temporary, final_path) != 0) {
        unlink(temporary);
        return MAINUI_LAUNCH_WRITE_FAILED;
    }
    return MAINUI_LAUNCH_OK;
}

static enum mainui_launch_result publish_host(const char *root, const char *command, const char *advertisement) {
    char host[PATH_BYTES], temporary[PATH_BYTES];
    if (under(host, root, "/tmp/host_msg") < 0 || under(temporary, root, "/tmp/host_msg.brickos") < 0)
        return MAINUI_LAUNCH_TOO_LONG;
    if (advertisement) {
        int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return MAINUI_LAUNCH_WRITE_FAILED;
        size_t length = strlen(advertisement);
        int ok = write(fd, advertisement, length) == (ssize_t)length;
        if (fsync(fd) != 0) ok = 0;
        if (close(fd) != 0) ok = 0;
        if (!ok || rename(temporary, host) != 0) {
            unlink(temporary);
            return MAINUI_LAUNCH_WRITE_FAILED;
        }
    } else if (unlink(host) != 0 && access(host, F_OK) == 0) return MAINUI_LAUNCH_WRITE_FAILED;
    enum mainui_launch_result result = publish(root, command);
    if (result != MAINUI_LAUNCH_OK && advertisement) unlink(host);
    return result;
}

enum mainui_launch_result mainui_launch_game(const char *root, const char *launcher, const char *rompath) {
    char command[3 * PATH_BYTES];
    if (!is_file(root, launcher)) return MAINUI_LAUNCH_NO_LAUNCHER;
    if (!exists(root, rompath)) return MAINUI_LAUNCH_NO_TARGET;
    enum mainui_launch_result result = mainui_launch_game_command(command, sizeof(command), launcher, rompath);
    return result == MAINUI_LAUNCH_OK ? publish(root, command) : result;
}

static int netplay_rom_exists(const char *root, const char *rompath) {
    char full[PATH_BYTES], actual[PATH_BYTES], sd_path[PATH_BYTES], sd[PATH_BYTES];
    // A network advertisement names a ROM on this card. Resolve symlinks and dot segments
    // before accepting it; a peer cannot make the launcher open a file outside the card.
    if (under(full, root, rompath) < 0 || under(sd_path, root, "/mnt/SDCARD") < 0 ||
        !realpath(full, actual) || !realpath(sd_path, sd)) return 0;
    size_t length = strlen(sd);
    if (strncmp(actual, sd, length) || actual[length] != '/' || !is_file(root, rompath))
        return 0;
    return 1;
}

enum mainui_launch_result mainui_launch_netplay(const char *root, const char *launcher, const char *rompath,
                                               const char *hostip) {
    char command[3 * PATH_BYTES];
    if (!is_file(root, launcher)) return MAINUI_LAUNCH_NO_LAUNCHER;
    if (!netplay_rom_exists(root, rompath)) return MAINUI_LAUNCH_NO_TARGET;
    enum mainui_launch_result result = mainui_launch_netplay_command(command, sizeof(command), launcher, rompath, hostip);
    return result == MAINUI_LAUNCH_OK ? publish(root, command) : result;
}

enum mainui_launch_result mainui_launch_encoded_netplay(const char *root, const char *rompath,
                                                       const struct mainui_encoder *encoder) {
    if (!rompath || !netplay_rom_exists(root, rompath)) return MAINUI_LAUNCH_NO_TARGET;
    return mainui_launch_encoded(root, rompath, encoder);
}

int mainui_launch_other_platform_card(const char *root) {
    char path[PATH_BYTES];
    if (!exists(root, "/sys/block/mmcblk0/mmcblk0p1") || !exists(root, "/sys/block/mmcblk0/mmcblk0p2") ||
        under(path, root, "/mnt/SDCARD") < 0) return 0;
    DIR *directory = opendir(path);
    if (!directory) return 0;
    int found = 0;
    struct dirent *entry;
    // Static sdcardOfOtherPlatform: only visible regular entries with this six-byte
    // prefix count; directories and symlinks named KERNEL do not identify the card.
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] != '.' && entry->d_type == DT_REG && !strncasecmp(entry->d_name, "KERNEL", 6)) {
            found = 1;
            break;
        }
    }
    closedir(directory);
    return found;
}

static void builtin_neogeo_bios(const char *root, const char *home) {
    // defemu_loadrom_internal only prepares this BIOS on an other-platform card.
    // Its copy is best effort. Existing entries stay untouched, and a failed copy
    // must not prevent the otherwise valid game handoff.
    char source[PATH_BYTES], parent_path[PATH_BYTES], parent[PATH_BYTES], temporary[128], card_path[PATH_BYTES], card[PATH_BYTES];
    if (under(source, root, "/mnt/P2EXT/system/neogeo.zip") < 0 || under(parent_path, root, home) < 0 ||
        under(card_path, root, "/mnt/SDCARD") < 0 || !realpath(card_path, card) || !realpath(parent_path, parent)) return;
    size_t card_length = strlen(card);
    if (strncmp(parent, card, card_length) || (parent[card_length] && parent[card_length] != '/')) return;
    int input = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (input < 0) return;
    if (fstat(input, &st) != 0 || !S_ISREG(st.st_mode)) { close(input); return; }
    int directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) { close(input); return; }
    snprintf(temporary, sizeof(temporary), "/proc/self/fd/%d", directory);
    if (!realpath(temporary, parent) || strncmp(parent, card, card_length) ||
        (parent[card_length] && parent[card_length] != '/')) { close(directory); close(input); return; }
    if (fstatat(directory, "neogeo.zip", &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
        close(directory); close(input); return;
    }
    snprintf(temporary, sizeof(temporary), "/proc/self/fd/%d/.brickos-bios-XXXXXX", directory);
    int output = mkstemp(temporary);
    if (output < 0) { close(directory); close(input); return; }
    int ok = fcntl(output, F_SETFD, FD_CLOEXEC) == 0 && fchmod(output, 0644) == 0;
    char buffer[16384];
    while (ok) {
        ssize_t length = read(input, buffer, sizeof(buffer));
        if (length < 0 && errno == EINTR) continue;
        if (length < 0) { ok = 0; break; }
        if (!length) break;
        for (ssize_t written = 0; written < length;) {
            ssize_t amount = write(output, buffer + written, (size_t)(length - written));
            if (amount < 0 && errno == EINTR) continue;
            if (amount <= 0) { ok = 0; break; }
            written += amount;
        }
    }
    if (fsync(output) != 0) ok = 0;
    if (close(output) != 0) ok = 0;
    close(input);
    const char *name = strrchr(temporary, '/') + 1;
    if (ok) (void)syscall(SYS_renameat2, directory, name, directory, "neogeo.zip", 1 /* RENAME_NOREPLACE */);
    unlinkat(directory, name, 0);
    close(directory);
}

static enum mainui_launch_result builtin_launch(const char *root, int type, const char *rompath,
                                                const char *hostip, int hosting, const char *home,
                                                const char *advertisement, const struct mainui_encoder *encoder) {
    char command[3 * PATH_BYTES];
    char parent[PATH_BYTES];
    int other_card = !home && rompath && mainui_launch_other_platform_card(root);
    if (other_card) {
        const char *slash = strrchr(rompath, '/');
        if (!slash || slash == rompath) return MAINUI_LAUNCH_NO_TARGET;
        size_t length = (size_t)(slash - rompath);
        if (length >= sizeof(parent)) return MAINUI_LAUNCH_TOO_LONG;
        memcpy(parent, rompath, length);
        parent[length] = 0;
        home = parent;
    }
    enum mainui_launch_result result = builtin_command(command, sizeof(command), type, rompath, hostip, hosting, home);
    if (result != MAINUI_LAUNCH_OK) return result;
    if (!netplay_rom_exists(root, rompath)) return MAINUI_LAUNCH_NO_TARGET;
    int core_present = is_file(root, "/usr/trimui/bin/retroarch") && is_file(root, mainui_launch_builtin_core(type));
    unsigned char bytes[256]; int prepared = 0;
    if (encoder && encoder->block) {
        result = encoded_consumer(root);
        if (result != MAINUI_LAUNCH_OK) return result;
        // defemu_loadrom_internal passes the complete path for ARCADE/FC but
        // truncates NEOGEO's basename at its first literal ".zip" occurrence.
        char stem[256]; const char *payload = rompath;
        if (type == 6) {
            const char *base = strrchr(rompath, '/');
            const char *zip = base ? strstr(base + 1, ".zip") : NULL;
            if (!zip || zip == base + 1) return MAINUI_LAUNCH_NO_TARGET;
            size_t length = (size_t)(zip - base - 1);
            if (length >= sizeof(stem)) return MAINUI_LAUNCH_TOO_LONG;
            memcpy(stem, base + 1, length); stem[length] = 0; payload = stem;
        }
        result = encode_text(encoder, payload, bytes);
        if (result != MAINUI_LAUNCH_OK && result != MAINUI_LAUNCH_ENCODING_FAILED) return result;
        prepared = result == MAINUI_LAUNCH_OK;
        // Stock still writes its plain command after a failed ARE round trip.
        if (!prepared && !core_present) return MAINUI_LAUNCH_NO_LAUNCHER;
    } else if (!core_present) return MAINUI_LAUNCH_NO_LAUNCHER;
    if (type == 6 && other_card) builtin_neogeo_bios(root, home);
    result = hosting ? publish_host(root, command, advertisement) : publish(root, command);
    // Commit the current encoded companion last, retaining only this launch's
    // plain command/host advertisement. The init then takes its encoded branch.
    if (result == MAINUI_LAUNCH_OK && prepared) result = publish_encoded(root, bytes, 1);
    if (result != MAINUI_LAUNCH_OK && encoder && encoder->block) mainui_launch_clear(root);
    return result;
}

enum mainui_launch_result mainui_launch_builtin_netplay(const char *root, int type, const char *rompath,
                                                       const char *hostip, const char *home) {
    if (!hostip) return MAINUI_LAUNCH_NO_TARGET;
    return builtin_launch(root, type, rompath, hostip, 0, home, NULL, NULL);
}

enum mainui_launch_result mainui_launch_builtin_game(const char *root, int type, const char *rompath,
                                                    int hosting, const char *advertisement) {
    return builtin_launch(root, type, rompath, NULL, hosting, NULL, advertisement, NULL);
}

enum mainui_launch_result mainui_launch_builtin(const char *root, int type, const char *rompath,
                                               const char *hostip, int hosting, const char *advertisement,
                                               const struct mainui_encoder *encoder) {
    mainui_launch_clear(root);
    if (hostip && hosting) return MAINUI_LAUNCH_NO_TARGET;
    return builtin_launch(root, type, rompath, hostip, hosting, NULL, advertisement, encoder);
}

enum mainui_launch_result mainui_launch_host(const char *root, const char *launcher, const char *rompath,
                                            const char *advertisement) {
    char command[3 * PATH_BYTES];
    if (!is_file(root, launcher)) return MAINUI_LAUNCH_NO_LAUNCHER;
    if (!exists(root, rompath)) return MAINUI_LAUNCH_NO_TARGET;
    enum mainui_launch_result result = mainui_launch_host_command(command, sizeof(command), launcher, rompath);
    if (result != MAINUI_LAUNCH_OK) return result;
    return publish_host(root, command, advertisement);
}

enum mainui_launch_result mainui_launch_app(const char *root, const char *directory, const char *script) {
    char command[3 * PATH_BYTES];
    char full[PATH_BYTES];
    struct stat st;
    if (under(full, root, directory) < 0 || stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) return MAINUI_LAUNCH_NO_TARGET;
    char script_path[PATH_BYTES];
    if (script[0] == '/') snprintf(script_path, sizeof(script_path), "%s", script);
    else if (snprintf(script_path, sizeof(script_path), "%s/%s", directory, script) >= (int)sizeof(script_path))
        return MAINUI_LAUNCH_TOO_LONG;
    if (!is_file(root, script_path)) return MAINUI_LAUNCH_NO_LAUNCHER;
    enum mainui_launch_result result = mainui_launch_app_command(command, sizeof(command), directory, script);
    return result == MAINUI_LAUNCH_OK ? publish(root, command) : result;
}

// A GameSwitcher checkpoint's names as its store makes them: 32 lowercase hex digits, and the
// same digits with ".state".
static int hex32(const char *text) {
    for (int i = 0; i < 32; ++i)
        if (!text[i] || !strchr("0123456789abcdef", text[i])) return 0;
    return 1;
}

int mainui_launch_gameswitcher_names(const char *id, const char *state) {
    return id && state && strlen(id) == 32 && hex32(id) && strlen(state) == 38 && hex32(state) &&
           strcmp(state + 32, ".state") == 0;
}

enum mainui_launch_result mainui_launch_gameswitcher_resume(const char *root, const char *id, const char *state) {
    char command[256];
    if (!mainui_launch_gameswitcher_names(id, state)) return MAINUI_LAUNCH_NO_TARGET;
    if (!is_file(root, MAINUI_GAMESWITCHER_ENTRY)) return MAINUI_LAUNCH_NO_LAUNCHER;
    snprintf(command, sizeof(command), "exec sh '%s' resume %s %s", MAINUI_GAMESWITCHER_ENTRY, id, state);
    return publish(root, command);
}

enum mainui_launch_result mainui_launch_port(const char *root, const char *launch) {
    char command[3 * PATH_BYTES], script[PATH_BYTES];
    size_t n = 0;
    for (const char *p = launch; *p && n + 1 < sizeof(script); ++p) {
        if (*p == '\\' && p[1]) ++p;
        script[n++] = *p;
    }
    script[n] = 0;
    if (!is_file(root, script)) return MAINUI_LAUNCH_NO_LAUNCHER;
    enum mainui_launch_result result = mainui_launch_port_command(command, sizeof(command), launch);
    return result == MAINUI_LAUNCH_OK ? publish(root, command) : result;
}

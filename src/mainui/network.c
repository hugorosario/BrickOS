#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "network.h"

#include "json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ---- Netplay -------------------------------------------------------------------------------
struct host_parse { struct mainui_netplay_host *host; unsigned fields; };
static int host_member(void *context, const struct mainui_json_member *m) {
    struct host_parse *p = context;
    static const char *const keys[] = {"label", "iconpath", "rompath", "hostip", "gamelabel", "gametype"};
    char *values[] = {p->host->label, p->host->iconpath, p->host->rompath, p->host->hostip, p->host->gamelabel};
    const size_t sizes[] = {sizeof(p->host->label), sizeof(p->host->iconpath), sizeof(p->host->rompath),
                            sizeof(p->host->hostip), sizeof(p->host->gamelabel)};
    for (unsigned i = 0; i < 6; ++i) {
        if (strcmp(m->key, keys[i]) || (p->fields & (1u << i))) continue;
        if (i == 5) {
            if (m->type != MAINUI_JSON_NUMBER || m->number < 0 || m->number > 23 ||
                m->number != (int)m->number) return -1;
            p->host->gametype = (int)m->number;
        } else {
            if (m->type != MAINUI_JSON_STRING || m->text_length >= sizes[i] ||
                memchr(m->text, 0, m->text_length)) return -1;
            memcpy(values[i], m->text, m->text_length + 1);
        }
        p->fields |= 1u << i;
        break;
    }
    return 0;
}

int mainui_netplay_parse(const char *text, size_t length, struct mainui_netplay_host *out) {
    struct mainui_netplay_host h = {0};
    struct host_parse parse = {&h, 0};
    struct in_addr address;
    if (!length || length > 1024 || mainui_json_each(text, length, host_member, &parse) < 0 ||
        parse.fields != 63 || !h.label[0] || !h.rompath[0] || !h.gamelabel[0] ||
        inet_pton(AF_INET, h.hostip, &address) != 1) return -1;
    *out = h;
    return 0;
}

int mainui_netplay_format(const struct mainui_netplay_host *host, char *out, size_t size) {
    char escaped[5][1024];
    const char *values[] = {host->label, host->iconpath, host->rompath, host->hostip, host->gamelabel};
    for (int i = 0; i < 5; ++i)
        if (mainui_json_escape(escaped[i], sizeof(escaped[i]), values[i], strlen(values[i])) < 0) return -1;
    int n = snprintf(out, size, "{\"label\":%s,\"iconpath\":%s,\"rompath\":%s,\"hostip\":%s,\"gametype\":%d,\"gamelabel\":%s}",
                     escaped[0], escaped[1], escaped[2], escaped[3], host->gametype, escaped[4]);
    struct mainui_netplay_host check;
    return n > 0 && (size_t)n < size && mainui_netplay_parse(out, (size_t)n, &check) == 0 ? n : -1;
}

int mainui_netplay_address(const char *text, char *out, size_t size) {
    int found = 0;
    if (size) out[0] = 0;
    for (const char *p = text; (p = strstr(p, "inet ")); ++p) {
        if (p != text && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n') continue;
        const char *start = p + 5, *end = strchr(start, '/');
        char ip[16];
        struct in_addr address;
        if (!end || end - start >= (int)sizeof(ip) || end == start) continue;
        memcpy(ip, start, (size_t)(end - start)); ip[end - start] = 0;
        if (inet_pton(AF_INET, ip, &address) != 1 || strlen(ip) >= size) continue;
        snprintf(out, size, "%s", ip);
        found = 1;
    }
    return found ? 0 : -1;
}

int mainui_netplay_receive(struct mainui_netplay_host *hosts, int *count, const char *text,
                          size_t length, uint64_t now_ms) {
    struct mainui_netplay_host host;
    if (mainui_netplay_parse(text, length, &host) < 0) return 0;
    for (int i = 0; i < *count; ++i) {
        if (strcmp(hosts[i].label, host.label)) continue;
        hosts[i].seen_ms = now_ms;
        return 0;
    }
    if (*count >= MAINUI_NETPLAY_MAX) return 0;
    host.seen_ms = now_ms;
    hosts[(*count)++] = host;
    return 1;
}

int mainui_netplay_expire(struct mainui_netplay_host *hosts, int *count, uint64_t now_ms) {
    int kept = 0, before = *count;
    for (int i = 0; i < before; ++i)
        if (now_ms < hosts[i].seen_ms || now_ms - hosts[i].seen_ms <= 3000) hosts[kept++] = hosts[i];
    *count = kept;
    return before != kept;
}

int mainui_netplay_listen(unsigned port) {
    if (port > 65535) return -1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                  .sin_addr = {.s_addr = htonl(INADDR_ANY)}};
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static uint64_t cover_clock(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static int cover_cancelled(const int *cancel) {
    return cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE);
}

static int cover_inside(const char *path, const char *card) {
    size_t length = strlen(card);
    return !strncmp(path, card, length) && (path[length] == '/' || !path[length]);
}

int mainui_netplay_cover_path(const char *root, const char *iconpath, char *out, size_t size) {
    char device[1024], full[4096], parent[4096], card_path[4096], card[4096], actual[4096];
    if (size) out[0] = 0;
    if (!iconpath || !iconpath[0] || strlen(iconpath) >= 512) return -1;
    int n = snprintf(device, sizeof(device), "/mnt/SDCARD/%s", iconpath);
    if (n < 0 || (size_t)n >= size || snprintf(full, sizeof(full), "%s%s", root ? root : "", device) >= (int)sizeof(full) ||
        snprintf(card_path, sizeof(card_path), "%s/mnt/SDCARD", root ? root : "") >= (int)sizeof(card_path)) return -1;
    char *slash = strrchr(full, '/');
    if (!slash || !slash[1] || !strcmp(slash + 1, ".") || !strcmp(slash + 1, "..")) return -1;
    struct stat st;
    if (!realpath(card_path, card)) return -1;
    if (lstat(full, &st) == 0) {
        if (!realpath(full, actual) || !cover_inside(actual, card) || stat(actual, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    } else if (errno != ENOENT) return -1;
    *slash = 0;
    if (!realpath(full, parent) || !cover_inside(parent, card) ||
        stat(parent, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    strcpy(out, device);
    return 0;
}

int mainui_netplay_cover_local(const char *root, const char *iconpath, char *out, size_t size) {
    char device[1024], full[4096], actual[4096], card_path[4096], card[4096];
    if (size) out[0] = 0;
    if (mainui_netplay_cover_path(root, iconpath, device, sizeof(device)) < 0 ||
        snprintf(full, sizeof(full), "%s%s", root ? root : "", device) >= (int)sizeof(full) ||
        snprintf(card_path, sizeof(card_path), "%s/mnt/SDCARD", root ? root : "") >= (int)sizeof(card_path) ||
        strlen(device) >= size) return -1;
    struct stat st;
    if (!realpath(full, actual) || !realpath(card_path, card) || !cover_inside(actual, card) ||
        stat(actual, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    strcpy(out, device);
    return 0;
}

int mainui_netplay_cover_cache(const char *root, const char *iconpath, const char *source,
                              char *out, size_t size) {
    if (mainui_netplay_cover_local(root, iconpath, out, size) == 0) return 0;
    const char *extension = iconpath ? strrchr(iconpath, '.') : NULL;
    if (!extension || (strcasecmp(extension, ".png") && strcasecmp(extension, ".jpg") &&
        strcasecmp(extension, ".jpeg") && strcasecmp(extension, ".bmp") && strcasecmp(extension, ".gif") &&
        strcasecmp(extension, ".webp"))) return -1;
    char device[1024], full[4096], parent[4096], temporary[128], card_path[4096], card[4096];
    if (mainui_netplay_cover_path(root, iconpath, device, sizeof(device)) < 0 || strlen(device) >= size ||
        snprintf(full, sizeof(full), "%s%s", root ? root : "", device) >= (int)sizeof(full)) return -1;
    char *slash = strrchr(full, '/');
    const char *name = slash + 1;
    *slash = 0;
    if (snprintf(card_path, sizeof(card_path), "%s/mnt/SDCARD", root ? root : "") >= (int)sizeof(card_path) ||
        !realpath(card_path, card) || !realpath(full, parent) || !cover_inside(parent, card)) return -1;
    int input = source ? open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
    struct stat st;
    if (input < 0) return -1;
    if (fstat(input, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > MAINUI_NETPLAY_COVER_MAX) {
        close(input); return -1;
    }
    int directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) { close(input); return -1; }
    snprintf(temporary, sizeof(temporary), "/proc/self/fd/%d", directory);
    if (!realpath(temporary, parent) || !cover_inside(parent, card)) {
        close(directory); close(input); return -1;
    }
    // Bind creation and publication to the same opened directory, even if its
    // pathname changes while the completed download is copied.
    snprintf(temporary, sizeof(temporary), "/proc/self/fd/%d/.brickos-cover-XXXXXX", directory);
    int output = mkstemp(temporary);
    if (output < 0) { close(directory); close(input); return -1; }
    int ok = fcntl(output, F_SETFD, FD_CLOEXEC) == 0 && fchmod(output, 0644) == 0;
    size_t bytes = 0;
    char buffer[16384];
    while (ok) {
        ssize_t amount = read(input, buffer, sizeof(buffer));
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0 || bytes + (size_t)amount > MAINUI_NETPLAY_COVER_MAX) { ok = 0; break; }
        if (!amount) break;
        for (size_t written = 0; written < (size_t)amount;) {
            ssize_t got = write(output, buffer + written, (size_t)amount - written);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) { ok = 0; break; }
            written += (size_t)got;
        }
        bytes += (size_t)amount;
    }
    if (!bytes || fsync(output) != 0) ok = 0;
    if (close(output) != 0) ok = 0;
    close(input);
    // The destination never replaces a file or link. Use Linux's no-replace rename;
    // unlike a hard-link publication this can work on the target's FAT card.
    const char *temporary_name = strrchr(temporary, '/') + 1;
    if (ok && syscall(SYS_renameat2, directory, temporary_name, directory, name, 1 /* RENAME_NOREPLACE */) != 0) ok = 0;
    unlinkat(directory, temporary_name, 0);
    close(directory);
    if (!ok) return mainui_netplay_cover_local(root, iconpath, out, size);
    strcpy(out, device);
    return 0;
}

int mainui_netplay_cover(const char *ip, const char *iconpath, unsigned port, const char *path,
                         unsigned timeout_ms, const int *cancel) {
    struct in_addr address;
    if (!ip || inet_pton(AF_INET, ip, &address) != 1 || !iconpath || !iconpath[0] ||
        strlen(iconpath) >= 512 || !port || port > 65535 || !timeout_ms || timeout_ms > 10000 ||
        !path || !path[0] || cover_cancelled(cancel)) return -1;
    char url[1600], temporary[4096];
    int prefix = snprintf(url, sizeof(url), "http://%s:%u/", ip, port);
    size_t n = (size_t)prefix;
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)iconpath; *p; ++p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            strchr("/._~-", *p)) url[n++] = (char)*p;
        else { url[n++] = '%'; url[n++] = hex[*p >> 4]; url[n++] = hex[*p & 15]; }
    }
    url[n] = 0;
    if (snprintf(temporary, sizeof(temporary), "%s.part", path) >= (int)sizeof(temporary)) return -1;
    int file = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file < 0) return -1;
    int descriptors[2], result = -1;
    if (pipe(descriptors) != 0) { close(file); unlink(temporary); return -1; }
    if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(descriptors[0], F_SETFL, O_NONBLOCK) < 0) {
        close(descriptors[0]); close(descriptors[1]); close(file); unlink(temporary); return -1;
    }
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    int actions_ready = error == 0;
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, descriptors[0]);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, descriptors[1]);
    if (!error) error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char *argv[] = {"wget", "-q", "-T", "2", "-O", "-", url, NULL};
    char *env[] = {"PATH=/usr/bin:/bin", "LD_LIBRARY_PATH=/usr/trimui/lib:/usr/lib", NULL};
    pid_t child = -1;
    uint64_t deadline = cover_clock() + timeout_ms;
    if (!error) error = posix_spawn(&child, "/usr/bin/wget", &actions, NULL, argv, env);
    if (actions_ready) posix_spawn_file_actions_destroy(&actions);
    close(descriptors[1]);
    size_t bytes = 0;
    int eof = 0, status = 0, reaped = 0;
    while (!error && !eof && cover_clock() < deadline && !cover_cancelled(cancel)) {
        struct pollfd readfd = {.fd = descriptors[0], .events = POLLIN};
        int ready = poll(&readfd, 1, 20);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { error = 1; break; }
        if (!ready) continue;
        char buffer[16384];
        ssize_t count = read(descriptors[0], buffer, sizeof(buffer));
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (count < 0) { error = 1; break; }
        if (!count) { eof = 1; break; }
        if (bytes + (size_t)count > MAINUI_NETPLAY_COVER_MAX) { error = 1; break; }
        size_t written = 0;
        while (written < (size_t)count) {
            ssize_t amount = write(file, buffer + written, (size_t)count - written);
            if (amount < 0 && errno == EINTR) continue;
            if (amount <= 0) { error = 1; break; }
            written += (size_t)amount;
        }
        bytes += written;
    }
    close(descriptors[0]);
    while (!error && eof && child > 0 && cover_clock() < deadline && !cover_cancelled(cancel)) {
        pid_t got = waitpid(child, &status, WNOHANG);
        if (got == child) { reaped = 1; break; }
        if (got < 0 && errno != EINTR) { error = reaped = 1; break; }
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    if (child > 0 && !reaped) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    int ok = !error && eof && reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0 && bytes &&
             !cover_cancelled(cancel) && fsync(file) == 0;
    if (close(file) != 0) ok = 0;
    if (ok && rename(temporary, path) == 0) result = 0;
    if (result != 0) unlink(temporary);
    return result;
}

// ---- Wi-Fi ---------------------------------------------------------------------------------

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void mainui_wifi_unescape(char *ssid) {
    char *out = ssid;
    for (const char *p = ssid; *p;) {
        if (p[0] == '\\' && p[1] == 'x' && hex_digit(p[2]) >= 0 && hex_digit(p[3]) >= 0) {
            *out++ = (char)(hex_digit(p[2]) * 16 + hex_digit(p[3]));
            p += 4;
        } else if (p[0] == '\\' && (p[1] == '\\' || p[1] == '"')) {
            *out++ = p[1];
            p += 2;
        } else {
            *out++ = *p++;
        }
    }
    *out = 0;
}

static void copy_field(char *out, size_t size, const char *start, size_t length) {
    if (length >= size) length = size - 1;
    memcpy(out, start, length);
    out[length] = 0;
}

static int compare_signal(const void *a, const void *b) {
    const struct mainui_wifi_network *x = a, *y = b;
    return y->rssi - x->rssi;
}

int mainui_wifi_parse_scan(const char *text, struct mainui_wifi_network *out, int max) {
    int n = 0;
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        // bssid \t frequency \t signal level \t flags \t ssid; the header has no tabs.
        const char *fields[5];
        size_t lengths[5];
        int count = 0;
        const char *p = line, *stop = line + length;
        while (count < 5 && p <= stop) {
            const char *tab = memchr(p, '\t', (size_t)(stop - p));
            if (count == 4 || !tab) tab = stop;
            fields[count] = p;
            lengths[count] = (size_t)(tab - p);
            ++count;
            p = tab + 1;
        }
        if (count == 5 && lengths[4] && fields[4][lengths[4] - 1] == '\r') --lengths[4];
        if (count == 5 && lengths[4] > 0 && n < max) {
            struct mainui_wifi_network net;
            memset(&net, 0, sizeof(net));
            copy_field(net.ssid, sizeof(net.ssid), fields[4], lengths[4]);
            mainui_wifi_unescape(net.ssid);
            char number[16];
            copy_field(number, sizeof(number), fields[2], lengths[2]);
            net.rssi = atoi(number);
            char flags[256];
            copy_field(flags, sizeof(flags), fields[3], lengths[3]);
            net.locked = strstr(flags, "WPA") || strstr(flags, "WEP") || strstr(flags, "RSN");
            net.saved_id = -1;
            int existing = -1;
            for (int i = 0; i < n; ++i)
                if (strcmp(out[i].ssid, net.ssid) == 0) existing = i;
            if (existing < 0) out[n++] = net;
            else if (net.rssi > out[existing].rssi) out[existing].rssi = net.rssi;
        }
        line = end ? end + 1 : NULL;
    }
    qsort(out, (size_t)n, sizeof(out[0]), compare_signal);
    return n;
}

void mainui_wifi_parse_status(const char *text, struct mainui_wifi_status *status) {
    memset(status, 0, sizeof(*status));
    status->id = -1;
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length && line[length - 1] == '\r') --length;
        const char *equals = memchr(line, '=', length);
        if (equals) {
            size_t key = (size_t)(equals - line), value_length = length - key - 1;
            const char *value = equals + 1;
            if (key == 9 && strncmp(line, "wpa_state", 9) == 0) copy_field(status->state, sizeof(status->state), value, value_length);
            else if (key == 4 && strncmp(line, "ssid", 4) == 0) {
                copy_field(status->ssid, sizeof(status->ssid), value, value_length);
                mainui_wifi_unescape(status->ssid);
            } else if (key == 10 && strncmp(line, "ip_address", 10) == 0) copy_field(status->ip, sizeof(status->ip), value, value_length);
            else if (key == 2 && strncmp(line, "id", 2) == 0) status->id = atoi(value);
        }
        line = end ? end + 1 : NULL;
    }
}

int mainui_wifi_saved_id(const char *text, const char *ssid) {
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        // network id \t ssid \t bssid \t flags
        const char *tab = memchr(line, '\t', length);
        if (tab) {
            const char *second = memchr(tab + 1, '\t', length - (size_t)(tab + 1 - line));
            size_t ssid_length = second ? (size_t)(second - tab - 1) : length - (size_t)(tab + 1 - line);
            char name[MAINUI_SSID_MAX];
            copy_field(name, sizeof(name), tab + 1, ssid_length);
            mainui_wifi_unescape(name);
            if (line[0] >= '0' && line[0] <= '9' && strcmp(name, ssid) == 0) return atoi(line);
        }
        line = end ? end + 1 : NULL;
    }
    return -1;
}

void mainui_wifi_mark(struct mainui_wifi_network *networks, int count, const char *list_text, const struct mainui_wifi_status *status) {
    int completed = strcmp(status->state, "COMPLETED") == 0;
    for (int i = 0; i < count; ++i) {
        networks[i].saved_id = list_text ? mainui_wifi_saved_id(list_text, networks[i].ssid) : -1;
        networks[i].connected = completed && strcmp(status->ssid, networks[i].ssid) == 0;
    }
}

// Static: WifiMenu picks ic-setting-wifi-01 for a value out of -126..127, and otherwise 02 below
// -80 dBm for a network not in use, 02 from -80 to -71 for the one in use, and 03 for the rest.
int mainui_wifi_level(int rssi, int connected) {
    if (rssi < -126 || rssi > 127) return 1;
    if (connected) return rssi >= -80 && rssi <= -71 ? 2 : 3;
    return rssi < -80 ? 2 : 3;
}

// Static: draw_nav_bar draws ic-wifi-03 from -70 dBm, 02 from -80, and 01 below or out of range.
int mainui_wifi_bar_level(int rssi) {
    if (rssi < -126 || rssi > 127 || rssi < -80) return 1;
    return rssi >= -70 ? 3 : 2;
}

int mainui_wifi_parse_signal(const char *text) {
    const char *line = strstr(text, "RSSI=");
    // AVG_RSSI= also holds "RSSI=": the first line is RSSI itself.
    while (line && line != text && line[-1] != '\n') line = strstr(line + 1, "RSSI=");
    return line ? atoi(line + 5) : MAINUI_RSSI_NONE;
}

int mainui_shell_quote(char *out, size_t size, const char *text) {
    size_t n = strlen(out);
#define PUT(c) do { if (n + 1 >= size) return -1; out[n++] = (c); } while (0)
    PUT('\'');
    for (const char *p = text; *p; ++p) {
        if (*p == '\'') { PUT('\''); PUT('\\'); PUT('\''); PUT('\''); }
        else PUT(*p);
    }
    PUT('\'');
#undef PUT
    out[n] = 0;
    return 0;
}

// wpa_cli set_network takes a quoted string value: '"name"' in the official command line.
static int set_network_line(char *line, size_t size, int id, const char *field, const char *value) {
    char quoted[MAINUI_SSID_MAX * 2 + 8];
    snprintf(line, size, MAINUI_WPA_CLI " set_network %d %s ", id, field);
    if (strlen(value) + 3 > sizeof(quoted)) return -1;
    snprintf(quoted, sizeof(quoted), "\"%s\"", value);
    return mainui_shell_quote(line, size, quoted);
}

int mainui_wifi_connect_commands(const char *ssid, const char *password, int id, char lines[][512], int max) {
    int n = 0;
    if (id < 0 || max < 5) return -1;
    if (set_network_line(lines[n++], 512, id, "ssid", ssid) < 0) return -1;
    if (password) {
        if (set_network_line(lines[n++], 512, id, "psk", password) < 0) return -1;
    } else {
        snprintf(lines[n++], 512, MAINUI_WPA_CLI " set_network %d key_mgmt NONE", id);
    }
    snprintf(lines[n++], 512, MAINUI_WPA_CLI " select_network %d", id);
    snprintf(lines[n++], 512, MAINUI_WPA_CLI " enable_network %d", id);
    snprintf(lines[n++], 512, MAINUI_WPA_CLI " save_config");
    return n;
}

static int network_id_commands(const char *verb, int id, char lines[][512], int max) {
    if (id < 0 || max < 2) return -1;
    snprintf(lines[0], 512, MAINUI_WPA_CLI " %s %d", verb, id);
    snprintf(lines[1], 512, MAINUI_WPA_CLI " save_config");
    return 2;
}

int mainui_wifi_reconnect_commands(int id, char lines[][512], int max) { return network_id_commands("select_network", id, lines, max); }
int mainui_wifi_forget_commands(int id, char lines[][512], int max) { return network_id_commands("remove_network", id, lines, max); }

// ---- Hotspot -------------------------------------------------------------------------------

int mainui_hotspot_commands(int on, int wifi_on, const char **lines, int max) {
    static const char *const ap[] = {
        "ifconfig wlan0 down", "killall -15 wpa_supplicant", "killall -9 udhcpc", "killall  -9 udhcpd",
        "killall  -15 hostapd", "ifconfig wlan0 192.168.0.18 up", "hostapd /etc/wifi/hostapd.conf -B",
        "udhcpd /etc/wifi/udhcpd.conf -f &",
    };
    static const char *const client[] = {
        "killall  -9 udhcpd", "killall  -15 hostapd", "ifconfig wlan0 down", "ifconfig wlan0 up",
        "wpa_supplicant -B -D nl80211 -iwlan0 -c /etc/wifi/wpa_supplicant.conf -O /etc/wifi/sockets", "udhcpc -i wlan0 &",
    };
    static const char *const off[] = { "killall  -9 udhcpd", "killall  -15 hostapd" };
    const char *const *from = on ? ap : wifi_on ? client : off;
    int count = on ? 8 : wifi_on ? 6 : 2;
    int n = 0;
    for (int i = 0; i < count && n < max; ++i) lines[n++] = from[i];
    return n;
}

static int starts_key(const char *line, size_t length, const char *key) {
    size_t k = strlen(key);
    return length > k && strncmp(line, key, k) == 0 && line[k] == '=';
}

int mainui_hostapd_edit(const char *text, const char *key, const char *value, char *out, size_t size) {
    static const char *const security[] = { "wpa", "wpa_key_mgmt", "wpa_pairwise", "rsn_pairwise", "wpa_passphrase" };
    size_t n = 0;
    int password = strcmp(key, "wpa_passphrase") == 0, open = password && !value[0], replaced = 0;
    char added[256];
#define ADD(s, l) do { if (n + (l) + 1 >= size) return -1; memcpy(out + n, (s), (l)); n += (l); } while (0)
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        int drop = starts_key(line, length, key);
        // A new password rewrites the whole security block (static: the official deletes the
        // lines and echoes wpa=3, WPA-PSK, TKIP, CCMP); an empty one leaves an open network.
        for (int i = 0; password && i < 5; ++i) drop |= starts_key(line, length, security[i]);
        if (!drop) {
            ADD(line, length);
            ADD("\n", 1);
        } else if (!password && !replaced) {
            // Another key keeps its line's place.
            int l = snprintf(added, sizeof(added), "%s=%s\n", key, value);
            if (l < 0 || (size_t)l >= sizeof(added)) return -1;
            ADD(added, (size_t)l);
            replaced = 1;
        }
        line = end ? end + 1 : NULL;
    }
    if (!password && !replaced) {
        int l = snprintf(added, sizeof(added), "%s=%s\n", key, value);
        if (l < 0 || (size_t)l >= sizeof(added)) return -1;
        ADD(added, (size_t)l);
    } else if (password && !open) {
        int l = snprintf(added, sizeof(added), "wpa=3\nwpa_key_mgmt=WPA-PSK\nwpa_pairwise=TKIP\nrsn_pairwise=CCMP\nwpa_passphrase=%s\n", value);
        if (l < 0 || (size_t)l >= sizeof(added)) return -1;
        ADD(added, (size_t)l);
    }
#undef ADD
    out[n] = 0;
    return (int)n;
}

// ---- Bluetooth -----------------------------------------------------------------------------

static void json_text(const struct mainui_json *json, const char *key, char *out, size_t size) {
    const struct mainui_json_member *m = mainui_json_get(json, key);
    out[0] = 0;
    if (m && m->type == MAINUI_JSON_STRING) snprintf(out, size, "%.*s", (int)(size - 1), m->text);
}

static int parse_device(const char *text, size_t length, struct mainui_bt_device *device) {
    struct mainui_json json;
    if (mainui_json_parse(&json, text, length) < 0) return -1;
    memset(device, 0, sizeof(*device));
    json_text(&json, "addr", device->addr, sizeof(device->addr));
    json_text(&json, "name", device->name, sizeof(device->name));
    json_text(&json, "icon", device->icon, sizeof(device->icon));
    // Open: the type of "connected" (bluetoothctl info prints yes or no).
    const struct mainui_json_member *connected = mainui_json_get(&json, "connected");
    if (connected)
        device->connected = connected->type == MAINUI_JSON_NUMBER ? connected->number != 0
                                                                   : strcmp(connected->text, "true") == 0 || strcmp(connected->text, "yes") == 0 ||
                                                                         strcmp(connected->text, "1") == 0;
    return device->addr[0] ? 0 : -1;
}

struct device_list {
    struct mainui_bt_device *devices;
    int count, max;
};

static int add_device(void *context, const char *text, size_t length) {
    struct device_list *list = context;
    if (list->count < list->max && parse_device(text, length, &list->devices[list->count]) == 0) ++list->count;
    return 0;
}

// A short member of an object of any size: a string with \" and \\ decoded, or the source text
// of a number or literal (d1: "result":1); "" when absent.
static void find_text(const char *text, size_t size, const char *key, char *out, size_t out_size) {
    const char *value;
    size_t length, n = 0;
    out[0] = 0;
    if (mainui_json_find(text, size, key, &value, &length) < 0 || length == 0) return;
    if (value[0] != '"') {
        snprintf(out, out_size, "%.*s", (int)length, value);
        return;
    }
    for (size_t i = 1; i + 1 < length && n + 1 < out_size; ++i) {
        if (value[i] == '\\' && i + 2 < length) ++i;
        out[n++] = value[i];
    }
    out[n] = 0;
}

int mainui_bt_parse(const char *text, struct mainui_bt_answer *answer, struct mainui_bt_device *devices, int max) {
    const char *value;
    size_t length, size = strlen(text);
    memset(answer, 0, sizeof(*answer));
    struct device_list list = { devices, 0, max };
    while (size && (text[size - 1] == '\n' || text[size - 1] == '\r' || text[size - 1] == ' ')) --size;
    if (!size || text[0] != '{' || text[size - 1] != '}') return -1;
    find_text(text, size, "func", answer->func, sizeof(answer->func));
    find_text(text, size, "result", answer->result, sizeof(answer->result));
    find_text(text, size, "error", answer->error, sizeof(answer->error));
    if (mainui_json_find(text, size, "devices", &value, &length) == 0) mainui_json_array_each(value, length, add_device, &list);
    else if (max > 0 && parse_device(text, size, &devices[0]) == 0) list.count = 1;
    return list.count;
}

int mainui_bt_next_object(const char *data, size_t length, size_t *start, size_t *end) {
    const char *open = memchr(data, '{', length);
    if (!open) return 0;
    int depth = 0, quoted = 0;
    for (size_t i = (size_t)(open - data); i < length; ++i) {
        char c = data[i];
        if (quoted) {
            if (c == '\\') ++i;
            else if (c == '"') quoted = 0;
        } else if (c == '"') {
            quoted = 1;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            *start = (size_t)(open - data);
            *end = i + 1;
            return 1;
        }
    }
    return 0;
}

const char *mainui_bt_icon(const struct mainui_bt_device *device) {
    // Static: BTDevice::draw picks the headphones for "audio", the plain icon for "input", and
    // the gamepad otherwise; the scan in session a3 reported empty icons, drawn as gamepads (w1).
    if (strstr(device->icon, "audio")) return "icon-bluetooth-headphone.png";
    if (strstr(device->icon, "input")) return "icon-bluetooth.png";
    return "icon-bluetooth-gamepad.png";
}

// ---- Clock ---------------------------------------------------------------------------------

static int compare_zone(const void *a, const void *b) {
    const struct mainui_zone *x = a, *y = b;
    if (x->folder != y->folder) return y->folder - x->folder;
    return strcmp(x->name, y->name);
}

void mainui_zone_sort(struct mainui_zone *zones, int count) {
    qsort(zones, (size_t)count, sizeof(zones[0]), compare_zone);
}

int mainui_zone_command(const char *zone, char *out, size_t size) {
    out[0] = 0;
    // Default has no command: the link the boot script made stays (static: MainUI's start only
    // relinks a stored zone). A zone name comes from the folder listing.
    if (!zone[0] || zone[0] == '/' || strchr(zone, '\'') || strstr(zone, "..")) return -1;
    snprintf(out, size, "rm /tmp/localtime; ln -sf ");
    char path[256];
    snprintf(path, sizeof(path), MAINUI_ZONEINFO "/%s", zone);
    if (mainui_shell_quote(out, size, path) < 0) return -1;
    size_t n = strlen(out);
    if (n + 16 >= size) return -1;
    memcpy(out + n, " /tmp/localtime", 16);
    return 0;
}

int mainui_date_command(int year, int month, int day, int hour, int minute, int second, char *out, size_t size) {
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
        second > 59)
        return -1;
    int n = snprintf(out, size, "date -s \"%04d-%02d-%02d %02d:%02d:%02d\" && hwclock -w", year, month, day, hour, minute, second);
    return n > 0 && (size_t)n < size ? 0 : -1;
}

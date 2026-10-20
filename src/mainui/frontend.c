#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700  // sync()
#include "frontend.h"
#include "input.h"
#include "json.h"
#include "launch.h"
#include "sound.h"
#include "brickos_return.h"
#include "../inputd/config.h"
#include "../ledd/config.h"
#include "../ledd/health.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PATH_BYTES 1100
#define STATE_BYTES ((size_t)MAINUI_STACK_MAX * 12 * 1024 + 8 * 1024)
#define POSITION_BYTES ((size_t)MAINUI_POSITIONS_MAX * (1024 * 6 + 128) + 3)

static void path_under(const struct mainui_frontend *fe, char *out, const char *path) {
    size_t root = strlen(fe->fs.root), length = strlen(path);
    if (root + length >= PATH_BYTES) { out[0] = 0; return; }
    memcpy(out, fe->fs.root, root);
    memcpy(out + root, path, length + 1);
}

static int led_read(struct mainui_frontend *fe);
static void appearance_open(struct mainui_frontend *fe);
static void appearance_save(struct mainui_frontend *fe);
static int appearance_tick(struct mainui_frontend *fe, uint64_t now_ms);
static void appearance_fonts(struct mainui_frontend *fe);
static void carry_out(struct mainui_frontend *fe, const struct mainui_effect *e);
static int boot_rotation_tick(struct mainui_frontend *fe);

// Appends one line to the effect record.
static void note(struct mainui_frontend *fe, const char *format, ...) {
    if (!fe->record) return;
    va_list args;
    va_start(args, format);
    vfprintf(fe->record, format, args);
    va_end(args);
    fputc('\n', fe->record);
    fflush(fe->record);
}

// ---- Loading -------------------------------------------------------------------------------

static void refresh_lists(struct mainui_frontend *fe) {
    fe->recent_count = (int)mainui_recent_visible(&fe->recent, fe->recent_shown, 64);
    fe->favorites_count = (int)mainui_favorites_visible(&fe->favorites, fe->favorites_shown, 512);
}

static void load_lists(struct mainui_frontend *fe) {
    char path[PATH_BYTES];
    mainui_list_free(&fe->recent);
    mainui_list_free(&fe->favorites);
    path_under(fe, path, "/mnt/SDCARD/Roms/recentlist.json");
    mainui_list_load(&fe->recent, path);
    path_under(fe, path, "/mnt/SDCARD/Roms/favourite2.json");
    mainui_list_load(&fe->favorites, path);
    refresh_lists(fe);
}

static void drop_games(struct mainui_frontend *fe) {
    free(fe->games);
    fe->games = NULL;
    fe->game_count = 0;
    fe->games_system = -1;
    fe->games_path[0] = 0;
}

// ---- Game counts ---------------------------------------------------------------------------
// The official grid shows a system's count only once it has a list cache, that is after the
// system was opened (k2i). BrickOS counts every shown system (a stock defect fixed). The
// counting thread reads the systems and the shown list, which only a catalog reload changes, and
// it stops before one. A count stored by a load of the games wins over the thread's.

static void count_system(struct mainui_frontend *fe, int system) {
    if (__atomic_load_n(&fe->game_counts[system], __ATOMIC_ACQUIRE) >= 0) return;
    struct mainui_game *games;
    int n = mainui_catalog_games(&fe->fs, &fe->systems[system], &games);
    free(games);
    int expected = -1;
    if (__atomic_compare_exchange_n(&fe->game_counts[system], &expected, n > 0 ? n : 0, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
        __atomic_store_n(&fe->counts_changed, 1, __ATOMIC_RELEASE);
}

static void *count_thread(void *arg) {
    struct mainui_frontend *fe = arg;
    for (int v = 0; v < fe->visible_count && !__atomic_load_n(&fe->count_stop, __ATOMIC_ACQUIRE); ++v)
        count_system(fe, fe->visible[v]);
    return NULL;
}

static void stop_counting(struct mainui_frontend *fe) {
    if (!fe->counter_running) return;
    __atomic_store_n(&fe->count_stop, 1, __ATOMIC_RELEASE);
    pthread_join(fe->counter, NULL);
    fe->counter_running = 0;
    fe->count_stop = 0;
}

static void start_counting(struct mainui_frontend *fe) {
    if (!fe->count_mode || !fe->game_counts) return;
    if (fe->count_mode == 2 && pthread_create(&fe->counter, NULL, count_thread, fe) == 0) {
        fe->counter_running = 1;
        return;
    }
    for (int v = 0; v < fe->visible_count; ++v) count_system(fe, fe->visible[v]);
}

void mainui_frontend_count_games(struct mainui_frontend *fe, int background) {
    stop_counting(fe);
    fe->count_mode = background ? 2 : 1;
    start_counting(fe);
}

int mainui_frontend_game_count(const struct mainui_frontend *fe, int visible_system) {
    if (visible_system >= MAINUI_BEST_SYSTEM_BASE) {
        int best = visible_system - MAINUI_BEST_SYSTEM_BASE;
        return best < fe->shown_best_count ? fe->best_game_counts[best] : -1;
    }
    if (!fe->game_counts || visible_system < 0 || visible_system >= fe->visible_count) return -1;
    return __atomic_load_n(&fe->game_counts[fe->visible[visible_system]], __ATOMIC_ACQUIRE);
}

// GameSwitcher's app, where its installer puts it and its rollback removes it. Settings >
// GameSwitcher turns GameSwitcher off with GAMESWITCHER_OFF: Apps then leaves the app out and the
// BrickOS desktop has no card. The switch is read with the catalog and each second after.
#define GAMESWITCHER_APP "/mnt/SDCARD/Apps/GameSwitcher"
#define GAMESWITCHER_OFF "/mnt/SDCARD/BrickOS/gameswitcher/OFF"

static int is_gameswitcher(const struct mainui_app *app) {
    return !app->stock && strcmp(app->directory, GAMESWITCHER_APP) == 0;
}

static int gameswitcher_off(const struct mainui_frontend *fe) {
    char path[PATH_BYTES];
    struct stat st;
    path_under(fe, path, GAMESWITCHER_OFF);
    return path[0] && stat(path, &st) == 0;
}

static void show_apps(struct mainui_frontend *fe) {
    fe->shown_app_count = 0;
    for (int i = 0; i < fe->app_count && fe->shown_app_count < 256; ++i)
        if (fe->apps[i].show && !(fe->gameswitcher_off && is_gameswitcher(&fe->apps[i])))
            fe->shown_apps[fe->shown_app_count++] = i;
}

static int load_catalog(struct mainui_frontend *fe) {
    stop_counting(fe);
    drop_games(fe);
    free(fe->systems);
    free(fe->apps);
    free(fe->ports);
    free(fe->bests);
    free(fe->game_counts);
    fe->systems = NULL;
    fe->apps = NULL;
    fe->ports = NULL;
    fe->bests = NULL;
    fe->game_counts = NULL;
    fe->system_count = mainui_catalog_systems(&fe->fs, &fe->systems);
    if (fe->system_count < 0) { fe->system_count = 0; return -1; }
    fe->game_counts = malloc(sizeof(*fe->game_counts) * (size_t)(fe->system_count ? fe->system_count : 1));
    for (int i = 0; fe->game_counts && i < fe->system_count; ++i) fe->game_counts[i] = -1;
    fe->visible_count = 0;
    for (int i = 0; i < fe->system_count && fe->visible_count < 128; ++i)
        if (fe->systems[i].show) fe->visible[fe->visible_count++] = i;
    fe->app_count = mainui_catalog_apps(&fe->fs, &fe->apps);
    if (fe->app_count < 0) fe->app_count = 0;
    fe->gameswitcher_off = gameswitcher_off(fe);
    show_apps(fe);
    fe->port_count = mainui_catalog_ports(&fe->fs, &fe->ports);
    if (fe->port_count < 0) fe->port_count = 0;
    fe->shown_port_count = 0;
    for (int i = 0; i < fe->port_count && fe->shown_port_count < 256; ++i)
        if (fe->ports[i].show) fe->shown_ports[fe->shown_port_count++] = i;
    fe->best_count = mainui_catalog_bests(&fe->fs, &fe->bests);
    if (fe->best_count < 0) fe->best_count = 0;
    fe->shown_best_count = 0;
    for (int i = 0; i < fe->best_count && fe->shown_best_count < 128; ++i)
        if (fe->bests[i].show) {
            fe->shown_bests[fe->shown_best_count] = i;
            fe->best_game_counts[fe->shown_best_count++] = -1;
        }
    start_counting(fe);
    return 0;
}

static void load_language(struct mainui_frontend *fe) {
    char path[PATH_BYTES], full[PATH_BYTES];
    mainui_lang_free(&fe->lang);
    if (mainui_asset_language(&fe->fs, fe->settings.language, path, sizeof(path)) < 0) return;
    path_under(fe, full, path);
    mainui_lang_load(&fe->lang, full);
}

// Copies s into out, cut to fit.
static void copy_value(char *out, size_t size, const char *s) {
    size_t n = strlen(s);
    if (n >= size) n = size - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

// The language list's pictures and names: each .lang file names its flag-and-name image (e13b)
// and holds its language's own name under "lang", which the BrickOS list shows.
static void load_language_icons(struct mainui_frontend *fe) {
    char path[PATH_BYTES], full[PATH_BYTES];
    for (int i = 0; i < fe->language_count; ++i) {
        struct mainui_lang lang;
        memset(&lang, 0, sizeof(lang));
        fe->language_icons[i][0] = 0;
        fe->language_names[i][0] = 0;
        if (mainui_asset_language(&fe->fs, fe->languages[i], path, sizeof(path)) < 0) continue;
        path_under(fe, full, path);
        if (mainui_lang_load(&lang, full) >= 0) {
            copy_value(fe->language_icons[i], sizeof(fe->language_icons[i]), lang.icon);
            copy_value(fe->language_names[i], sizeof(fe->language_names[i]), lang.name);
            static const int label_ids[MAINUI_NETPLAY_BUILTIN_LABELS] = {3, 4, 5, 6, 7, 9, 8, 11, 13, 12};
            for (int core = 0; core < MAINUI_NETPLAY_BUILTIN_LABELS; ++core) {
                const char *label = mainui_lang_get(&lang, label_ids[core]);
                if (label && strlen(label) < sizeof(fe->netplay_builtin_labels[i][core]))
                    strcpy(fe->netplay_builtin_labels[i][core], label);
            }
        }
        mainui_lang_free(&lang);
    }
}

// The first line of a file under the root, or "".
static void read_line(const struct mainui_frontend *fe, const char *device_path, char *out, size_t size) {
    char path[PATH_BYTES];
    path_under(fe, path, device_path);
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)size, f)) out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}

// Device Info values from the sources the official screen reads (static: /etc/version and the
// kernel release and build date from /proc/version). The specified five-row
// screen omits the stock authentication-chip serial.
static void load_device_info(struct mainui_frontend *fe) {
    char line[512], value[600];
    copy_value(fe->device_info[MAINUI_INFO_MODEL_NAME], sizeof(fe->device_info[0]), "Trimui Brick");
    copy_value(fe->device_info[MAINUI_INFO_MODEL_NUMBER], sizeof(fe->device_info[0]), "TG3040");
    read_line(fe, "/etc/version", fe->device_info[MAINUI_INFO_VERSION], sizeof(fe->device_info[0]));
    // UI Version: the frontend's build date as 20YYMMDD, read from __DATE__ ("Mmm dd yyyy") as the
    // official one reads its own (static: "20%02d%02d%02d" from the year's last two digits).
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *date = __DATE__;
    char month_name[4] = { date[0], date[1], date[2], 0 };
    const char *month = strstr(months, month_name);
    snprintf(fe->device_info[MAINUI_INFO_UI_VERSION], sizeof(fe->device_info[0]), "20%02d%02d%02d", atoi(date + 9),
             month ? (int)(month - months) / 3 + 1 : 0, atoi(date + 4));
    read_line(fe, "/proc/version", line, sizeof(line));
    int major, minor, patch;
    const char *release = strstr(line, "version ");
    const char *built = strstr(line, "SMP PREEMPT");
    if (release && sscanf(release + 8, "%d.%d.%d", &major, &minor, &patch) == 3) {
        snprintf(value, sizeof(value), "%d.%d.%d %s", major, minor, patch, built ? built + strlen("SMP PREEMPT") : "");
        copy_value(fe->device_info[MAINUI_INFO_LINUX_VERSION], sizeof(fe->device_info[0]), value);
    }
}

// The hotspot name and password the Hotspot screen shows, from the access point configuration
// the official screen reads (static: /etc/wifi/hostapd.conf).
static void load_hotspot(struct mainui_frontend *fe) {
    char path[PATH_BYTES], line[256];
    path_under(fe, path, "/etc/wifi/hostapd.conf");
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "ssid=", 5) == 0) copy_value(fe->hotspot_ssid, sizeof(fe->hotspot_ssid), line + 5);
        else if (strncmp(line, "wpa_passphrase=", 15) == 0) copy_value(fe->hotspot_password, sizeof(fe->hotspot_password), line + 15);
    }
    fclose(f);
}

const struct mainui_system *mainui_frontend_system(const struct mainui_frontend *fe, int visible_system) {
    if (visible_system >= MAINUI_BEST_SYSTEM_BASE) {
        int best = visible_system - MAINUI_BEST_SYSTEM_BASE;
        return best < fe->shown_best_count ? &fe->bests[fe->shown_bests[best]] : NULL;
    }
    return visible_system >= 0 && visible_system < fe->visible_count ? &fe->systems[fe->visible[visible_system]] : NULL;
}

static int rom_path_valid(const struct mainui_frontend *fe, int system, const char *path) {
    if (!path[0]) return 1;
    const struct mainui_system *s = mainui_frontend_system(fe, system);
    if (!s || strlen(path) >= sizeof(fe->games_path)) return 0;
    char full[PATH_BYTES];
    path_under(fe, full, s->rompath); char *base = full[0] ? realpath(full, NULL) : NULL;
    path_under(fe, full, path); char *child = full[0] ? realpath(full, NULL) : NULL;
    struct stat st;
    int ok = base && child && !strncmp(base, child, strlen(base)) &&
        (child[strlen(base)] == '/' || child[strlen(base)] == 0) && !stat(child, &st) && S_ISDIR(st.st_mode);
    free(base); free(child); return ok;
}

static int games_at(struct mainui_frontend *fe, int visible_system, const char *path) {
    const struct mainui_system *system = mainui_frontend_system(fe, visible_system);
    if (!system) return 0;
    if (!rom_path_valid(fe, visible_system, path)) { drop_games(fe); return 0; }
    if (fe->games_system != visible_system || strcmp(fe->games_path, path) != 0) {
        drop_games(fe);
        int n = mainui_catalog_games_at(&fe->fs, system, &fe->names, fe->settings.language, path, &fe->games);
        fe->game_count = n > 0 ? n : 0;
        fe->games_system = visible_system;
        snprintf(fe->games_path, sizeof(fe->games_path), "%s", path);
        if (path[0]) return fe->game_count;
        if (visible_system >= MAINUI_BEST_SYSTEM_BASE)
            fe->best_game_counts[visible_system - MAINUI_BEST_SYSTEM_BASE] = fe->game_count;
        else if (fe->game_counts) {
            __atomic_store_n(&fe->game_counts[fe->visible[visible_system]], fe->game_count, __ATOMIC_RELEASE);
            __atomic_store_n(&fe->counts_changed, 1, __ATOMIC_RELEASE);
        }
    }
    return fe->game_count;
}

int mainui_frontend_games(struct mainui_frontend *fe, int visible_system) {
    return games_at(fe, visible_system, "");
}

static struct mainui_window *current_rom_window(struct mainui_frontend *fe, int system) {
    for (int i = fe->model.depth - 1; i > 0; --i) {
        struct mainui_window *w = &fe->model.stack[i];
        if (w->kind == MAINUI_WIN_SEARCH) break;
        if (w->kind == MAINUI_WIN_ROMS && (system < 0 || w->system == system)) return w;
    }
    return NULL;
}

int mainui_frontend_roms(struct mainui_frontend *fe, int visible_system) {
    struct mainui_window *w = current_rom_window(fe, visible_system);
    int n = games_at(fe, visible_system, w ? w->rompath : "");
    if (w) {
        w->item_count = n;
        // Back can reveal a parent whose contents changed while its child was open.
        if (w->focus < 0 || w->focus >= n) w->focus = 0;
        if (w->first < 0) w->first = 0;
        if (w->first > w->focus) w->first = w->focus;
        if (w->focus - w->first >= MAINUI_ROM_LIST_ROWS) w->first = w->focus - MAINUI_ROM_LIST_ROWS + 1;
        int last_first = n > MAINUI_ROM_LIST_ROWS ? n - MAINUI_ROM_LIST_ROWS : 0;
        if (w->first > last_first) w->first = last_first;
        if (w->first < 0) w->first = 0;
    }
    return n;
}

// EmuAction opens a new MenuWindow for a directory instead of publishing a launch.
// Return 0 for a game, 1 for a handled folder and -1 for a refused folder.
static int open_rom_folder(struct mainui_frontend *fe, int system, int index) {
    int n = mainui_frontend_roms(fe, system);
    if (index < 0 || index >= n || !fe->games[index].folder) return 0;
    const struct mainui_system *s = mainui_frontend_system(fe, system);
    char path[1024], image[1024];
    mainui_catalog_game_paths(s, &fe->games[index], path, sizeof(path), image, sizeof(image));
    if (!path[0] || !rom_path_valid(fe, system, path) || fe->model.depth == MAINUI_STACK_MAX) return -1;
    struct mainui_window *w = &fe->model.stack[fe->model.depth++];
    memset(w, 0, sizeof(*w)); w->kind = MAINUI_WIN_ROMS; w->system = system;
    memcpy(w->rompath, path, strlen(path) + 1);
    // MenuWindow retains positions independently for each ppath.
    const struct mainui_position *saved = mainui_positions_find(&fe->positions, path);
    int games = mainui_frontend_roms(fe, system);
    if (saved && saved->pos >= 0 && saved->pos < games) {
        w->focus = saved->pos;
        w->first = saved->start >= 0 && saved->start <= w->focus ? saved->start : 0;
        if (w->focus >= w->first + MAINUI_ROM_LIST_ROWS) w->first = w->focus - MAINUI_ROM_LIST_ROWS + 1;
    }
    note(fe, "open ROM folder system=%d depth=%d", system, fe->model.depth);
    return 1;
}

const struct mainui_list_entry *mainui_frontend_recent(const struct mainui_frontend *fe, int row) {
    return row >= 0 && row < fe->recent_count ? &fe->recent.entries[fe->recent_shown[row]] : NULL;
}

const struct mainui_list_entry *mainui_frontend_favorite(const struct mainui_frontend *fe, int row) {
    return row >= 0 && row < fe->favorites_count ? &fe->favorites.entries[fe->favorites_shown[row]] : NULL;
}

// ---- Time zones ----------------------------------------------------------------------------
// The Time Zone screen browses /usr/share/zoneinfo as a file browser: every entry but hidden
// ones (static: file_filter shows any file under zoneinfo), folders first, each sorted by name
// (w1).

static void drop_zones(struct mainui_frontend *fe, int from_level) {
    for (int level = from_level; level < MAINUI_ZONE_LEVELS; ++level) {
        free(fe->zones[level].zones);
        fe->zones[level].zones = NULL;
        fe->zones[level].count = fe->zones[level].loaded = 0;
    }
}

static struct mainui_zone_folder *zone_folder(struct mainui_frontend *fe, int level) {
    char path[PATH_BYTES], entry_path[PATH_BYTES + 64], folder[160];
    if (level < 0 || level >= MAINUI_ZONE_LEVELS) return NULL;
    struct mainui_zone_folder *f = &fe->zones[level];
    if (f->loaded) return f;
    f->loaded = 1;
    snprintf(folder, sizeof(folder), MAINUI_ZONEINFO "%s%s", f->path[0] ? "/" : "", f->path);
    path_under(fe, path, folder);
    DIR *dir = path[0] ? opendir(path) : NULL;
    if (!dir) return f;
    int capacity = 0;
    if (level > 0) {
        f->zones = calloc(64, sizeof(*f->zones));
        if (!f->zones) { closedir(dir); return f; }
        capacity = 64;
        strcpy(f->zones[0].name, "..");
        f->zones[0].folder = 1;
        f->count = 1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.' || strlen(entry->d_name) >= sizeof(f->zones[0].name)) continue;
        if (f->count == capacity) {
            int grown_capacity = capacity ? capacity * 2 : 64;
            struct mainui_zone *grown = realloc(f->zones, (size_t)grown_capacity * sizeof(*grown));
            if (!grown) break;
            f->zones = grown;
            capacity = grown_capacity;
        }
        struct mainui_zone *z = &f->zones[f->count++];
        struct stat st;
        copy_value(z->name, sizeof(z->name), entry->d_name);
        snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);
        z->folder = stat(entry_path, &st) == 0 && S_ISDIR(st.st_mode);
    }
    closedir(dir);
    mainui_zone_sort(f->zones, f->count);
    return f;
}

// ---- The model's catalog -------------------------------------------------------------------

static void drop_files(struct mainui_frontend *fe) {
    for (int i = 0; i < fe->file_count; ++i) free(fe->file_folders[i].files);
    free(fe->file_folders);
    free(fe->file_history);
    fe->file_folders = NULL;
    fe->file_history = NULL;
    fe->file_count = fe->file_depth = fe->file_capacity = 0;
}

const struct mainui_file_folder *mainui_frontend_file_folder(const struct mainui_frontend *fe, int level) {
    if (level < 0 || level >= fe->file_depth) return NULL;
    int i = fe->file_history[level];
    return i >= 0 && i < fe->file_count ? &fe->file_folders[i] : NULL;
}

// Fake-root sessions must not traverse a link or '..' outside their private tree.
// The real stock browser may follow directories and links under any mount.
static int file_path_inside(const struct mainui_frontend *fe, const char *path) {
    if (!fe->fs.root[0]) return 1;
    char full[PATH_BYTES];
    path_under(fe, full, path);
    char *root = realpath(fe->fs.root, NULL), *resolved = full[0] ? realpath(full, NULL) : NULL;
    int ok = 0;
    if (root && resolved) {
        size_t n = strlen(root);
        ok = !strncmp(root, resolved, n) && (resolved[n] == '/' || resolved[n] == 0);
    }
    free(root); free(resolved);
    return ok;
}

static int reserve_files(struct mainui_frontend *fe, int needed) {
    if (needed <= fe->file_capacity) return 0;
    if (fe->file_capacity > INT_MAX / 2) return -1;
    int capacity = fe->file_capacity ? fe->file_capacity * 2 : 8;
    if (capacity < needed) capacity = needed;
    struct mainui_file_folder *grown = realloc(fe->file_folders, (size_t)capacity * sizeof(*grown));
    if (!grown) return -1;
    fe->file_folders = grown;
    int *history = realloc(fe->file_history, (size_t)capacity * sizeof(*history));
    if (!history) return -1;
    fe->file_history = history;
    fe->file_capacity = capacity;
    return 0;
}

static int file_cache(struct mainui_frontend *fe, const char *path, int parent) {
    for (int i = 0; i < fe->file_count; ++i)
        if (!strcmp(fe->file_folders[i].path, path)) return i;
    if (strlen(path) >= sizeof(fe->file_folders[0].path) || !file_path_inside(fe, path)) return -1;
    struct mainui_file *rows;
    int count = mainui_catalog_files(&fe->fs, fe->systems, fe->system_count, path, parent, &rows);
    if (count < 0) return -1;
    if (reserve_files(fe, fe->file_count + 1) < 0) { free(rows); return -1; }
    int index = fe->file_count++;
    struct mainui_file_folder *f = &fe->file_folders[index];
    snprintf(f->path, sizeof(f->path), "%s", path);
    f->files = rows; f->count = count;
    return index;
}

int mainui_frontend_browse(struct mainui_frontend *fe, const char *path, int title) {
    if (!fe || fe->exit_requested || fe->model.depth <= 0 || fe->model.depth >= MAINUI_STACK_MAX || fe->file_depth) return -1;
    if (!path) path = mainui_launch_other_platform_card(fe->fs.root) ? "/mnt/P2EXT" : "/mnt/";
    if (path[0] != '/') return -1;
    int index = file_cache(fe, path, 0);
    if (index < 0) { drop_files(fe); return -1; }
    if (mainui_model_open_files(&fe->model, title) < 0) { drop_files(fe); return -1; }
    fe->file_history[0] = index;
    fe->file_depth = 1;
    return 0;
}

static int catalog_count(void *context, enum mainui_list list, int arg) {
    struct mainui_frontend *fe = context;
    switch (list) {
    case MAINUI_LIST_NETPLAY: return fe->host_count;
    case MAINUI_LIST_WIFI: return fe->wifi_count;
    case MAINUI_LIST_PAIRED: return fe->paired_count;
    case MAINUI_LIST_BLUETOOTH: return fe->found_count;
    case MAINUI_LIST_TIMEZONES: {
        const struct mainui_zone_folder *f = zone_folder(fe, arg);
        return f ? f->count : 0;
    }
    case MAINUI_LIST_FILES: {
        const struct mainui_file_folder *f = mainui_frontend_file_folder(fe, arg);
        return f ? f->count : 0;
    }
    case MAINUI_LIST_FAVORITES: return fe->favorites_count;
    case MAINUI_LIST_RECENT: return fe->recent_count;
    case MAINUI_LIST_SYSTEMS: return fe->visible_count;
    case MAINUI_LIST_APPS: return fe->shown_app_count;
    case MAINUI_LIST_PORTS: return fe->shown_port_count;
    case MAINUI_LIST_BESTS: return fe->shown_best_count;
    case MAINUI_LIST_ROMS: return mainui_frontend_roms(fe, arg);
    case MAINUI_LIST_LAUNCHERS: {
        const struct mainui_system *s = mainui_frontend_system(fe, arg);
        return s ? s->launcher_count : 0;
    }
    case MAINUI_LIST_SEARCH: return fe->hit_count;
    case MAINUI_LIST_THEMES: return fe->theme_count;
    case MAINUI_LIST_LANGUAGES: return fe->language_count;
    case MAINUI_LIST_BOOTLOGOS: return fe->bootlogo_count;
    case MAINUI_LIST_SPLASHSCREENS: return fe->splashscreen_count;
    case MAINUI_LIST_LOADING_SCREENS: return fe->loading_screen_count;
    case MAINUI_LIST_FONTS: return fe->font_count;
    case MAINUI_LIST_EDIT: return arg == MAINUI_TAB_APPS ? fe->app_count : fe->system_count;
    default: return 0;
    }
}

// Both optional tabs show only when a configured item is shown (static: loadEmusApps).
static int catalog_content(void *context, enum mainui_tab tab) {
    const struct mainui_frontend *fe = context;
    return (tab == MAINUI_TAB_PORTS && fe->shown_port_count > 0) ||
           (tab == MAINUI_TAB_BESTS && fe->shown_best_count > 0);
}

// GameSwitcher's app unless it is turned off; an app hidden from Apps by show.json still counts,
// as the BrickOS desktop shows its card either way.
static const struct mainui_app *gameswitcher_app(const struct mainui_frontend *fe) {
    if (fe->gameswitcher_off) return NULL;
    for (int i = 0; i < fe->app_count; ++i)
        if (is_gameswitcher(&fe->apps[i])) return &fe->apps[i];
    return NULL;
}

static int catalog_gameswitcher(void *context) {
    return gameswitcher_app(context) != NULL;
}

static int catalog_flags(void *context, enum mainui_list list, int arg, int index) {
    struct mainui_frontend *fe = context;
    if (list == MAINUI_LIST_ROMS) {
        int n = mainui_frontend_roms(fe, arg);
        if (index >= 0 && index < n && fe->games[index].folder) return MAINUI_ENTRY_FOLDER;
    }
    if (list == MAINUI_LIST_WIFI && index >= 0 && index < fe->wifi_count) {
        const struct mainui_wifi_network *w = &fe->wifi[index];
        return (w->locked ? MAINUI_ENTRY_LOCKED : 0) | (w->saved_id >= 0 ? MAINUI_ENTRY_SAVED : 0) |
               (w->connected ? MAINUI_ENTRY_CONNECTED : 0);
    }
    if (list == MAINUI_LIST_PAIRED && index >= 0 && index < fe->paired_count)
        return fe->paired[index].connected ? MAINUI_ENTRY_CONNECTED : 0;
    if (list == MAINUI_LIST_TIMEZONES) {
        const struct mainui_zone_folder *f = zone_folder(fe, arg);
        if (f && index >= 0 && index < f->count && f->zones[index].folder) return MAINUI_ENTRY_FOLDER;
    }
    return 0;
}

// ---- System list positions -----------------------------------------------------------------
// A system list opens where it was left, in this run or an earlier one of either frontend: the
// positions are shared with the official in /mnt/romwinidx.json (lists.h).

static void load_positions(struct mainui_frontend *fe) {
    char path[PATH_BYTES];
    fe->positions.count = 0;
    path_under(fe, path, MAINUI_POSITIONS_FILE);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t size = POSITION_BYTES;
    char *text = malloc(size);
    size_t n = text ? fread(text, 1, size - 1, f) : 0;
    fclose(f);
    if (!text) return;
    text[n] = 0;
    mainui_positions_parse(&fe->positions, text, n);
    free(text);
}

static int catalog_position(void *context, enum mainui_list list, int arg, int *pos, int *start, int *end) {
    struct mainui_frontend *fe = context;
    const struct mainui_system *s = mainui_frontend_system(fe, arg);
    if (list != MAINUI_LIST_ROMS || !s) return -1;
    const struct mainui_position *p = mainui_positions_find(&fe->positions, s->rompath);
    if (!p) return -1;
    *pos = p->pos;
    *start = p->start;
    *end = p->end;
    return 0;
}

// The official records a list's position as the list closes (static: MenuWindow::quit), with
// its last shown row; keeping the open list's position current gives the same table, also for a
// list left by a launch.
static void track_positions(struct mainui_frontend *fe) {
    struct mainui_window *visible = current_rom_window(fe, -1);
    if (visible) mainui_frontend_roms(fe, visible->system);
    const struct mainui_model *m = &fe->model;
    for (int d = 1; d < m->depth; ++d) {
        const struct mainui_window *w = &m->stack[d];
        const struct mainui_system *s = mainui_frontend_system(fe, w->system);
        if (w->kind != MAINUI_WIN_ROMS || !s) continue;
        // Hidden parents retain their last count; loading each one would evict the
        // active folder and rescan the whole chain on every navigation key.
        int n = w->item_count;
        int end = w->first + MAINUI_ROM_LIST_ROWS < n ? w->first + MAINUI_ROM_LIST_ROWS - 1 : n - 1;
        if (end < 0) end = 0;
        if (mainui_positions_set(&fe->positions, w->rompath[0] ? w->rompath : s->rompath, w->focus, w->first, end) > 0)
            fe->positions_changed = 1;
    }
}

int mainui_frontend_save_positions(struct mainui_frontend *fe) {
    char path[PATH_BYTES], temporary[PATH_BYTES + 8];
    // Main calls this before close joins a pending refresh. Its old in-memory
    // positions must not recreate a file the worker has already removed.
    if (!fe->positions_changed || fe->dry || fe->refresh_pending) return 0;
    size_t size = POSITION_BYTES;
    char *text = malloc(size);
    int n = text ? mainui_positions_format(&fe->positions, text, size) : -1;
    path_under(fe, path, MAINUI_POSITIONS_FILE);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *f = n >= 0 ? fopen(temporary, "wb") : NULL;
    int ok = f && fwrite(text, 1, (size_t)n, f) == (size_t)n;
    if (f && fclose(f) != 0) ok = 0;
    free(text);
    if (!ok || rename(temporary, path) != 0) {
        if (f) remove(temporary);
        note(fe, "positions save failed");
        return -1;
    }
    fe->positions_changed = 0;
    note(fe, "positions saved count=%d", fe->positions.count);
    return 0;
}

// The entry a launch or favorite of a system's game records.
static int game_value_entry(struct mainui_frontend *fe, int visible_system, const struct mainui_game *game,
                            struct mainui_list_entry *e) {
    const struct mainui_system *s = mainui_frontend_system(fe, visible_system);
    if (!s || !game) return -1;
    char rompath[1024], imgpath[1024];
    mainui_catalog_game_paths(s, game, rompath, sizeof(rompath), imgpath, sizeof(imgpath));
    mainui_list_entry_set(e, game->label, rompath, imgpath, s->launch, s->label, MAINUI_ENTRY_GAME);
    return 0;
}

static int game_entry(struct mainui_frontend *fe, int visible_system, int index, struct mainui_list_entry *e) {
    if (index < 0 || index >= mainui_frontend_roms(fe, visible_system)) return -1;
    return game_value_entry(fe, visible_system, &fe->games[index], e);
}

const struct mainui_game *mainui_frontend_search_game(const struct mainui_frontend *fe, int row) {
    if (row < 0 || row >= fe->hit_count || !fe->hit_games) return NULL;
    int game = fe->hits[row].game;
    return game >= 0 && game < fe->hit_count ? &fe->hit_games[game] : NULL;
}

int mainui_frontend_is_favorite(struct mainui_frontend *fe, enum mainui_list list, int arg, int index) {
    struct mainui_list_entry e;
    if (list == MAINUI_LIST_RECENT) {
        const struct mainui_list_entry *r = mainui_frontend_recent(fe, index);
        return r && mainui_favorites_find(&fe->favorites, r) >= 0;
    }
    if (list == MAINUI_LIST_ROMS && game_entry(fe, arg, index, &e) == 0)
        return mainui_favorites_find(&fe->favorites, &e) >= 0;
    return 0;
}

static int index_of(const char *value, const char *const *choices, int count) {
    for (int i = 0; i < count; ++i)
        if (strcmp(value, choices[i]) == 0) return i;
    return 0;
}

static void load_state(struct mainui_frontend *fe);
#define BRICKOS_LAST_VIEW_JSON "/mnt/SDCARD/BrickOS/mainui/brickos/last-view.json"
static int load_brickos_last_view(struct mainui_frontend *fe);
static int save_brickos_last_view(struct mainui_frontend *fe);

static void ext_open(struct mainui_frontend *fe);

int mainui_frontend_open(struct mainui_frontend *fe, const char *root, int dry, FILE *record) {
    char path[PATH_BYTES];
    memset(fe, 0, sizeof(*fe));
    fe->fs.root = root ? root : "";
    fe->dry = dry;
    fe->record = record;
    fe->bootlogo_active = fe->splashscreen_active = fe->loading_screen_active = -2;
    fe->boot_visuals.lock_ready = pthread_mutex_init(&fe->boot_visuals.lock, NULL) == 0;
    if (mainui_boot_visuals_modes_load(fe->fs.root, fe->boot_visuals.random) < 0)
        note(fe, "boot visual modes invalid; both fixed");
    fe->games_system = -1;
    fe->bt_fd = fe->bt_request_fd = fe->netplay_fd = -1;
    fe->cover_enabled = !dry && !fe->fs.root[0];
    fe->cover_port = 80;
    fe->wifi_rssi = MAINUI_RSSI_NONE;
    mainui_brickos_preferences_init(&fe->brickos_preferences);
    path_under(fe, path, MAINUI_BRICKOS_PREFERENCES_PATH);
    if (mainui_brickos_preferences_load(&fe->brickos_preferences, path) < 0)
        note(fe, "frontend preferences invalid; using launch default");
    path_under(fe, path, "/mnt/UDISK/system.json");
    mainui_settings_load(&fe->settings, path);
    if (load_catalog(fe) < 0) return -1;
    mainui_names_open(&fe->names, fe->fs.root);
    load_lists(fe);
    fe->theme_count = mainui_catalog_themes(&fe->fs, &fe->themes);
    if (fe->theme_count < 0) fe->theme_count = 0;
    fe->bootlogo_count = mainui_boot_visuals_catalog(fe->fs.root, MAINUI_BOOTLOGO, &fe->bootlogos);
    if (fe->bootlogo_count < 0) fe->bootlogo_count = 0;
    fe->splashscreen_count = mainui_boot_visuals_catalog(fe->fs.root, MAINUI_SPLASHSCREEN, &fe->splashscreens);
    if (fe->splashscreen_count < 0) fe->splashscreen_count = 0;
    fe->loading_screen_count = mainui_loading_screens_catalog(fe->fs.root, &fe->loading_screens);
    if (fe->loading_screen_count < 0) fe->loading_screen_count = 0;
    fe->language_count = mainui_asset_languages(&fe->fs, fe->languages, 16);
    load_language(fe);
    load_language_icons(fe);
    load_device_info(fe);
    load_hotspot(fe);
    int values[MAINUI_SET_COUNT];
    memcpy(values, fe->settings.values, sizeof(values));
    const char *themes[64], *languages[16];
    for (int i = 0; i < fe->theme_count && i < 64; ++i) themes[i] = fe->themes[i].path;
    for (int i = 0; i < fe->language_count; ++i) languages[i] = fe->languages[i];
    values[MAINUI_SET_THEME] = index_of(fe->settings.theme, themes, fe->theme_count < 64 ? fe->theme_count : 64);
    values[MAINUI_SET_LANGUAGE] = index_of(fe->settings.language, languages, fe->language_count);
    load_positions(fe);
    struct mainui_catalog catalog = { fe, catalog_count, catalog_content, catalog_flags, catalog_position,
                                      catalog_gameswitcher };
    mainui_model_init(&fe->model, values, &catalog);
    fe->model.brickos_initial_view = fe->brickos_preferences.initial_view;
    led_read(fe);
    appearance_open(fe);
    load_state(fe);
    ext_open(fe);
    const struct mainui_window *top = mainui_model_top(&fe->model);
    fe->ext_on_settings = fe->model.depth == 1 && top->kind == MAINUI_WIN_TABS && fe->model.tab == MAINUI_TAB_SETTINGS;
    return 0;
}

const char *mainui_frontend_look_theme(const struct mainui_frontend *fe) {
    return fe->model.presentation == MAINUI_PRESENTATION_MATERIAL_YOU ? "../res/" : fe->settings.theme;
}

void mainui_frontend_set_presentation(struct mainui_frontend *fe, enum mainui_presentation presentation) {
    if (!fe) return;
    mainui_model_set_presentation(&fe->model, presentation, fe->resume_found);
    if (presentation == MAINUI_PRESENTATION_MATERIAL_YOU) {
        char sidecar[PATH_BYTES], last_view[PATH_BYTES];
        path_under(fe, sidecar, MAINUI_BRICKOS_RETURN_PATH);
        path_under(fe, last_view, BRICKOS_LAST_VIEW_JSON);
        const char *pending = getenv("BRICKOS_MAINUI_PENDING");
        int valid_sidecar = fe->resume_found && pending &&
            mainui_brickos_return_validate_view(sidecar, pending, fe->brickos_state_digest, last_view);
        if (valid_sidecar) note(fe, "brickos launch return validated");
        else if (pending && pending[0]) note(fe, "brickos launch return did not match pending token");
        char path[PATH_BYTES];
        path_under(fe, path, MAINUI_BRICKOS_PREFERENCES_PATH);
        if (mainui_brickos_preferences_load(&fe->brickos_preferences, path) < 0)
            note(fe, "brickos preferences invalid; using desktop default");
        fe->model.brickos_initial_view = fe->brickos_preferences.initial_view;
        int returned = valid_sidecar && load_brickos_last_view(fe);
        if (!fe->dry) mainui_brickos_return_consume(sidecar);
        if (returned) return;
        if (!fe->resume_found && fe->brickos_preferences.initial_view == MAINUI_BRICKOS_VIEW_LAST_USED &&
            load_brickos_last_view(fe)) return;
        if (!fe->resume_found)
            mainui_model_apply_brickos_initial_view(&fe->model, fe->brickos_preferences.initial_view);
    }
}

void mainui_frontend_use(struct mainui_frontend *fe, const char *theme, const char *language) {
    const char *themes[64], *languages[16];
    if (theme) mainui_settings_set_theme(&fe->settings, theme);
    if (language) {
        mainui_settings_set_language(&fe->settings, language);
        load_language(fe);
        drop_games(fe);
        // Extension texts follow at once; their scripts get the new code.
        mainui_ext_set_language(&fe->ext, fe->settings.language);
        mainui_ext_runner_set_language(&fe->ext_runner, fe->ext.language);
    }
    for (int i = 0; i < fe->theme_count && i < 64; ++i) themes[i] = fe->themes[i].path;
    for (int i = 0; i < fe->language_count; ++i) languages[i] = fe->languages[i];
    fe->model.settings[MAINUI_SET_THEME] = index_of(fe->settings.theme, themes, fe->theme_count < 64 ? fe->theme_count : 64);
    fe->model.settings[MAINUI_SET_LANGUAGE] = index_of(fe->settings.language, languages, fe->language_count);
}

static void bt_pairable(struct mainui_frontend *fe, int on);
static void carry_out_all(struct mainui_frontend *fe);
static void ext_stop(struct mainui_frontend *fe);
static int ext_settle(struct mainui_frontend *fe, uint64_t now_ms, int all);
static void mapping_save(struct mainui_frontend *fe, int reset);
static void netplay_cover_close(struct mainui_frontend *fe);
static int refresh_finish(struct mainui_frontend *fe, int closing);
static void power_close(struct mainui_frontend *fe);
static void rumble_close(struct mainui_frontend *fe);
static void io_motor_stop(struct mainui_frontend *fe);
static void io_shortcut(struct mainui_frontend *fe, int active);

static void backdrop_close(struct mainui_frontend *fe);

void mainui_frontend_close(struct mainui_frontend *fe) {
    const char *run_token = getenv("BRICKOS_MAINUI_TOKEN");
    if (fe->model.presentation == MAINUI_PRESENTATION_MATERIAL_YOU && !fe->dry &&
        (!fe->exit_requested || !run_token || !run_token[0]))
        (void)save_brickos_last_view(fe);
    ext_stop(fe);
    mainui_ext_runner_stop_all(&fe->picker_runner);
    // A mapping or appearance change still settling is written before the frontend goes.
    if (fe->mapping_pending) mapping_save(fe, 0);
    if (fe->appearance_pending) appearance_save(fe);
    io_motor_stop(fe);
    rumble_close(fe);
    backdrop_close(fe);
    power_close(fe);
    refresh_finish(fe, 1);
    mainui_model_finish(&fe->model);
    carry_out_all(fe);
    io_shortcut(fe, 0); // retry an owned release that failed at screen exit
    // A connect still running when the frontend ends leaves the adapter as it found it.
    bt_pairable(fe, 0);
    stop_counting(fe);
    // A boot image being written is finished before the game or the next frontend starts.
    if (fe->boot_visuals.worker_running) pthread_join(fe->boot_visuals.worker, NULL);
    fe->boot_visuals.worker_running = 0;
    // So is the music picture, which is then not named.
    if (fe->music.running) pthread_join(fe->music.worker, NULL);
    fe->music.running = 0;
    free(fe->music.job);
    fe->music.job = NULL;
    if (fe->boot_visuals.lock_ready) pthread_mutex_destroy(&fe->boot_visuals.lock);
    fe->boot_visuals.lock_ready = 0;
    netplay_cover_close(fe);
    drop_games(fe);
    mainui_names_close(&fe->names);
    free(fe->game_counts);
    free(fe->systems);
    free(fe->apps);
    free(fe->ports);
    free(fe->bests);
    free(fe->themes);
    free(fe->bootlogos);
    free(fe->splashscreens);
    free(fe->loading_screens);
    free(fe->fonts);
    free(fe->hit_games);
    free(fe->hits);
    mainui_list_free(&fe->recent);
    mainui_list_free(&fe->favorites);
    mainui_lang_free(&fe->lang);
    drop_zones(fe, 0);
    drop_files(fe);
    if (fe->bt_fd >= 0) close(fe->bt_fd);
    if (fe->bt_request_fd >= 0) close(fe->bt_request_fd);
    if (fe->netplay_fd >= 0) close(fe->netplay_fd);
    ext_stop(fe);
    mainui_ext_free(&fe->ext);
    free(fe->ext_view);
    memset(fe, 0, sizeof(*fe));
    fe->bt_fd = fe->bt_request_fd = fe->netplay_fd = -1;
}

// ---- Effects -------------------------------------------------------------------------------

static int rumble_write(struct mainui_frontend *fe, const char *value) {
    return fe->rumble_ops.write_file(fe->rumble_ops.context, "/sys/class/gpio/gpio227/value", value);
}

static void *rumble_worker(void *context) {
    struct mainui_frontend *fe = context;
    pthread_mutex_lock(&fe->rumble_mutex);
    while (!fe->rumble_cancel) {
        int result = pthread_cond_timedwait(&fe->rumble_cond, &fe->rumble_mutex, &fe->rumble_until);
        if (result == 0) continue; // Cancellation or a spurious wakeup.
        if (result != ETIMEDOUT) fe->rumble_failed = 1;
        if (!fe->rumble_cancel) {
            if (rumble_write(fe, "0") < 0) fe->rumble_failed = 1;
            else fe->rumble_active = 0;
        }
        break;
    }
    __atomic_store_n(&fe->rumble_done, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&fe->rumble_mutex);
    return NULL;
}

// Replacing a stock timer does not insert an off write between two previews.
// Close/handoff and scheduling refusal do stop the pulse this frontend owns.
static void rumble_finish(struct mainui_frontend *fe, int off) {
    if (!fe->rumble_initialized) return;
    if (fe->rumble_running) {
        pthread_mutex_lock(&fe->rumble_mutex);
        fe->rumble_cancel = 1;
        pthread_cond_signal(&fe->rumble_cond);
        pthread_mutex_unlock(&fe->rumble_mutex);
        pthread_join(fe->rumble_thread, NULL);
        fe->rumble_running = 0;
        if (fe->rumble_failed) { ++fe->effects_failed; note(fe, "rumble preview completion failed"); }
        fe->rumble_failed = 0;
    }
    if (off && fe->rumble_active) {
        if (rumble_write(fe, "0") < 0) { ++fe->effects_failed; note(fe, "rumble preview stop failed"); }
        else fe->rumble_active = 0;
    }
}

static void rumble_start(struct mainui_frontend *fe) {
    if (!fe->ops.write_file) { rumble_finish(fe, 1); return; }
    if (!fe->rumble_initialized) {
        pthread_condattr_t attr;
        int mutex_ok = pthread_mutex_init(&fe->rumble_mutex, NULL) == 0;
        int attr_ok = pthread_condattr_init(&attr) == 0;
        int cond_ok = mutex_ok && attr_ok && pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 &&
                      pthread_cond_init(&fe->rumble_cond, &attr) == 0;
        if (attr_ok) pthread_condattr_destroy(&attr);
        if (!cond_ok) {
            if (mutex_ok) pthread_mutex_destroy(&fe->rumble_mutex);
            ++fe->effects_failed; note(fe, "rumble preview timer unavailable"); return;
        }
        fe->rumble_initialized = 1;
    }
    fe->rumble_ops = fe->ops;
    if (clock_gettime(CLOCK_MONOTONIC, &fe->rumble_until) < 0) {
        ++fe->effects_failed; note(fe, "rumble preview clock unavailable"); rumble_finish(fe, 1); return;
    }
    fe->rumble_until.tv_nsec += 200000000;
    if (fe->rumble_until.tv_nsec >= 1000000000) { ++fe->rumble_until.tv_sec; fe->rumble_until.tv_nsec -= 1000000000; }
    fe->rumble_cancel = fe->rumble_failed = 0;
    __atomic_store_n(&fe->rumble_done, 0, __ATOMIC_RELEASE);
    // Hold the worker until enable has been attempted, so it cannot publish
    // its timeout's zero before the UI thread publishes one.
    pthread_mutex_lock(&fe->rumble_mutex);
    int result = pthread_create(&fe->rumble_thread, NULL, rumble_worker, fe);
    if (!result) {
        fe->rumble_running = 1;
        // A failed enable write is still followed by the scheduled off attempt.
        fe->rumble_active = 1;
        if (rumble_write(fe, "1") < 0) { ++fe->effects_failed; note(fe, "rumble preview enable failed"); }
    }
    pthread_mutex_unlock(&fe->rumble_mutex);
    if (result) {
        ++fe->effects_failed; note(fe, "rumble preview worker unavailable"); rumble_finish(fe, 1);
    }
}

static void rumble_close(struct mainui_frontend *fe) {
    rumble_finish(fe, 1);
    if (fe->rumble_initialized) {
        pthread_cond_destroy(&fe->rumble_cond); pthread_mutex_destroy(&fe->rumble_mutex);
        fe->rumble_initialized = 0;
    }
}

static int io_motor_write(struct mainui_frontend *fe, int on) {
    if (!fe->dry && fe->ops.write_file && fe->ops.write_file(fe->ops.context,
            "/sys/class/gpio/gpio227/value", on ? "1" : "0") < 0) {
        ++fe->effects_failed; note(fe, "IO motor %s failed", on ? "enable" : "release");
        return -1;
    }
    return 0;
}

static void io_motor_stop(struct mainui_frontend *fe) {
    if (fe->io_motor) io_motor_write(fe, 0);
    fe->io_motor = 0;
}

// keymon parses hotkeyshow to select the OSD chord. Hardware Test owns the
// conflicting MENU+START until exit; keep system.json and shared settings intact.
static int io_shortcut_write(const char *path, const char *value) {
    char temporary[PATH_BYTES + 16];
    int n = snprintf(temporary, sizeof(temporary), "%s.io-XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(temporary)) return -1;
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    size_t length = strlen(value);
    int failed = write(fd, value, length) != (ssize_t)length;
    if (close(fd) < 0) failed = 1;
    if (!failed && rename(temporary, path) < 0) failed = 1;
    if (failed) unlink(temporary);
    return failed ? -1 : 0;
}

static void io_shortcut(struct mainui_frontend *fe, int active) {
    if (fe->dry || (active && (fe->io_shortcut_suspended ||
            fe->settings.values[MAINUI_SET_OSDSHORTCUT] != 1)) ||
            (!active && !fe->io_shortcut_suspended)) return;
    char path[PATH_BYTES], current[128];
    path_under(fe, path, "/tmp/trimui_osd/hotkeyshow");
    FILE *file = fopen(path, "rb");
    size_t length = file ? fread(current, 1, sizeof(current) - 1, file) : 0;
    int failed = !file || ferror(file) || !feof(file);
    if (file && fclose(file) < 0) failed = 1;
    if (failed) {
        ++fe->effects_failed; note(fe, "IO shortcut read failed"); return;
    }
    current[length] = 0;
    if (active && (length != 10 || strcmp(current, "MENU+START"))) return;
    if (!active && (length != 3 || strcmp(current, "OFF"))) {
        fe->io_shortcut_suspended = 0;
        note(fe, "IO shortcut ownership transferred");
        return;
    }
    if (io_shortcut_write(path, active ? "OFF" : "MENU+START") < 0) {
        ++fe->effects_failed; note(fe, "IO shortcut %s failed", active ? "suspend" : "restore"); return;
    }
    fe->io_shortcut_suspended = active;
    note(fe, "IO shortcut %s file=%s configured=MENU+START", active ? "suspended" : "restored", active ? "OFF" : "MENU+START");
}

int mainui_frontend_io(struct mainui_frontend *fe, uint32_t held) {
    const struct mainui_window *w = mainui_model_top(&fe->model);
    int active = w && w->kind == MAINUI_WIN_SETTINGS && w->screen == MAINUI_SCREEN_IO_TEST;
    if (!active) held = 0;
    uint32_t shoulders = (UINT32_C(1) << MAINUI_TEST_L1) | (UINT32_C(1) << MAINUI_TEST_R1);
    int motor = (held & shoulders) == shoulders, fn1 = 0;
    if (active) {
        char path[PATH_BYTES], value[16];
        int n = snprintf(path, sizeof(path), "%s/sys/class/gpio/gpio243/value", fe->fs.root ? fe->fs.root : "");
        if (n >= 0 && (size_t)n < sizeof(path)) {
            FILE *file = fopen(path, "rb");
            if (file) { if (fgets(value, sizeof(value), file)) fn1 = atoi(value) != 0; fclose(file); }
        }
    }
    int changed = held != fe->io_held || motor != fe->io_motor || fn1 != fe->io_fn1;
    if (motor != fe->io_motor) {
        if (motor) rumble_finish(fe, 1); // an earlier Volume preview cannot turn this test off later
        int result = io_motor_write(fe, motor);
        if (!result || motor) fe->io_motor = motor; // retain failed release for retry/close
    }
    fe->io_held = held; fe->io_fn1 = fn1;
    return changed;
}

static void save_list(struct mainui_frontend *fe, struct mainui_entries *list, const char *path) {
    char full[PATH_BYTES];
    refresh_lists(fe);
    if (fe->dry) return;
    path_under(fe, full, path);
    if (mainui_list_save(list, full) < 0) ++fe->effects_failed;
}

// ---- The window stack across a launch ------------------------------------------------------
// The official MainUI writes /tmp/state.json with its launch command, and the next MainUI reads
// and removes it (l2b, a2). Its objects, in cJSON's formatted print: home (type 49), the tab
// window (type 0) with the open tab's place and each visible tab's currpos, pagestart, and
// pageend, and a system list (type 23) with the launch script that names its system.

#define STATE_JSON "/tmp/state.json"
static void search(struct mainui_frontend *fe, const char *text, int system);

static int state_append(char **out, size_t *size, size_t *n, const char *text) {
    size_t length = strlen(text);
    if (length >= STATE_BYTES || *n >= STATE_BYTES - length) return -1;
    if (*n + length >= *size) {
        size_t capacity = *size;
        while (capacity <= *n + length) capacity = capacity > STATE_BYTES / 2 ? STATE_BYTES : capacity * 2;
        char *grown = realloc(*out, capacity);
        if (!grown) return -1;
        *out = grown; *size = capacity;
    }
    memcpy(*out + *n, text, length + 1);
    *n += length;
    return 0;
}

static int state_parent_directories(const char *path) {
    char parent[PATH_BYTES];
    size_t n = strlen(path);
    if (n >= sizeof(parent)) return -1;
    memcpy(parent, path, n + 1);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return 0;
    *slash = 0;
    for (char *p = parent + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(parent, 0755) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(parent, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

// The items of a tab and whether it is a list, for its page bounds.
static int state_tab_items(const struct mainui_frontend *fe, enum mainui_tab tab, int *list) {
    *list = tab == MAINUI_TAB_RECENT || tab == MAINUI_TAB_FAVORITES;
    switch (tab) {
    case MAINUI_TAB_RECENT: return fe->recent_count;
    case MAINUI_TAB_FAVORITES: return fe->favorites_count;
    case MAINUI_TAB_GAMES: return fe->visible_count;
    case MAINUI_TAB_BESTS: return fe->shown_best_count;
    case MAINUI_TAB_APPS: return fe->shown_app_count;
    case MAINUI_TAB_PORTS: return fe->shown_port_count;
    case MAINUI_TAB_SETTINGS: return mainui_model_settings_tiles(&fe->model);
    default: return 0;
    }
}

static int save_state_file(struct mainui_frontend *fe, const char *destination, int brickos_metadata) {
    struct mainui_resume r;
    enum mainui_tab tabs[MAINUI_TAB_COUNT];
    char line[12 * 1024], escaped[512 * 6 + 3], directory[1024 * 6 + 3], query[MAINUI_TEXT_MAX * 6 + 3],
         focus_path[1024], image_path[1024], focus_escaped[1024 * 6 + 3], focus_name[256 * 6 + 3],
         path[PATH_BYTES], temporary[PATH_BYTES + 8];
    size_t n = 0, capacity = 4096;
    if (mainui_model_resume_state(&fe->model, &r) < 0) return 0;
    char *text = malloc(capacity);
    if (!text) return -1;
    mainui_model_visible_tabs(&fe->model, tabs);
    if (brickos_metadata)
        snprintf(line, sizeof(line), "{\n\t\"version\":\t1,\n\t\"presentation\":\t\"material-you\",\n"
            "\t\"list\":\t[{\n\t\t\t\"title\":\t225,\n\t\t\t\"type\":\t49,\n"
            "\t\t\t\"currpos\":\t%d,\n\t\t\t\"pagestart\":\t%d,\n\t\t\t\"pageend\":\t%d%s\n\t\t}",
            r.home ? r.home_focus : 0, r.home ? r.home_first : 0,
            r.home ? (r.home_first + 3 < r.tab_count ? r.home_first + 3 : r.tab_count - 1) : 0,
            // Only BrickOS's own file names the GameSwitcher card; state.json keeps the tab beside it.
            r.home && r.home_gameswitcher ? ",\n\t\t\t\"gameswitcher\":\t1" : "");
    else snprintf(line, sizeof(line), "{\n\t\"list\":\t[{\n\t\t\t\"title\":\t225,\n\t\t\t\"type\":\t49,\n"
        "\t\t\t\"currpos\":\t%d,\n\t\t\t\"pagestart\":\t%d,\n\t\t\t\"pageend\":\t%d\n\t\t}",
        r.home ? r.home_focus : 0, r.home ? r.home_first : 0,
        r.home ? (r.home_first + 3 < r.tab_count ? r.home_first + 3 : r.tab_count - 1) : 0);
    int failed = state_append(&text, &capacity, &n, line);
    if (!r.home) {
        failed |= state_append(&text, &capacity, &n, ", {\n\t\t\t\"title\":\t225,\n\t\t\t\"type\":\t0,\n");
        snprintf(line, sizeof(line), "\t\t\t\"tabidx\":\t%d,\n\t\t\t\"tabstartidx\":\t0,\n\t\t\t\"tabstate\":\t[", r.tab_index);
        failed |= state_append(&text, &capacity, &n, line);
        for (int i = 0; i < r.tab_count; ++i) {
            int list, items = state_tab_items(fe, tabs[i], &list);
            int start = list ? r.tabs[i].first : r.tabs[i].focus / 6 * 6;
            int end = start + (list ? MAINUI_TAB_LIST_ROWS : 6) - 1;
            if (end > items - 1) end = items > 0 ? items - 1 : 0;
            if (items == 0) start = 0;
            snprintf(line, sizeof(line), "%s{\n\t\t\t\t\t\"currpos\":\t%d,\n\t\t\t\t\t\"pagestart\":\t%d,\n\t\t\t\t\t\"pageend\":\t%d\n\t\t\t\t}",
                     i ? ", " : "", r.tabs[i].focus, start, end);
            failed |= state_append(&text, &capacity, &n, line);
        }
        failed |= state_append(&text, &capacity, &n, "]\n\t\t}");
    }
    const struct mainui_system *s = NULL;
    for (int i = 0; i < r.rom_count; ++i) {
        s = mainui_frontend_system(fe, r.roms[i].system);
        if (!s) goto refused;
        int games = games_at(fe, r.roms[i].system, r.roms[i].path);
        int end = r.roms[i].first + MAINUI_ROM_LIST_ROWS - 1;
        if (end > games - 1) end = games > 0 ? games - 1 : 0;
        if (mainui_json_escape(escaped, sizeof(escaped), s->launch, strlen(s->launch)) < 0) goto refused;
        snprintf(line, sizeof(line), ", {\n\t\t\t\"title\":\t-1,\n\t\t\t\"type\":\t23,\n\t\t\t\"currpos\":\t%d,\n\t\t\t\"pagestart\":\t%d,\n"
                 "\t\t\t\"pageend\":\t%d,\n\t\t\t\"emulaunch\":\t%s", r.roms[i].focus, r.roms[i].first, end, escaped);
        failed |= state_append(&text, &capacity, &n, line);
        if (r.roms[i].path[0]) {
            if (mainui_json_escape(directory, sizeof(directory), r.roms[i].path, strlen(r.roms[i].path)) < 0) goto refused;
            snprintf(line, sizeof(line), ",\n\t\t\t\"ppath\":\t%s", directory);
            failed |= state_append(&text, &capacity, &n, line);
        }
        if (brickos_metadata && i == r.rom_count - 1 && r.roms[i].focus >= 0 &&
            r.roms[i].focus < games && fe->games) {
            mainui_catalog_game_paths(s, &fe->games[r.roms[i].focus], focus_path, sizeof(focus_path),
                                      image_path, sizeof(image_path));
            if (mainui_json_escape(focus_escaped, sizeof(focus_escaped), focus_path, strlen(focus_path)) < 0)
                goto refused;
            snprintf(line, sizeof(line), ",\n\t\t\t\"brickos_focus_path\":\t%s", focus_escaped);
            failed |= state_append(&text, &capacity, &n, line);
        }
        failed |= state_append(&text, &capacity, &n, "\n\t\t}");
    }
    const struct mainui_window *w = mainui_model_top(&fe->model);
    if (w && w->kind == MAINUI_WIN_FILES) {
        const struct mainui_file_folder *folder = mainui_frontend_file_folder(fe, w->system);
        int end = w->first + MAINUI_ROM_LIST_ROWS - 1;
        if (folder && end >= folder->count) end = folder->count - 1;
        snprintf(line, sizeof(line), ", {\n\t\t\t\"title\":\t%d,\n\t\t\t\"type\":\t4,\n"
                 "\t\t\t\"currpos\":\t%d,\n\t\t\t\"pagestart\":\t%d,\n\t\t\t\"pageend\":\t%d",
                 w->index, w->focus, w->first, end);
        failed |= state_append(&text, &capacity, &n, line);
        if (brickos_metadata && fe->file_depth > 0) {
            const struct mainui_file_folder *root = mainui_frontend_file_folder(fe, 0);
            const char *selected = folder && w->focus >= 0 && w->focus < folder->count
                ? folder->files[w->focus].name : "";
            if (!folder || !root ||
                mainui_json_escape(directory, sizeof(directory), folder->path, strlen(folder->path)) < 0 ||
                mainui_json_escape(escaped, sizeof(escaped), root->path, strlen(root->path)) < 0 ||
                mainui_json_escape(focus_name, sizeof(focus_name), selected, strlen(selected)) < 0) goto refused;
            snprintf(line, sizeof(line), ",\n\t\t\t\"ppath\":\t%s,\n\t\t\t\"brickos_files_root\":\t%s,\n"
                     "\t\t\t\"brickos_focus_name\":\t%s", directory, escaped, focus_name);
            failed |= state_append(&text, &capacity, &n, line);
        }
        failed |= state_append(&text, &capacity, &n, "\n\t\t}");
    }
    if (r.keyboard) {
        // ImeWindow saves only these fields; its editor is freshly constructed on return.
        failed |= state_append(&text, &capacity, &n, ", {\n\t\t\t\"title\":\t134,\n\t\t\t\"type\":\t26,\n"
            "\t\t\t\"parenttype\":\t-1,\n\t\t\t\"emuname\":\t\"\"\n\t\t}");
    }
    if (r.search) {
        s = mainui_frontend_system(fe, r.search_system);
        if (r.search_system >= 0 && !s) goto refused;
        int end = r.search_first + mainui_search_rows(r.search_system) - 1;
        if (end >= fe->hit_count) end = fe->hit_count ? fe->hit_count - 1 : 0;
        if (mainui_json_escape(query, sizeof(query), r.query, strlen(r.query)) < 0) goto refused;
        snprintf(line, sizeof(line), ", {\n\t\t\t\"title\":\t134,\n\t\t\t\"type\":\t%d,\n"
            "\t\t\t\"currpos\":\t%d,\n\t\t\t\"pagestart\":\t%d,\n\t\t\t\"pageend\":\t%d,\n"
            "\t\t\t\"searchstr\":\t%s", s ? 23 : 34, r.search_focus, r.search_first, end, query);
        failed |= state_append(&text, &capacity, &n, line);
        if (s) {
            if (mainui_json_escape(escaped, sizeof(escaped), s->launch, strlen(s->launch)) < 0) goto refused;
            snprintf(line, sizeof(line), ",\n\t\t\t\"emulaunch\":\t%s", escaped);
            failed |= state_append(&text, &capacity, &n, line);
        }
        if (brickos_metadata && r.search_focus >= 0 && r.search_focus < fe->hit_count) {
            const struct mainui_game *game = mainui_frontend_search_game(fe, r.search_focus);
            const struct mainui_system *system = mainui_frontend_system(fe, fe->hits[r.search_focus].system);
            if (game && system) {
                mainui_catalog_game_paths(system, game, focus_path, sizeof(focus_path), image_path, sizeof(image_path));
                if (mainui_json_escape(focus_escaped, sizeof(focus_escaped), focus_path, strlen(focus_path)) < 0)
                    goto refused;
                snprintf(line, sizeof(line), ",\n\t\t\t\"brickos_focus_path\":\t%s", focus_escaped);
                failed |= state_append(&text, &capacity, &n, line);
            }
        }
        failed |= state_append(&text, &capacity, &n, "\n\t\t}");
    }
    failed |= state_append(&text, &capacity, &n, "]\n}");
    if (failed) goto refused;
    path_under(fe, path, destination);
    if (brickos_metadata && state_parent_directories(path) < 0) goto refused;
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *f = fopen(temporary, "wb");
    if (!f) goto refused;
    int ok = fwrite(text, 1, n, f) == n;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(temporary, path) != 0) { remove(temporary); goto refused; }
    free(text);
    return 0;
refused:
    free(text);
    return -1;
}

static int save_state(struct mainui_frontend *fe) {
    return save_state_file(fe, STATE_JSON, 0);
}

static int save_brickos_last_view(struct mainui_frontend *fe) {
    if (!fe || fe->dry || fe->model.presentation != MAINUI_PRESENTATION_MATERIAL_YOU) return 0;
    struct mainui_resume resume;
    if (mainui_model_resume_state(&fe->model, &resume) < 0) return 0;
    return save_state_file(fe, BRICKOS_LAST_VIEW_JSON, 1);
}

static int state_integer(const struct mainui_json_member *value, int fallback) {
    return value && value->type == MAINUI_JSON_NUMBER && value->number >= INT_MIN && value->number <= INT_MAX
        ? (int)value->number : fallback;
}

static int saved_absolute_path(const char *path) {
    if (!path || path[0] != '/' || !path[1]) return 0;
    const char *part = path + 1;
    for (const char *p = part;; ++p) {
        if (*p != '/' && *p != 0) continue;
        size_t length = (size_t)(p - part);
        if ((length == 1 && part[0] == '.') || (length == 2 && part[0] == '.' && part[1] == '.')) return 0;
        if (!*p) break;
        if (p[1] == '/') return 0;
        part = p + 1;
    }
    return 1;
}

static int saved_identity_path(const char *path) {
    return path && path[0] == '/' && !strchr(path, '\n') && !strchr(path, '\r');
}

static int read_tab_state(void *context, const char *text, size_t length) {
    struct mainui_resume *r = context;
    struct mainui_json json;
    if (r->tab_count == MAINUI_TAB_COUNT || mainui_json_parse(&json, text, length) < 0) return -1;
    const struct mainui_json_member *focus = mainui_json_get(&json, "currpos"), *start = mainui_json_get(&json, "pagestart");
    r->tabs[r->tab_count].focus = state_integer(focus, 0);
    r->tabs[r->tab_count].first = state_integer(start, 0);
    ++r->tab_count;
    return 0;
}

// Reads supported base, list and search windows; other stock windows are skipped.
static int read_window(void *context, const char *text, size_t length) {
    struct mainui_frontend *fe = context;
    struct mainui_resume *r = &fe->resume;
    struct mainui_json json;
    const char *tabstate;
    size_t tabstate_length;
    if (mainui_json_parse(&json, text, length) < 0) return 0;
    const struct mainui_json_member *type = mainui_json_get(&json, "type");
    if (!type || type->type != MAINUI_JSON_NUMBER) return 0;
    int kind = state_integer(type, -1);
    if (kind == 49) {
        r->home = 1;
        r->home_focus = state_integer(mainui_json_get(&json, "currpos"), 0);
        r->home_first = state_integer(mainui_json_get(&json, "pagestart"), 0);
        r->home_gameswitcher = state_integer(mainui_json_get(&json, "gameswitcher"), 0) == 1;
        fe->resume_found = 1;
    } else if (kind == 0) {
        r->home = 0;
        const struct mainui_json_member *index = mainui_json_get(&json, "tabidx");
        r->tab_index = state_integer(index, 0);
        r->tab_count = 0;
        if (mainui_json_find(text, length, "tabstate", &tabstate, &tabstate_length) == 0)
            mainui_json_array_each(tabstate, tabstate_length, read_tab_state, r);
        fe->resume_found = 1;
    } else if (kind == 26) {
        if (state_integer(mainui_json_get(&json, "title"), 134) != 134) return 0;
        r->keyboard = 1;
        r->search_system = r->system;
    } else if (kind == 4) {
        const struct mainui_json_member *title = mainui_json_get(&json, "title"), *focus = mainui_json_get(&json, "currpos"),
                                        *start = mainui_json_get(&json, "pagestart"), *ppath = mainui_json_get(&json, "ppath"),
                                        *root = mainui_json_get(&json, "brickos_files_root"),
                                        *name = mainui_json_get(&json, "brickos_focus_name");
        fe->resume_files = 1;
        fe->resume_file_title = state_integer(title, 225);
        fe->resume_file_focus = state_integer(focus, 0);
        fe->resume_file_first = state_integer(start, 0);
        fe->resume_focus_path[0] = 0;
        fe->resume_file_has_paths = 0;
        if (ppath && root && name && ppath->type == MAINUI_JSON_STRING && root->type == MAINUI_JSON_STRING &&
            name->type == MAINUI_JSON_STRING && ppath->text_length < sizeof(fe->resume_file_path) &&
            root->text_length < sizeof(fe->resume_file_root) && name->text_length < sizeof(fe->resume_file_focus_name) &&
            strlen(ppath->text) == ppath->text_length && strlen(root->text) == root->text_length &&
            strlen(name->text) == name->text_length && saved_absolute_path(ppath->text) && saved_absolute_path(root->text)) {
            memcpy(fe->resume_file_path, ppath->text, ppath->text_length + 1);
            memcpy(fe->resume_file_root, root->text, root->text_length + 1);
            memcpy(fe->resume_file_focus_name, name->text, name->text_length + 1);
            fe->resume_file_has_paths = 1;
        }
        if (!r->rom_count) r->system = -1;
    } else if (kind == 23 || kind == 34 || kind == 46) {
        const struct mainui_json_member *launch = mainui_json_get(&json, "emulaunch"), *focus = mainui_json_get(&json, "currpos"),
                                        *start = mainui_json_get(&json, "pagestart");
        enum mainui_tab tabs[MAINUI_TAB_COUNT];
        int n = mainui_model_visible_tabs(&fe->model, tabs);
        int best = r->tab_index >= 0 && r->tab_index < n && tabs[r->tab_index] == MAINUI_TAB_BESTS;
        int system = -1;
        for (int v = 0; !best && launch && launch->type == MAINUI_JSON_STRING && v < fe->visible_count; ++v)
            if (strcmp(fe->systems[fe->visible[v]].launch, launch->text) == 0) system = v;
        for (int v = 0; best && launch && launch->type == MAINUI_JSON_STRING && v < fe->shown_best_count; ++v)
            if (strcmp(fe->bests[fe->shown_bests[v]].launch, launch->text) == 0) system = MAINUI_BEST_SYSTEM_BASE + v;
        const struct mainui_json_member *query = mainui_json_get(&json, "searchstr");
        const struct mainui_json_member *stable = mainui_json_get(&json, "brickos_focus_path");
        fe->resume_focus_path[0] = 0;
        if (stable && stable->type == MAINUI_JSON_STRING && stable->text_length < sizeof(fe->resume_focus_path) &&
            strlen(stable->text) == stable->text_length && saved_identity_path(stable->text))
            memcpy(fe->resume_focus_path, stable->text, stable->text_length + 1);
        if (query || kind != 23) {
            // A lost scoped launcher must not silently broaden the saved query to all systems.
            if (!query || query->type != MAINUI_JSON_STRING || query->text_length >= sizeof(r->query) ||
                strlen(query->text) != query->text_length || (kind != 34 && system < 0)) return 0;
            r->search = 1;
            r->search_system = kind == 34 ? -1 : system;
            memcpy(r->query, query->text, query->text_length + 1);
            r->search_focus = state_integer(focus, 0);
            r->search_first = state_integer(start, 0);
        } else {
            const struct mainui_json_member *ppath = mainui_json_get(&json, "ppath");
            if (system < 0 || r->rom_count == MAINUI_STACK_MAX - 1 ||
                (ppath && (ppath->type != MAINUI_JSON_STRING || ppath->text_length >= sizeof(r->roms[0].path) ||
                 strlen(ppath->text) != ppath->text_length || !rom_path_valid(fe, system, ppath->text)))) return 0;
            int at = r->rom_count++;
            r->roms[at].system = system;
            r->roms[at].focus = state_integer(focus, 0); r->roms[at].first = state_integer(start, 0);
            if (ppath) memcpy(r->roms[at].path, ppath->text, ppath->text_length + 1);
            if (!at) {
                r->system = system;
                r->focus = state_integer(focus, 0);
                r->first = state_integer(start, 0);
            }
        }
    }
    return 0;
}

static int restore_stable_focus(struct mainui_frontend *fe) {
    if (!fe->resume_focus_path[0] || fe->model.depth <= 0) return 0;
    struct mainui_window *w = &fe->model.stack[fe->model.depth - 1];
    int selected = -1, count = 0;
    char rompath[1024], imagepath[1024];
    if (w->kind == MAINUI_WIN_ROMS) {
        const struct mainui_system *system = mainui_frontend_system(fe, w->system);
        count = mainui_frontend_roms(fe, w->system);
        if (system) for (int i = 0; i < count; ++i) {
            mainui_catalog_game_paths(system, &fe->games[i], rompath, sizeof(rompath), imagepath, sizeof(imagepath));
            if (!strcmp(rompath, fe->resume_focus_path)) { selected = i; break; }
        }
    } else if (w->kind == MAINUI_WIN_SEARCH) {
        count = fe->hit_count;
        for (int i = 0; i < count; ++i) {
            const struct mainui_game *game = mainui_frontend_search_game(fe, i);
            const struct mainui_system *system = mainui_frontend_system(fe, fe->hits[i].system);
            if (!game || !system) continue;
            mainui_catalog_game_paths(system, game, rompath, sizeof(rompath), imagepath, sizeof(imagepath));
            if (!strcmp(rompath, fe->resume_focus_path)) { selected = i; break; }
        }
    } else return 0;
    w->focus = selected >= 0 ? selected : 0;
    if (w->first < 0 || w->first > w->focus) w->first = 0;
    int rows = w->kind == MAINUI_WIN_SEARCH ? mainui_search_rows(w->system) : MAINUI_ROM_LIST_ROWS;
    if (w->focus >= w->first + rows) w->first = w->focus - rows + 1;
    int last = count > rows ? count - rows : 0;
    if (w->first > last) w->first = last;
    return 1;
}

static int restore_brickos_files(struct mainui_frontend *fe) {
    if (!fe->resume_files) return 0;
    if (!fe->resume_file_has_paths) {
        if (mainui_frontend_browse(fe, NULL, fe->resume_file_title) < 0) return 0;
        struct mainui_window *w = &fe->model.stack[fe->model.depth - 1];
        const struct mainui_file_folder *folder = mainui_frontend_file_folder(fe, 0);
        if (folder && folder->count > 0 && fe->resume_file_focus >= 0 && fe->resume_file_focus < folder->count)
            w->focus = fe->resume_file_focus;
        if (fe->resume_file_first >= 0 && fe->resume_file_first <= w->focus) w->first = fe->resume_file_first;
        if (w->focus >= w->first + MAINUI_ROM_LIST_ROWS) w->first = w->focus - MAINUI_ROM_LIST_ROWS + 1;
        return 1;
    }

    char root[1024];
    snprintf(root, sizeof(root), "%s", fe->resume_file_root);
    while (mainui_frontend_browse(fe, root, fe->resume_file_title) < 0) {
        size_t length = strlen(root);
        while (length > 1 && root[length - 1] == '/') root[--length] = 0;
        if (length <= 1) return 0;
        char *slash = strrchr(root, '/');
        if (!slash || slash == root) { strcpy(root, "/"); }
        else *slash = 0;
    }

    const char *target = fe->resume_file_path;
    size_t root_length = strlen(root);
    if (strncmp(target, root, root_length) ||
        (target[root_length] && root[root_length - 1] != '/' && target[root_length] != '/')) return 0;
    const char *part = target + root_length;
    while (*part == '/') ++part;
    int reached = 1;
    while (*part) {
        const char *end = strchr(part, '/');
        size_t component_length = end ? (size_t)(end - part) : strlen(part);
        if (!component_length || component_length >= sizeof(fe->resume_file_focus_name)) { reached = 0; break; }
        char component[256], next[1024];
        memcpy(component, part, component_length); component[component_length] = 0;
        const struct mainui_file_folder *folder = mainui_frontend_file_folder(fe, fe->file_depth - 1);
        struct mainui_window *w = &fe->model.stack[fe->model.depth - 1];
        int row = -1;
        if (folder) for (int i = 0; i < folder->count; ++i)
            if (folder->files[i].folder && !strcmp(folder->files[i].name, component)) { row = i; break; }
        if (row < 0 || snprintf(next, sizeof(next), "%s%s%s", folder->path,
                                folder->path[strlen(folder->path) - 1] == '/' ? "" : "/", component) >= (int)sizeof(next)) {
            reached = 0; break;
        }
        w->focus = row;
        if (row < w->first || row >= w->first + MAINUI_ROM_LIST_ROWS)
            w->first = row >= MAINUI_ROM_LIST_ROWS ? row - MAINUI_ROM_LIST_ROWS + 1 : 0;
        int previous_depth = fe->file_depth;
        enum mainui_key confirm = fe->model.settings[MAINUI_SET_BCONFIRM] ? MAINUI_KEY_B : MAINUI_KEY_A;
        (void)mainui_frontend_key(fe, confirm);
        folder = mainui_frontend_file_folder(fe, fe->file_depth - 1);
        if (fe->file_depth != previous_depth + 1 || !folder || strcmp(folder->path, next)) { reached = 0; break; }
        part += component_length;
        while (*part == '/') ++part;
    }

    struct mainui_file_folder *folder = (struct mainui_file_folder *)mainui_frontend_file_folder(fe, fe->file_depth - 1);
    struct mainui_window *w = &fe->model.stack[fe->model.depth - 1];
    int focus = -1;
    if (reached && fe->resume_file_focus_name[0] && folder)
        for (int i = 0; i < folder->count; ++i)
            if (!strcmp(folder->files[i].name, fe->resume_file_focus_name)) { focus = i; break; }
    if (focus >= 0) w->focus = focus;
    else if (!reached || fe->resume_file_focus_name[0]) w->focus = 0;
    else if (folder && folder->count > 0 && fe->resume_file_focus >= 0 && fe->resume_file_focus < folder->count)
        w->focus = fe->resume_file_focus;
    else w->focus = 0;
    w->first = reached && fe->resume_file_first >= 0 && fe->resume_file_first <= w->focus
        ? fe->resume_file_first : 0;
    if (w->focus >= w->first + MAINUI_ROM_LIST_ROWS) w->first = w->focus - MAINUI_ROM_LIST_ROWS + 1;
    int last = folder && folder->count > MAINUI_ROM_LIST_ROWS ? folder->count - MAINUI_ROM_LIST_ROWS : 0;
    if (w->first > last) w->first = last;
    return 1;
}

// Restores the stack a launch left, then removes the file as the official MainUI does. A dry
// run only reads it.
static void load_state(struct mainui_frontend *fe) {
    char path[PATH_BYTES];
    const char *list;
    size_t list_length;
    path_under(fe, path, STATE_JSON);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *text = malloc(STATE_BYTES);
    size_t n = text ? fread(text, 1, STATE_BYTES - 1, f) : 0;
    fclose(f);
    if (!text) return;
    text[n] = 0;
    if (mainui_brickos_return_digest(text, n, fe->brickos_state_digest) < 0)
        fe->brickos_state_digest[0] = 0;
    memset(&fe->resume, 0, sizeof(fe->resume));
    fe->resume.system = fe->resume.search_system = -1;
    fe->resume_found = 0;
    fe->resume_files = 0;
    fe->resume_file_has_paths = 0;
    fe->resume_focus_path[0] = 0;
    if (mainui_json_find(text, n, "list", &list, &list_length) == 0 &&
        mainui_json_array_each(list, list_length, read_window, fe) < 0)
        fe->resume_found = fe->resume_files = 0;
    free(text);
    if (fe->resume_found) {
        if (fe->resume.search) search(fe, fe->resume.query, fe->resume.search_system);
        mainui_model_resume(&fe->model, &fe->resume);
    }
    // Stock type 4 always reconstructs its fixed initial root. It saves positions,
    // but neither the current FileBrowser path nor its folder history.
    if (fe->resume_files && mainui_frontend_browse(fe, NULL, fe->resume_file_title) == 0) {
        struct mainui_window *w = &fe->model.stack[fe->model.depth - 1];
        const struct mainui_file_folder *folder = mainui_frontend_file_folder(fe, 0);
        if (fe->resume_file_focus >= 0 && fe->resume_file_focus < folder->count) w->focus = fe->resume_file_focus;
        if (fe->resume_file_first >= 0 && fe->resume_file_first <= w->focus) w->first = fe->resume_file_first;
        if (w->focus >= w->first + MAINUI_ROM_LIST_ROWS) w->first = w->focus - MAINUI_ROM_LIST_ROWS + 1;
        fe->resume_found = 1;
    }
    note(fe, "state restored=%d tab=%d system=%d focus=%d", fe->resume_found, fe->resume.tab_index, fe->resume.system,
         fe->resume.focus);
    if (!fe->dry) remove(path);
}

static int load_brickos_last_view(struct mainui_frontend *fe) {
    char path[PATH_BYTES];
    path_under(fe, path, BRICKOS_LAST_VIEW_JSON);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size >= STATE_BYTES) {
        close(fd);
        return 0;
    }
    char *text = malloc(STATE_BYTES);
    if (!text) { close(fd); return 0; }
    size_t length = 0;
    while (length < (size_t)st.st_size) {
        ssize_t n = read(fd, text + length, (size_t)st.st_size - length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(text); close(fd); return 0; }
        length += (size_t)n;
    }
    close(fd);
    text[length] = 0;
    const char *version, *presentation, *list;
    size_t version_length, presentation_length, list_length;
    int valid = mainui_json_find(text, length, "version", &version, &version_length) == 0 &&
                version_length == 1 && version[0] == '1' &&
                mainui_json_find(text, length, "presentation", &presentation, &presentation_length) == 0 &&
                presentation_length == 14 && memcmp(presentation, "\"material-you\"", 14) == 0 &&
                mainui_json_find(text, length, "list", &list, &list_length) == 0;
    if (!valid) { free(text); return 0; }
    struct mainui_resume previous_resume = fe->resume;
    int previous_found = fe->resume_found, previous_files = fe->resume_files;
    int previous_file_paths = fe->resume_file_has_paths;
    int previous_file_title = fe->resume_file_title, previous_file_focus = fe->resume_file_focus,
        previous_file_first = fe->resume_file_first;
    char previous_file_root[sizeof(fe->resume_file_root)], previous_file_path[sizeof(fe->resume_file_path)],
         previous_file_name[sizeof(fe->resume_file_focus_name)], previous_focus_path[sizeof(fe->resume_focus_path)];
    memcpy(previous_file_root, fe->resume_file_root, sizeof(previous_file_root));
    memcpy(previous_file_path, fe->resume_file_path, sizeof(previous_file_path));
    memcpy(previous_file_name, fe->resume_file_focus_name, sizeof(previous_file_name));
    memcpy(previous_focus_path, fe->resume_focus_path, sizeof(previous_focus_path));
    memset(&fe->resume, 0, sizeof(fe->resume));
    fe->resume.system = fe->resume.search_system = -1;
    fe->resume_found = 0;
    fe->resume_files = 0;
    fe->resume_file_has_paths = 0;
    fe->resume_focus_path[0] = 0;
    if (mainui_json_array_each(list, list_length, read_window, fe) < 0 || !fe->resume_found) {
        fe->resume = previous_resume;
        fe->resume_found = previous_found; fe->resume_files = previous_files;
        fe->resume_file_has_paths = previous_file_paths;
        fe->resume_file_title = previous_file_title; fe->resume_file_focus = previous_file_focus;
        fe->resume_file_first = previous_file_first;
        memcpy(fe->resume_file_root, previous_file_root, sizeof(previous_file_root));
        memcpy(fe->resume_file_path, previous_file_path, sizeof(previous_file_path));
        memcpy(fe->resume_file_focus_name, previous_file_name, sizeof(previous_file_name));
        memcpy(fe->resume_focus_path, previous_focus_path, sizeof(previous_focus_path));
        free(text);
        return 0;
    }
    enum mainui_tab tabs[MAINUI_TAB_COUNT];
    int tab_count = mainui_model_visible_tabs(&fe->model, tabs);
    if (!tab_count || fe->resume.tab_index < 0 || fe->resume.tab_index >= tab_count) {
        fe->resume = previous_resume;
        fe->resume_found = previous_found; fe->resume_files = previous_files;
        fe->resume_file_has_paths = previous_file_paths;
        fe->resume_file_title = previous_file_title; fe->resume_file_focus = previous_file_focus;
        fe->resume_file_first = previous_file_first;
        memcpy(fe->resume_file_root, previous_file_root, sizeof(previous_file_root));
        memcpy(fe->resume_file_path, previous_file_path, sizeof(previous_file_path));
        memcpy(fe->resume_file_focus_name, previous_file_name, sizeof(previous_file_name));
        memcpy(fe->resume_focus_path, previous_focus_path, sizeof(previous_focus_path));
        free(text);
        return 0;
    }
    free(text);
    drop_files(fe);
    if (fe->resume.search) search(fe, fe->resume.query, fe->resume.search_system);
    mainui_model_resume(&fe->model, &fe->resume);
    (void)restore_stable_focus(fe);
    (void)restore_brickos_files(fe);
    note(fe, "brickos last view restored tab=%d system=%d focus=%d", fe->resume.tab_index,
         fe->resume.system, fe->resume.focus);
    return 1;
}

static int host_advertisement(struct mainui_frontend *fe, const struct mainui_list_entry *entry,
                              char *out, size_t size) {
    struct mainui_netplay_host host = {.gametype = entry->type};
    char status[2048];
    if (!fe->runner.capture || fe->runner.capture(fe->runner.context, "ip addr show wlan0", status, sizeof(status)) < 0 ||
        mainui_netplay_address(status, host.hostip, sizeof(host.hostip)) < 0) return -1;
    const char *rom = strstr(entry->rompath, "/mnt/SDCARD/");
    const char *icon = strstr(entry->imgpath, "/mnt/SDCARD/");
    const char *system = entry->sublabel[0] ? entry->sublabel : mainui_frontend_entry_system(fe, entry);
    if (!rom || !system || strlen(entry->label) >= sizeof(host.label) ||
        strlen(rom + 12) >= sizeof(host.rompath) || strlen(system) >= sizeof(host.gamelabel) ||
        (icon && strlen(icon + 12) >= sizeof(host.iconpath))) return -1;
    strcpy(host.label, entry->label);
    strcpy(host.rompath, rom + 12);
    strcpy(host.gamelabel, system);
    if (icon) strcpy(host.iconpath, icon + 12);
    return mainui_netplay_format(&host, out, size);
}

struct frontend_file_snapshot { unsigned char *bytes; size_t length; int present; };

static int frontend_file_read(const char *path, struct frontend_file_snapshot *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size >= STATE_BYTES) {
        close(fd);
        return -1;
    }
    size_t length = (size_t)st.st_size;
    unsigned char *bytes = malloc(length ? length : 1);
    if (!bytes) { close(fd); return -1; }
    size_t used = 0;
    while (used < length) {
        ssize_t n = read(fd, bytes + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(bytes); close(fd); return -1; }
        used += (size_t)n;
    }
    close(fd);
    snapshot->bytes = bytes;
    snapshot->length = length;
    snapshot->present = 1;
    return 0;
}

static void frontend_file_snapshot_free(struct frontend_file_snapshot *snapshot) {
    free(snapshot->bytes);
    memset(snapshot, 0, sizeof(*snapshot));
}

static int frontend_file_replace(const char *path, const void *bytes, size_t length) {
    char temporary[PATH_BYTES + 24];
    if (!path || snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >= (int)sizeof(temporary)) return -1;
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (fchmod(fd, 0600) < 0) { close(fd); unlink(temporary); return -1; }
    size_t written = 0;
    int ok = 1;
    while (written < length) {
        ssize_t n = write(fd, (const unsigned char *)bytes + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = 0; break; }
        written += (size_t)n;
    }
    if (ok && fsync(fd) < 0) ok = 0;
    if (close(fd) < 0) ok = 0;
    if (!ok || rename(temporary, path) < 0) { unlink(temporary); return -1; }
    return 0;
}

static int frontend_file_restore(const char *path, const struct frontend_file_snapshot *snapshot) {
    if (snapshot->present) return frontend_file_replace(path, snapshot->bytes, snapshot->length);
    return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
}

static int gameswitcher_launch(const struct mainui_list_entry *entry);
static void picker_start(struct mainui_frontend *fe, const struct mainui_list_entry *entry, int recent);

// recent 0 leaves Recent alone: GameSwitcher opened as the Default destination at each start.
static void launch_entry_recent(struct mainui_frontend *fe, const struct mainui_list_entry *entry, const char *launcher,
                                const char *hostip, int recent) {
    enum mainui_launch_result result;
    int legacy = entry->type >= 5 && entry->type <= 15;
    int encoded = legacy && !mainui_launch_builtin_core(entry->type);
    // GameSwitcher's app runs over the frontend, unless the picker itself asks for the handoff.
    if (!fe->picker_handoff && gameswitcher_launch(entry)) {
        picker_start(fe, entry, recent);
        return;
    }
    note(fe, "launch label=%s type=%d", entry->label, entry->type);
    // Settings extension scripts end before the launch command is written.
    ext_stop(fe);
    if (encoded && !fe->encoder.block) {
        note(fe, "launch unsupported builtin=%d", entry->type);
        ++fe->effects_failed;
        return;
    }
    if (fe->dry) return;
    if (entry->type == MAINUI_ENTRY_PORT) {
        result = mainui_launch_port(fe->fs.root, entry->launch);
    } else if (entry->type == MAINUI_ENTRY_APP && fe->picker_handoff && fe->picker_id[0]) {
        result = mainui_launch_gameswitcher_resume(fe->fs.root, fe->picker_id, fe->picker_state);
    } else if (entry->type == MAINUI_ENTRY_APP) {
        char directory[1024];
        snprintf(directory, sizeof(directory), "%s", entry->launch);
        char *slash = strrchr(directory, '/');
        if (!slash) {
            note(fe, "launch failed label=%s result=%d", entry->label, MAINUI_LAUNCH_NO_LAUNCHER);
            ++fe->effects_failed;
            return;
        }
        *slash = 0;
        result = mainui_launch_app(fe->fs.root, directory, slash + 1);
    } else if (encoded) {
        result = hostip ? mainui_launch_encoded_netplay(fe->fs.root, entry->rompath, &fe->encoder)
                        : mainui_launch_encoded(fe->fs.root, entry->rompath, &fe->encoder);
    } else if (mainui_launch_builtin_core(entry->type)) {
        char advertisement[1025];
        int hosting = !hostip && fe->settings.values[MAINUI_SET_NETPLAY];
        int length = hosting ? host_advertisement(fe, entry, advertisement, sizeof(advertisement)) : -1;
        result = mainui_launch_builtin(fe->fs.root, entry->type, entry->rompath, hostip, hosting,
                                      length < 0 ? NULL : advertisement, &fe->encoder);
    } else if (hostip) {
        result = mainui_launch_netplay(fe->fs.root, entry->launch, entry->rompath, hostip);
    } else if (fe->settings.values[MAINUI_SET_NETPLAY]) {
        char advertisement[1025];
        int length = host_advertisement(fe, entry, advertisement, sizeof(advertisement));
        result = mainui_launch_host(fe->fs.root, launcher ? launcher : entry->launch, entry->rompath,
                                    length < 0 ? NULL : advertisement);
        note(fe, "netplay host advertised=%d", length >= 0 && result == MAINUI_LAUNCH_OK);
    } else {
        result = mainui_launch_game(fe->fs.root, launcher ? launcher : entry->launch, entry->rompath);
    }
    if (result != MAINUI_LAUNCH_OK) {
        // Stock AppAction/EmuAction request event 64 even if fopen of the command
        // fails, without a failure dialog. Keep its screen and diagnostic path,
        // but preserve a usable frontend and leave history/focus untouched.
        note(fe, "launch failed label=%s result=%d", entry->label, (int)result);
        ++fe->effects_failed;
        return;
    }
    if ((entry->type == MAINUI_ENTRY_GAME || legacy) &&
        mainui_effects_launch_keymap(&fe->ops, fe->settings.values[MAINUI_SET_KEYMAP]) < 0) {
        note(fe, "launch keymap effect failed");
        ++fe->effects_failed;
    }
    const char *run_token = getenv("BRICKOS_MAINUI_TOKEN");
    int tracked_brickos_return = fe->model.presentation == MAINUI_PRESENTATION_MATERIAL_YOU && run_token && run_token[0];
    if (tracked_brickos_return) {
        struct mainui_resume resume;
        char state_path[PATH_BYTES], sidecar_path[PATH_BYTES], view_path[PATH_BYTES],
             digest[MAINUI_BRICKOS_RETURN_DIGEST], view_digest[MAINUI_BRICKOS_RETURN_DIGEST];
        struct frontend_file_snapshot old_state, old_sidecar, old_view, current_state, current_view;
        path_under(fe, state_path, STATE_JSON);
        path_under(fe, sidecar_path, MAINUI_BRICKOS_RETURN_PATH);
        path_under(fe, view_path, BRICKOS_LAST_VIEW_JSON);
        memset(&old_state, 0, sizeof(old_state));
        memset(&old_sidecar, 0, sizeof(old_sidecar));
        memset(&old_view, 0, sizeof(old_view));
        memset(&current_state, 0, sizeof(current_state));
        memset(&current_view, 0, sizeof(current_view));
        if (mainui_model_resume_state(&fe->model, &resume) < 0 ||
            frontend_file_read(state_path, &old_state) < 0 || frontend_file_read(sidecar_path, &old_sidecar) < 0 ||
            frontend_file_read(view_path, &old_view) < 0) {
            mainui_launch_clear(fe->fs.root);
            frontend_file_snapshot_free(&old_state);
            frontend_file_snapshot_free(&old_sidecar);
            frontend_file_snapshot_free(&old_view);
            ++fe->effects_failed;
            note(fe, "brickos return context backup refused");
            return;
        }
        int published = save_brickos_last_view(fe) == 0 && frontend_file_read(view_path, &current_view) == 0 &&
                        current_view.present &&
                        mainui_brickos_return_digest(current_view.bytes, current_view.length, view_digest) == 0 &&
                        save_state(fe) == 0 && frontend_file_read(state_path, &current_state) == 0 &&
                        current_state.present &&
                        mainui_brickos_return_digest(current_state.bytes, current_state.length, digest) == 0 &&
                        mainui_brickos_return_publish_view(sidecar_path, run_token, digest, view_digest) == 0;
        if (!published) {
            int restored = frontend_file_restore(state_path, &old_state) == 0 &&
                           frontend_file_restore(sidecar_path, &old_sidecar) == 0 &&
                           frontend_file_restore(view_path, &old_view) == 0;
            mainui_launch_clear(fe->fs.root);
            ++fe->effects_failed;
            note(fe, "brickos return context publication refused restored=%d", restored);
            frontend_file_snapshot_free(&old_state);
            frontend_file_snapshot_free(&old_sidecar);
            frontend_file_snapshot_free(&old_view);
            frontend_file_snapshot_free(&current_state);
            frontend_file_snapshot_free(&current_view);
            return;
        }
        frontend_file_snapshot_free(&old_state);
        frontend_file_snapshot_free(&old_sidecar);
        frontend_file_snapshot_free(&old_view);
        frontend_file_snapshot_free(&current_state);
        frontend_file_snapshot_free(&current_view);
    } else if (save_state(fe) < 0) {
        note(fe, "state not saved");
    }
    if (recent) {
        mainui_recent_add(&fe->recent, entry);
        save_list(fe, &fe->recent, "/mnt/SDCARD/Roms/recentlist.json");
    }
    fe->exit_requested = 1;
    // Stock main preserves its startup result (zero) across runWindows; the
    // latter handles launch event 64 and does not return it as a process status.
    fe->exit_status = 0;
}

static void launch_entry(struct mainui_frontend *fe, const struct mainui_list_entry *entry, const char *launcher,
                         const char *hostip) {
    launch_entry_recent(fe, entry, launcher, hostip, 1);
}

// ---- GameSwitcher over the frontend --------------------------------------------------------
// MainUI runs GameSwitcher's app as its picker (entry.sh pick), over its own screen, instead of
// handing it to the init and starting again when it closes. B leaves MainUI's screen in place; a
// chosen game is handed to the init as "entry.sh resume ID STATE" (docs/mainui.md).

// GameSwitcher's app, by the folder its launch script is in.
static int gameswitcher_launch(const struct mainui_list_entry *entry) {
    if (entry->type != MAINUI_ENTRY_APP) return 0;
    const char *slash = strrchr(entry->launch, '/');
    size_t length = strlen(GAMESWITCHER_APP);
    return slash && (size_t)(slash - entry->launch) == length && strncmp(entry->launch, GAMESWITCHER_APP, length) == 0;
}

// The picker names a chosen checkpoint in one line; any other line is a message of its own.
static void picker_on_line(void *context, struct mainui_ext_job *job, const char *line) {
    struct mainui_frontend *fe = context;
    char id[64], state[64], rest[8];
    (void)job;
    if (sscanf(line, "resume %63s %63s %7s", id, state, rest) == 2 && mainui_launch_gameswitcher_names(id, state)) {
        // The names are 32 and 38 characters long.
        memcpy(fe->picker_id, id, 33);
        memcpy(fe->picker_state, state, 39);
        return;
    }
    ++fe->picker_lines;
    note(fe, "gameswitcher picker said %s", line);
}

static void picker_on_exit(void *context, struct mainui_ext_job *job) {
    struct mainui_frontend *fe = context;
    fe->picker_job = 0;
    fe->picker_ended = 1;
    fe->picker_status = job->status;
    note(fe, "gameswitcher picker ended status=%d", job->status);
}

// The app goes to the init as before: when the picker cannot run, or the entry has none.
static void picker_handover(struct mainui_frontend *fe, const char *why) {
    note(fe, "gameswitcher picker %s; the app goes to the init", why);
    fe->picker_handoff = 1;
    fe->picker_id[0] = fe->picker_state[0] = 0;
    launch_entry_recent(fe, &fe->picker_entry, NULL, NULL, 0);
    fe->picker_handoff = 0;
}

static void picker_start(struct mainui_frontend *fe, const struct mainui_list_entry *entry, int recent) {
    char dir[PATH_BYTES], entry_path[PATH_BYTES], command[2 * PATH_BYTES + 16];
    if (fe->picker_job) return;
    fe->picker_entry = *entry;
    fe->picker_id[0] = fe->picker_state[0] = 0;
    fe->picker_ended = fe->picker_lines = fe->picker_status = 0;
    if (fe->dry) {
        note(fe, "gameswitcher picker (dry)");
        return;
    }
    // Opening GameSwitcher's app adds it to Recent, as its launch did.
    if (recent) {
        mainui_recent_add(&fe->recent, entry);
        save_list(fe, &fe->recent, "/mnt/SDCARD/Roms/recentlist.json");
    }
    path_under(fe, dir, "/mnt/SDCARD/BrickOS/gameswitcher");
    path_under(fe, entry_path, MAINUI_GAMESWITCHER_ENTRY);
    snprintf(command, sizeof(command), "exec sh ");
    int id = -1;
    if (access(entry_path, F_OK) == 0 && mainui_shell_quote(command, sizeof(command), entry_path) == 0 &&
        strlen(command) + 6 < sizeof(command)) {
        strcat(command, " pick");
        id = mainui_ext_runner_start(&fe->picker_runner, MAINUI_RUN_CMD, -1, 0, dir, command, NULL, "gameswitcher pick", 0);
    }
    if (id <= 0) {
        picker_handover(fe, "cannot start");
        return;
    }
    fe->picker_job = id;
    note(fe, "gameswitcher picker started");
}

int mainui_frontend_picker_active(const struct mainui_frontend *fe) {
    return fe->picker_job != 0;
}

int mainui_frontend_picker_poll(struct mainui_frontend *fe, uint64_t now_ms) {
    if (!fe->picker_job && !fe->picker_ended) return 0;
    mainui_ext_runner_poll(&fe->picker_runner, now_ms);
    if (!fe->picker_ended) return 0;
    fe->picker_ended = 0;
    if (fe->picker_status == 0 && fe->picker_id[0]) {
        // A chosen game: the init resumes it, as after a launch.
        fe->picker_handoff = 1;
        launch_entry_recent(fe, &fe->picker_entry, NULL, NULL, 0);
        fe->picker_handoff = 0;
        fe->picker_id[0] = fe->picker_state[0] = 0;
    } else if (fe->picker_status == 2 && !fe->picker_lines) {
        // An entry from before the picker answers its unknown mode with status 2.
        picker_handover(fe, "not in this GameSwitcher");
    } else if (fe->picker_status != 0) {
        ++fe->effects_failed;
    }
    return 1;
}

void mainui_frontend_picker_cancel(struct mainui_frontend *fe, uint64_t now_ms) {
    if (fe->picker_job) mainui_ext_runner_cancel(&fe->picker_runner, fe->picker_job, now_ms);
}

static const struct mainui_system *host_system(const struct mainui_frontend *fe, const struct mainui_netplay_host *host) {
    if (host->gametype != MAINUI_ENTRY_GAME && (host->gametype < 5 || host->gametype > 15)) return NULL;
    size_t length = strlen(host->gamelabel);
    for (int i = 0; i < fe->system_count; ++i)
        if (strncmp(fe->systems[i].label, host->gamelabel, length) == 0) return &fe->systems[i];
    for (int i = 0; i < fe->best_count; ++i)
        if (strncmp(fe->bests[i].label, host->gamelabel, length) == 0) return &fe->bests[i];
    static const struct mainui_system builtin[MAINUI_NETPLAY_BUILTIN_LABELS] = {
        {.label = "Arcade"}, {.label = "NEOGEO"}, {.label = "FC"}, {.label = "SFC"}, {.label = "GB"},
        {.label = "MD"}, {.label = "GBA"}, {.label = "SMS"}, {.label = "PCE"}, {.label = "PS"}
    };
    for (int core = 0; core < MAINUI_NETPLAY_BUILTIN_LABELS; ++core)
        if (!strncmp(builtin[core].label, host->gamelabel, length)) return &builtin[core];
    // HallMenu also looks up the advertised label across every installed language.
    // This fallback is exact, whereas catalog labels use the stock prefix comparison.
    for (int language = 0; language < fe->language_count; ++language)
        for (int core = 0; core < MAINUI_NETPLAY_BUILTIN_LABELS; ++core)
            if (fe->netplay_builtin_labels[language][core][0] &&
                !strcmp(fe->netplay_builtin_labels[language][core], host->gamelabel)) return &builtin[core];
    return NULL;
}

// The list entry an effect names, for launches and favorites.
static int effect_entry(struct mainui_frontend *fe, const struct mainui_effect *e, struct mainui_list_entry *out) {
    const struct mainui_list_entry *entry = NULL;
    switch (e->list) {
    case MAINUI_LIST_NETPLAY: {
        if (e->index < 0 || e->index >= fe->host_count) return -1;
        const struct mainui_netplay_host *h = &fe->hosts[e->index];
        const struct mainui_system *s = host_system(fe, h);
        char rompath[1024], imgpath[1024];
        if (!s || snprintf(rompath, sizeof(rompath), "/mnt/SDCARD/%s", h->rompath) >= (int)sizeof(rompath)) return -1;
        mainui_netplay_cover_path(fe->fs.root, h->iconpath, imgpath, sizeof(imgpath));
        mainui_list_entry_set(out, h->label, rompath, imgpath, h->gametype == MAINUI_ENTRY_GAME ? s->launch : "", s->label, h->gametype);
        return 0;
    }
    case MAINUI_LIST_ROMS: return game_entry(fe, e->system, e->index, out);
    case MAINUI_LIST_RECENT: entry = mainui_frontend_recent(fe, e->index); break;
    case MAINUI_LIST_FAVORITES: entry = mainui_frontend_favorite(fe, e->index); break;
    case MAINUI_LIST_SEARCH:
        if (e->index < 0 || e->index >= fe->hit_count) return -1;
        return game_value_entry(fe, fe->hits[e->index].system, mainui_frontend_search_game(fe, e->index), out);
    case MAINUI_LIST_APPS: {
        if (e->index < 0 || e->index >= fe->shown_app_count) return -1;
        const struct mainui_app *app = &fe->apps[fe->shown_apps[e->index]];
        char launch[1024];
        snprintf(launch, sizeof(launch), "%s/%s", app->directory, app->launch);
        mainui_list_entry_set(out, app->label, "", "", launch, "", MAINUI_ENTRY_APP);
        return 0;
    }
    case MAINUI_LIST_PORTS: {
        if (e->index < 0 || e->index >= fe->shown_port_count) return -1;
        const struct mainui_app *port = &fe->ports[fe->shown_ports[e->index]];
        mainui_list_entry_set(out, port->label, "", "", port->launch, "", MAINUI_ENTRY_PORT);
        return 0;
    }
    default: return -1;
    }
    if (!entry) return -1;
    *out = *entry;
    out->line = NULL;
    // A recent line has no sublabel; the system its launcher belongs to gives it, as the
    // official Add Favorite from Recent wrote "sublabel":"FC" (l4a), and an app's or a port's
    // config description gives it theirs (p7).
    const struct mainui_app *app = out->sublabel[0] ? NULL : mainui_frontend_entry_app(fe, out);
    const char *system = out->sublabel[0] ? NULL : app ? app->description : mainui_frontend_entry_system(fe, out);
    if (system) {
        size_t n = strlen(system);
        if (n >= sizeof(out->sublabel)) n = sizeof(out->sublabel) - 1;
        memcpy(out->sublabel, system, n);
        out->sublabel[n] = 0;
    }
    return 0;
}

static const struct mainui_system *history_system(const struct mainui_frontend *fe,
                                                 const struct mainui_list_entry *e) {
    if (e->type != MAINUI_ENTRY_GAME) return NULL;
    // Stock Recent/Favorites compare the full configured launch string as a
    // prefix, trying regular systems first and Bests only when none matched.
    for (int i = 0; i < fe->system_count; ++i) {
        size_t length = strlen(fe->systems[i].launch);
        if (length && !strncmp(e->launch, fe->systems[i].launch, length)) return &fe->systems[i];
    }
    for (int i = 0; i < fe->best_count; ++i) {
        size_t length = strlen(fe->bests[i].launch);
        if (length && !strncmp(e->launch, fe->bests[i].launch, length)) return &fe->bests[i];
    }
    return NULL;
}

const char *mainui_frontend_entry_system(const struct mainui_frontend *fe, const struct mainui_list_entry *e) {
    if (MAINUI_ENTRY_SCRIPT(e->type)) return NULL;
    if (mainui_launch_builtin_core(e->type)) {
        const char *translated = mainui_lang_get(&fe->lang, e->type - 2);
        return translated && translated[0] ? translated : e->type == 5 ? "Arcade" : e->type == 6 ? "NEOGEO" : "FC";
    }
    const struct mainui_system *system = history_system(fe, e);
    return system ? system->label : NULL;
}

const char *mainui_frontend_entry_label(const struct mainui_frontend *fe, const struct mainui_list_entry *e) {
    if (MAINUI_ENTRY_SCRIPT(e->type)) return e->label;
    // Legacy types select their own built-in EmuItem, never a configured system
    // merely sharing a launcher. No active stock built-in configuration source
    // is established, so its shortname flag is not inferred from numeric type.
    const struct mainui_system *system = history_system(fe, e);
    if (system && system->shortname) {
        const char *label = mainui_names_label(&fe->names, fe->settings.language, e->label);
        if (label && label[0]) return label;
    }
    return e->label;
}

const struct mainui_app *mainui_frontend_entry_app(const struct mainui_frontend *fe, const struct mainui_list_entry *e) {
    if (e->type == MAINUI_ENTRY_PORT) {
        for (int i = 0; i < fe->port_count; ++i)
            if (strcmp(e->launch, fe->ports[i].launch) == 0) return &fe->ports[i];
        return NULL;
    }
    if (e->type != MAINUI_ENTRY_APP) return NULL;
    for (int i = 0; i < fe->app_count; ++i) {
        size_t length = strlen(fe->apps[i].directory);
        if (strncmp(e->launch, fe->apps[i].directory, length) == 0 && e->launch[length] == '/') return &fe->apps[i];
    }
    return NULL;
}

static int contains_case(const char *value, const char *text) {
    size_t length = strlen(text);
    for (const char *p = value; *p; ++p) if (!strncasecmp(p, text, length)) return 1;
    return 0;
}

struct search_visit {
    struct mainui_frontend *fe;
    const char *query;
    int system;
};

static int search_game(void *context, const struct mainui_game *game) {
    struct search_visit *visit = context;
    struct mainui_frontend *fe = visit->fe;
    if (game->folder || !(contains_case(mainui_catalog_game_label(game), visit->query) ||
        contains_case(game->label, visit->query) || contains_case(game->pinyin, visit->query) ||
        contains_case(game->opinyin, visit->query))) return 0;
    if (fe->hit_count == fe->hit_capacity) {
        if (fe->hit_count == INT_MAX) return -1;
        int capacity = fe->hit_capacity > INT_MAX / 2 ? INT_MAX : fe->hit_capacity ? fe->hit_capacity * 2 : 16;
        if ((size_t)capacity > SIZE_MAX / sizeof(*fe->hit_games) ||
            (size_t)capacity > SIZE_MAX / sizeof(*fe->hits)) return -1;
        struct mainui_game *grown = realloc(fe->hit_games, (size_t)capacity * sizeof(*grown));
        if (!grown) return -1;
        fe->hit_games = grown;
        struct mainui_search_hit *hits = realloc(fe->hits, (size_t)capacity * sizeof(*hits));
        if (!hits) return -1;
        fe->hits = hits; fe->hit_capacity = capacity;
    }
    int index = fe->hit_count;
    fe->hit_games[index] = *game;
    fe->hits[index] = (struct mainui_search_hit){visit->system, index};
    ++fe->hit_count;
    return 0;
}

// Stable original-label ordering retains discovery order for equal labels.
// Merge sorted runs instead of shifting every earlier hit for large catalogs.
static int sort_search(struct mainui_frontend *fe, int start) {
    size_t count = (size_t)(fe->hit_count - start);
    if (count < 2) return 0;
    struct mainui_search_hit *temporary = malloc(count * sizeof(*temporary));
    if (!temporary) return -1;
    struct mainui_search_hit *rows = fe->hits + start;
    for (size_t width = 1; width < count; width *= 2) {
        for (size_t left = 0; left < count; left += width * 2) {
            size_t middle = left + width < count ? left + width : count;
            size_t end = middle + width < count ? middle + width : count;
            size_t a = left, b = middle;
            for (size_t out = left; out < end; ++out) {
                if (a < middle && (b == end || strcmp(fe->hit_games[rows[a].game].label,
                                                      fe->hit_games[rows[b].game].label) <= 0))
                    temporary[out] = rows[a++];
                else temporary[out] = rows[b++];
            }
        }
        memcpy(rows, temporary, count * sizeof(*rows));
    }
    free(temporary);
    return 0;
}

static void search(struct mainui_frontend *fe, const char *text, int system) {
    fe->hit_count = 0;
    if (!text[0]) return;
    for (int at = 0; at < fe->visible_count + fe->shown_best_count; ++at) {
        int v = at < fe->visible_count ? at : MAINUI_BEST_SYSTEM_BASE + at - fe->visible_count;
        if (system >= 0 && v != system) continue;
        int start = fe->hit_count;
        struct search_visit visit = {fe, text, v};
        if (mainui_catalog_walk_games(&fe->fs, mainui_frontend_system(fe, v), &fe->names,
                                     fe->settings.language, search_game, &visit) < 0 || sort_search(fe, start) < 0) {
            // A failed allocation must not leave a partial result set presented
            // as a complete search; the editor can submit again after recovery.
            fe->hit_count = 0; ++fe->effects_failed; note(fe, "search catalog failed system=%d", v);
            break;
        }
    }
}

// Each provider root uses stock JSON; internal Apps keep their override on SD.
static void save_show_root(struct mainui_frontend *fe, int tab, int stock) {
    int count = tab == MAINUI_TAB_APPS ? fe->app_count : fe->system_count;
    int editable = count;
    if (tab == MAINUI_TAB_APPS) {
        editable = 0;
        for (int i = 0; i < count; ++i) editable += fe->apps[i].stock == stock;
    }
    // Stock does not publish an empty provider.
    if (!editable) return;
    char path[PATH_BYTES], temporary[PATH_BYTES + 16];
    if (tab == MAINUI_TAB_APPS && stock) {
        path_under(fe, path, "/mnt/SDCARD/Apps");
        if (!path[0] || (mkdir(path, 0755) != 0 && errno != EEXIST)) { ++fe->effects_failed; return; }
    }
    path_under(fe, path, tab == MAINUI_TAB_APPS ? (stock ? MAINUI_STOCK_APPS_SHOW_FILE : "/mnt/SDCARD/Apps/show.json") : "/mnt/SDCARD/Emus/show.json");
    if (!path[0]) { ++fe->effects_failed; return; }
    snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temporary);
    FILE *file = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!file) {
        if (fd >= 0) { close(fd); unlink(temporary); }
        ++fe->effects_failed;
        return;
    }
    int ok = fputc('[', file) != EOF;
    int first = 1;
    for (int i = 0; i < count; ++i) {
        const char *label = tab == MAINUI_TAB_APPS ? fe->apps[i].label : fe->systems[i].label;
        int show = tab == MAINUI_TAB_APPS ? fe->apps[i].show : fe->systems[i].show;
        if (tab == MAINUI_TAB_APPS && fe->apps[i].stock != stock) continue;
        char escaped[128 * 6 + 3];
        if (mainui_json_escape(escaped, sizeof(escaped), label, strlen(label)) < 0) { ok = 0; break; }
        if (fprintf(file, "%s{\n\t\t\"label\":\t%s,\n\t\t\"show\":\t%d\n\t}", first ? "" : ", ", escaped, show != 0) < 0) ok = 0;
        first = 0;
    }
    if (fputc(']', file) == EOF || fflush(file) != 0) ok = 0;
    if (ok && fsync(fileno(file)) != 0) ok = 0;
    if (fclose(file) != 0) ok = 0;
    if (!ok || rename(temporary, path) != 0) {
        unlink(temporary);
        ++fe->effects_failed;
    }
}

static void save_show(struct mainui_frontend *fe, int tab) {
    if (tab == MAINUI_TAB_APPS) save_show_root(fe, tab, 1);
    save_show_root(fe, tab, 0);
}

// ---- Network adapters ----------------------------------------------------------------------

// Static (thread_wifi_monitor): every 3 s the connection is read (status, then signal_poll);
// with the scan on, scan follows, then scan_result 1.5 s later (rx_wifi_do_scan).
#define WIFI_RESCAN_MS 3000     // from scan_result to the next scan
#define STATUS_POLL_MS 3000     // the status bar's reading of the connection
#define WIFI_RESULT_MS 1500     // from wpa_cli scan to scan_result
#define BT_REQUEST_GAP_MS 500   // requests go one at a time, so the manager reads each alone
#define BT_INFO_MS 5000
#define BT_PENDING_LIMIT_MS 20000  // static: PairAction and RemoveAction wait 20 s for the answer
#define BT_ON_LIMIT_MS 20000
#define BT_OFF_LIMIT_MS 10000

static int capture(struct mainui_frontend *fe, const char *command, char *out, size_t size) {
    out[0] = 0;
    return fe->runner.capture ? fe->runner.capture(fe->runner.context, command, out, size) : -1;
}

static void note_command(struct mainui_frontend *fe, const char *command) {
    // The diagnostic includes the operation and network id, but neither credential-bearing
    // value. An SSID may equal or contain the Wi-Fi password, so redact it too.
    const char *set = strstr(command, " set_network ");
    if (set) {
        const char *network_id = set + sizeof(" set_network ") - 1;
        const char *field = strchr(network_id, ' ');
        if (field) {
            ++field;
            const char *hidden = !strncmp(field, "psk ", 4) ? "psk" :
                                 !strncmp(field, "ssid ", 5) ? "ssid" : NULL;
            if (hidden) {
                note(fe, "run %.*s%s (hidden)", (int)(field - command), command, hidden);
                return;
            }
        }
    }
    note(fe, "run %s", command);
}

static int run(struct mainui_frontend *fe, const char *command) {
    if (fe->dry || !fe->runner.run) return 0;
    note_command(fe, command);
    if (fe->runner.run(fe->runner.context, command) == 0) return 0;
    ++fe->effects_failed;
    return -1;
}

// A command whose status the official ignores, such as removing a marker that is usually
// absent: it is not counted as a failed effect.
static void run_unchecked(struct mainui_frontend *fe, const char *command) {
    if (fe->dry || !fe->runner.run) return;
    note_command(fe, command);
    (void)fe->runner.run(fe->runner.context, command);
}

static void *refresh_worker(void *context) {
    struct mainui_frontend *fe = context;
    int failed = 0;
    if (!fe->dry) {
        if (mainui_launch_other_platform_card(fe->fs.root)) {
            if (!fe->runner.run || fe->runner.run(fe->runner.context, "rm -rf /mnt/P2EXT/.dbs/*") != 0) ++failed;
        } else {
            for (int i = 0; i < fe->system_count; ++i)
                if (mainui_catalog_clear_caches(&fe->fs, fe->systems[i].rompath) < 0) ++failed;
            for (int i = 0; i < fe->best_count; ++i)
                if (mainui_catalog_clear_caches(&fe->fs, fe->bests[i].rompath) < 0) ++failed;
        }
        // removeAllDBFiles syncs first, then removeDBAndKeymap removes keymaps
        // and syncs again before unlinking the saved list positions.
        if (fe->runner.run && fe->runner.run(fe->runner.context, "sync") != 0) ++failed;
        if (mainui_catalog_clear_keymaps(&fe->fs) < 0) ++failed;
        if (fe->runner.run && fe->runner.run(fe->runner.context, "sync") != 0) ++failed;
        char path[PATH_BYTES];
        path_under(fe, path, MAINUI_POSITIONS_FILE);
        if (unlink(path) != 0 && errno != ENOENT) ++failed;
    }
    fe->refresh_failed = failed;
    __atomic_store_n(&fe->refresh_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int refresh_finish(struct mainui_frontend *fe, int closing) {
    if (!fe->refresh_pending) return 0;
    const struct mainui_window *top = mainui_model_top(&fe->model);
    if (!closing && (top->kind != MAINUI_WIN_PROGRESS || top->progress != MAINUI_PROGRESS_REFRESH ||
                     !__atomic_load_n(&fe->refresh_done, __ATOMIC_ACQUIRE))) return 0;
    if (fe->refresh_running) pthread_join(fe->refresh_thread, NULL);
    else if (closing && fe->refresh_pending == 1) refresh_worker(fe);
    fe->refresh_running = fe->refresh_pending = 0;
    fe->effects_failed += fe->refresh_failed;
    if (!fe->dry) {
        fe->positions.count = 0;
        fe->positions_changed = 0; // Do not recreate the positions just removed by refresh.
    }
    note(fe, "refresh complete failed=%d", fe->refresh_failed);
    if (!closing) {
        if (load_catalog(fe) < 0) ++fe->effects_failed;
        mainui_model_event(&fe->model, MAINUI_EVENT_PROGRESS_DONE);
    }
    return 1;
}

static int refresh_tick(struct mainui_frontend *fe) {
    if (fe->refresh_pending == 1) {
        fe->refresh_pending = 2;
        if (!fe->dry && pthread_create(&fe->refresh_thread, NULL, refresh_worker, fe) == 0)
            fe->refresh_running = 1;
        else refresh_worker(fe);
        // Keep the progress window for this UI iteration, including an instant worker.
        return 1;
    }
    return refresh_finish(fe, 0);
}

// Reads the scan results, the connection, and the saved networks (read-only wpa_cli queries).
static void wifi_read(struct mainui_frontend *fe) {
    static char text[16384], saved[8192];
    capture(fe, MAINUI_WPA_CLI " scan_result", text, sizeof(text));
    fe->wifi_count = mainui_wifi_parse_scan(text, fe->wifi, MAINUI_WIFI_MAX);
    capture(fe, MAINUI_WPA_CLI " status", text, sizeof(text));
    mainui_wifi_parse_status(text, &fe->wifi_status);
    int listed = capture(fe, MAINUI_WPA_CLI " list_network", saved, sizeof(saved)) == 0;
    mainui_wifi_mark(fe->wifi, fe->wifi_count, listed ? saved : NULL, &fe->wifi_status);
    note(fe, "wifi networks=%d state=%s", fe->wifi_count, fe->wifi_status.state);
}

// While the Wi-Fi screen is in front with Wi-Fi on: each opening shows Scanning until its first
// scan is read (r3: "Scanning" 2.4 s after A), then a scan every few seconds (static:
// rx_wifi_do_scan runs scan, then scan_result).
// open: the Wi-Fi screen is on the window stack; shown: nothing but a popup, dialog, or notice
// covers it (a keyboard or progress window over it keeps the list as it is).
static int wifi_tick(struct mainui_frontend *fe, uint64_t now, int open, int shown) {
    if (!open || !fe->settings.values[MAINUI_SET_WIFI] || !fe->runner.capture) {
        fe->wifi_shown = fe->wifi_scanning = fe->wifi_first = 0;
        return 0;
    }
    if (!shown) return 0;
    if (!fe->wifi_shown) {
        fe->wifi_shown = 1;
        fe->wifi_first = 1;
        fe->wifi_count = 0;
        fe->wifi_scanning = 0;
        fe->wifi_next_ms = now;
    }
    if (now < fe->wifi_next_ms) return 0;
    if (fe->wifi_scanning) {
        fe->wifi_scanning = 0;
        fe->wifi_first = 0;
        fe->wifi_next_ms = now + WIFI_RESCAN_MS;
        wifi_read(fe);
        return 1;
    }
    // A scan is an action: a dry run only reads what wpa_supplicant has.
    char out[256];
    if (!fe->dry) capture(fe, MAINUI_WPA_CLI " scan", out, sizeof(out));
    fe->wifi_scanning = 1;
    fe->wifi_next_ms = now + WIFI_RESULT_MS;
    return fe->wifi_first;
}

// The status bar shows the connection's signal on every screen (c1), so the connection and its
// signal are read every few seconds (read-only queries; static: MainUI reads status and
// signal_poll into /tmp/wpa_status.txt). With the hotspot on, the status bar shows its icon
// instead.
static int status_tick(struct mainui_frontend *fe, uint64_t now) {
    static char text[4096];
    if (!fe->runner.capture || now < fe->status_next_ms) return 0;
    fe->status_next_ms = now + STATUS_POLL_MS;
    int connected = strcmp(fe->wifi_status.state, "COMPLETED") == 0, level = mainui_wifi_bar_level(fe->wifi_rssi);
    // A readable /dev/dsp1 adds the headphones beside the speaker (static: draw_nav_bar).
    char path[PATH_BYTES];
    int dsp1 = fe->dsp1;
    path_under(fe, path, "/dev/dsp1");
    fe->dsp1 = access(path, R_OK) == 0;
    if (!fe->settings.values[MAINUI_SET_WIFI] || fe->settings.values[MAINUI_SET_HOSTAP]) {
        fe->wifi_status.state[0] = 0;
        fe->wifi_rssi = MAINUI_RSSI_NONE;
    } else {
        capture(fe, MAINUI_WPA_CLI " status", text, sizeof(text));
        mainui_wifi_parse_status(text, &fe->wifi_status);
        fe->wifi_rssi = MAINUI_RSSI_NONE;
        if (strcmp(fe->wifi_status.state, "COMPLETED") == 0 && capture(fe, MAINUI_WPA_CLI " signal_poll", text, sizeof(text)) == 0)
            fe->wifi_rssi = mainui_wifi_parse_signal(text);
    }
    return connected != (strcmp(fe->wifi_status.state, "COMPLETED") == 0) || level != mainui_wifi_bar_level(fe->wifi_rssi) ||
           dsp1 != fe->dsp1;
}

// Wi-Fi work under its progress window (static: WifiItemAction and the actions of the saved
// network popup, each on a thread behind a ProgressDialog). The first step runs at the next tick,
// after the window is drawn. A connect names the network (and a new one gets its id from
// add_network), selects and enables it, and saves; a second later it enables every saved network
// again and saves, then reads the state once a second. The connection leaves 2 s for DHCP before
// the window closes; no connection shows the failure notice and clears the ticks (event 42).
// Connect and Forget from the popup only select or remove the network and save, with no message.
// BrickOS change (stock defect): the stock reads the state 11 times; after a failed attempt
// wpa_supplicant waits before it tries again, so a right password connected after that and was
// reported as failed (w6). BrickOS reads it up to 30 times, and stops at the connection.
#define WIFI_STEP_MS 1000
#define WIFI_POLLS 30
#define WIFI_DHCP_MS 2000

static void wifi_start(struct mainui_frontend *fe, const struct mainui_effect *e) {
    if (e->index < 0 || e->index >= fe->wifi_count) {
        ++fe->effects_failed;
        mainui_model_event(&fe->model, MAINUI_EVENT_PROGRESS_DONE);
        return;
    }
    const struct mainui_wifi_network *w = &fe->wifi[e->index];
    note(fe, "wifi kind=%d network=%d saved=%d password=%d", (int)e->kind, e->index, w->saved_id, e->text[0] != 0);
    if (fe->dry || !fe->runner.run) {
        mainui_model_event(&fe->model, MAINUI_EVENT_PROGRESS_DONE);
        return;
    }
    fe->wifi_job = (int)e->kind;
    fe->wifi_step = 1;
    fe->wifi_step_ms = 0;
    fe->wifi_id = w->saved_id;
    fe->wifi_open = e->kind == MAINUI_EFFECT_WIFI_CONNECT && e->value == 1;
    fe->wifi_current = w->connected;
    fe->wifi_polls = 0;
    snprintf(fe->wifi_ssid, sizeof(fe->wifi_ssid), "%s", w->ssid);
    snprintf(fe->wifi_password, sizeof(fe->wifi_password), "%s", e->text);
}

static void wifi_done(struct mainui_frontend *fe, int failed) {
    note(fe, "wifi kind=%d done failed=%d polls=%d", fe->wifi_job, failed, fe->wifi_polls);
    if (failed)
        for (int i = 0; i < fe->wifi_count; ++i) fe->wifi[i].connected = 0;
    fe->wifi_step = 0;
    fe->wifi_password[0] = 0;
    fe->wifi_next_ms = 0;  // the list is read again when the screen is back
    mainui_model_event(&fe->model, MAINUI_EVENT_PROGRESS_DONE);
    if (failed) mainui_model_notice(&fe->model, MAINUI_NOTICE_WIFI_FAILED);
}

// Runs a wpa_cli setting that answers OK or FAIL; wpa_cli exits with 0 either way (w6), so the
// answer is read. Returns -1 unless it says OK.
static int run_setting(struct mainui_frontend *fe, const char *command) {
    char out[256];
    note_command(fe, command);
    if (capture(fe, command, out, sizeof(out)) < 0 || strncmp(out, "OK", 2) != 0) {
        note(fe, "wifi setting refused");
        ++fe->effects_failed;
        return -1;
    }
    return 0;
}

// Runs the lines in order; returns -1 at the first one that fails.
static int wifi_run_all(struct mainui_frontend *fe, char lines[][512], int n) {
    if (n < 0) { ++fe->effects_failed; return -1; }
    for (int i = 0; i < n; ++i)
        if (run(fe, lines[i]) < 0) return -1;
    return 0;
}

static int wifi_job_tick(struct mainui_frontend *fe, uint64_t now) {
    char lines[6][512], out[4096];
    if (!fe->wifi_step || now < fe->wifi_step_ms) return 0;
    if (fe->wifi_job != MAINUI_EFFECT_WIFI_CONNECT) {
        int n = fe->wifi_job == MAINUI_EFFECT_WIFI_FORGET ? mainui_wifi_forget_commands(fe->wifi_id, lines, 6)
                                                          : mainui_wifi_reconnect_commands(fe->wifi_id, lines, 6);
        wifi_run_all(fe, lines, n);
        sync();
        wifi_done(fe, 0);
        return 1;
    }
    switch (fe->wifi_step) {
    case 1:
        if (fe->wifi_id < 0) {
            // add_network prints the new network's id.
            capture(fe, MAINUI_WPA_CLI " add_network", out, sizeof(out));
            char *last = out, *line = strtok(out, "\r\n");
            for (; line; line = strtok(NULL, "\r\n")) last = line;
            if (last[0] >= '0' && last[0] <= '9') fe->wifi_id = atoi(last);
        }
        if (fe->wifi_id < 0) { ++fe->effects_failed; wifi_done(fe, 1); return 1; }
        int count = mainui_wifi_connect_commands(fe->wifi_ssid, fe->wifi_open ? NULL : fe->wifi_password, fe->wifi_id, lines, 6);
        // BrickOS changes (stock defects): a name or password that wpa_cli refuses (a password
        // under 8 characters) fails at once instead of going on with the old settings; and for
        // the network in use, whose association wpa_supplicant keeps (w5: a wrong password reported
        // success and was only saved), the link is dropped before select_network, which connects
        // again with the new password, so the wait below checks it.
        int failed = count < 2 || run_setting(fe, lines[0]) < 0 || run_setting(fe, lines[1]) < 0;
        if (!failed && fe->wifi_current) run(fe, MAINUI_WPA_CLI " disconnect");
        if (!failed) wifi_run_all(fe, lines + 2, count - 2);
        sync();
        if (failed) { wifi_done(fe, 1); return 1; }
        fe->wifi_step = 2;
        fe->wifi_step_ms = now + WIFI_STEP_MS;
        return 0;
    case 2: {
        // Every saved network is enabled again (static: enable_network for each saved entry).
        static char saved[8192];
        char line[128];
        if (capture(fe, MAINUI_WPA_CLI " list_network", saved, sizeof(saved)) == 0) {
            for (const char *p = saved; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
                if (*p >= '0' && *p <= '9') {
                    snprintf(line, sizeof(line), MAINUI_WPA_CLI " enable_network %d", atoi(p));
                    run(fe, line);
                }
        }
        run(fe, MAINUI_WPA_CLI " save_config");
        fe->wifi_step = 3;
        fe->wifi_step_ms = now + WIFI_STEP_MS;
        return 0;
    }
    case 3: {
        struct mainui_wifi_status status;
        capture(fe, MAINUI_WPA_CLI " status", out, sizeof(out));
        mainui_wifi_parse_status(out, &status);
        ++fe->wifi_polls;
        if (strcmp(status.state, "COMPLETED") == 0 && strcmp(status.ssid, fe->wifi_ssid) == 0) {
            fe->wifi_step = 4;
            fe->wifi_step_ms = now + WIFI_DHCP_MS;
        } else if (fe->wifi_polls >= WIFI_POLLS) {
            wifi_done(fe, 1);
            return 1;
        } else {
            fe->wifi_step_ms = now + WIFI_STEP_MS;
        }
        return 0;
    }
    default:
        wifi_done(fe, 0);
        return 1;
    }
}

// BrickOS change (the user's decision): the adapter is not pairable on this firmware, so a pairing
// keeps no link key and every connect has to pair again, which earbuds refuse outside pairing
// mode (a5: Pairable: no, and no LinkKey in /etc/lib/bluetooth, where bluetoothd stores its
// bonds). The adapter is pairable only while a connect runs, so the pairing it makes is kept.
// Saved devices then reconnect by themselves (a8: the earbuds, out of the case, connected to the
// official MainUI with an incoming AVDTP connection), so BrickOS sends no automatic connects:
// one to an absent device holds the manager about 2.5 min (a6, a7).
static void bt_pairable(struct mainui_frontend *fe, int on) {
    if (fe->bt_pairable == on) return;
    fe->bt_pairable = on;
    run(fe, on ? "bluetoothctl pairable on" : "bluetoothctl pairable off");
}

static int bt_is_queued(const struct mainui_frontend *fe, const char *request) {
    for (int i = 0; i < fe->bt_queued; ++i)
        if (strcmp(fe->bt_queue[i], request) == 0) return 1;
    return 0;
}

static void bt_queue(struct mainui_frontend *fe, const char *request) {
    if (fe->dry || fe->bt_queued == MAINUI_BT_QUEUE) return;
    snprintf(fe->bt_queue[fe->bt_queued++], sizeof(fe->bt_queue[0]), "%s", request);
}

// Writes one request to the manager's FIFO with its terminating NUL, as MainUI does: it opens the
// FIFO read-write once, so opening never waits and the manager never sees the writer go
// (static: main opens it with O_RDWR; each write is strlen + 1 bytes).
static int bt_request(struct mainui_frontend *fe, const char *request) {
    char path[PATH_BYTES];
    if (fe->dry) return 0;
    if (fe->bt_request_fd < 0) {
        path_under(fe, path, "/tmp/bluetooth_request");
        fe->bt_request_fd = open(path, O_RDWR | O_NONBLOCK);
        if (fe->bt_request_fd < 0) { note(fe, "bluetooth request %s failed errno=%d", request, errno); return -1; }
    }
    size_t length = strlen(request) + 1;
    void (*previous)(int) = signal(SIGPIPE, SIG_IGN);
    ssize_t n = write(fe->bt_request_fd, request, length);
    signal(SIGPIPE, previous);
    note(fe, "bluetooth request %s", request);
    return n == (ssize_t)length ? 0 : -1;
}

static int find_device(const struct mainui_bt_device *devices, int count, const char *addr) {
    for (int i = 0; i < count; ++i)
        if (strcmp(devices[i].addr, addr) == 0) return i;
    return -1;
}

const struct mainui_bt_device *mainui_frontend_audio_device(const struct mainui_frontend *fe) {
    if (!fe->settings.values[MAINUI_SET_BLUETOOTH]) return NULL;
    for (int i = 0; i < fe->paired_count; ++i)
        if (fe->paired[i].connected && strstr(fe->paired[i].icon, "audio")) return &fe->paired[i];
    return NULL;
}

// keymon, hardwareservice, and musicserver send sound and volume to the Bluetooth device named in
// /tmp/btdev_addr (the address alone). MainUI writes the connected audio device there when the
// file is missing (static: getSpecifcBTDev("audio"), which the status bar calls on every frame),
// and removes it when an answer reports a device disconnected and when Bluetooth goes off (c4:
// written while the headphones were connected). Open: bt_thread removes it for any disconnected
// device; here only for an audio one, which ends in the same state without the flapping.
#define BTDEV_ADDR "/tmp/btdev_addr"

static void bt_audio_sync(struct mainui_frontend *fe) {
    char path[PATH_BYTES];
    const struct mainui_bt_device *audio = mainui_frontend_audio_device(fe);
    if (fe->dry || !audio) return;
    path_under(fe, path, BTDEV_ADDR);
    if (access(path, F_OK) == 0) return;
    FILE *f = fopen(path, "w+");
    if (!f) return;
    fputs(audio->addr, f);
    fclose(f);
    note(fe, "bluetooth audio device written");
}

static void bt_audio_forget(struct mainui_frontend *fe) {
    char path[PATH_BYTES];
    if (fe->dry) return;
    path_under(fe, path, BTDEV_ADDR);
    if (unlink(path) == 0) note(fe, "bluetooth audio device removed");
}

// One answer from the manager (static: its reportDeviceList, reportDeviceStatus, and
// reportRemoveStatus; MainUI's bt_thread reads func scan, check, info, connect, ping, remove,
// and getdevs). A device list is My Devices, and each of its devices is asked about with info,
// as bt_thread does; an info answer updates the device; a scan answer adds to Other Devices a
// device that is not paired; a connect or remove answer asks for the list again.
static int bt_answer(struct mainui_frontend *fe, const char *text) {
    struct mainui_bt_answer answer;
    static struct mainui_bt_device devices[MAINUI_BT_MAX];
    char request[64];
    note(fe, "bluetooth answer %s", text);
    int n = mainui_bt_parse(text, &answer, devices, MAINUI_BT_MAX);
    if (n < 0) return 0;
    // An empty list comes without the devices member (a3: {"func":"getdevs"}).
    if (strcmp(answer.func, "getdevs") == 0 || strstr(text, "\"devices\"")) {
        memcpy(fe->paired, devices, (size_t)n * sizeof(devices[0]));
        fe->paired_count = n;
        fe->bt_devices_known = 1;
        for (int i = 0; i < n; ++i) {
            snprintf(request, sizeof(request), "info %.17s", devices[i].addr);
            bt_queue(fe, request);
        }
        // A device that is paired now leaves Other Devices.
        for (int i = fe->found_count - 1; i >= 0; --i)
            if (find_device(fe->paired, fe->paired_count, fe->found[i].addr) >= 0) {
                memmove(&fe->found[i], &fe->found[i + 1], (size_t)(fe->found_count - i - 1) * sizeof(fe->found[0]));
                --fe->found_count;
            }
        return 1;
    }
    if (strcmp(answer.func, "connect") == 0 || strcmp(answer.func, "remove") == 0) {
        // A removed audio device no longer takes the sound (static: bt_thread clears the
        // device's connection and removes /tmp/btdev_addr).
        int at = n > 0 ? find_device(fe->paired, fe->paired_count, devices[0].addr) : -1;
        if (strcmp(answer.func, "remove") == 0 && at >= 0) {
            fe->paired[at].connected = 0;
            if (strstr(fe->paired[at].icon, "audio")) bt_audio_forget(fe);
        }
        if (strcmp(fe->bt_pending, answer.func) == 0) fe->bt_pending[0] = 0;
        if (strcmp(answer.func, "connect") == 0) bt_pairable(fe, 0);
        // A removal that succeeded says so for about 5 s (d1: "result":1, then Device Removed). A
        // connect shows nothing when it works (the user: no dialog, it connected at once). The
        // Bluetooth screen's event handler shows Connection Failed in the same NotifyWindow
        // (static). Open: which answer makes it show, and what a failed removal shows.
        if (strcmp(answer.func, "remove") == 0 && strcmp(answer.result, "1") == 0) {
            mainui_model_notice(&fe->model, MAINUI_NOTICE_DEVICE_REMOVED);
            fe->notice_ms = 0;
        } else if (strcmp(answer.func, "connect") == 0 && !(n > 0 && devices[0].connected)) {
            mainui_model_notice(&fe->model, MAINUI_NOTICE_CONNECTION_FAILED);
            fe->notice_ms = 0;
        }
        bt_queue(fe, "getdevs");
        return 1;
    }
    if (n == 0) return 0;
    if (strcmp(answer.func, "info") == 0) {
        int at = find_device(fe->paired, fe->paired_count, devices[0].addr);
        if (at < 0) return 0;
        struct mainui_bt_device *d = &fe->paired[at];
        int changed = d->connected != devices[0].connected;
        d->connected = devices[0].connected;
        if (devices[0].name[0]) snprintf(d->name, sizeof(d->name), "%s", devices[0].name);
        if (devices[0].icon[0]) snprintf(d->icon, sizeof(d->icon), "%s", devices[0].icon);
        if (!d->connected && strstr(d->icon, "audio")) bt_audio_forget(fe);
        return changed;
    }
    if (strcmp(answer.func, "scan") == 0) {
        if (find_device(fe->paired, fe->paired_count, devices[0].addr) >= 0) return 0;
        int at = find_device(fe->found, fe->found_count, devices[0].addr);
        if (at < 0 && fe->found_count == MAINUI_BT_MAX) return 0;
        if (at < 0) at = fe->found_count++;
        fe->found[at] = devices[0];
        return 1;
    }
    return 0;
}

static int bt_read(struct mainui_frontend *fe) {
    int changed = 0;
    size_t start, end;
    for (;;) {
        // An answer longer than the buffer is dropped.
        if (fe->bt_length == sizeof(fe->bt_data) - 1) fe->bt_length = 0;
        ssize_t n = read(fe->bt_fd, fe->bt_data + fe->bt_length, sizeof(fe->bt_data) - 1 - fe->bt_length);
        if (n <= 0) break;
        note(fe, "bluetooth read bytes=%d", (int)n);
        fe->bt_length += (size_t)n;
    }
    while (mainui_bt_next_object(fe->bt_data, fe->bt_length, &start, &end)) {
        char kept = fe->bt_data[end];
        fe->bt_data[end] = 0;
        changed |= bt_answer(fe, fe->bt_data + start);
        fe->bt_data[end] = kept;
        memmove(fe->bt_data, fe->bt_data + end, fe->bt_length - end);
        fe->bt_length -= end;
    }
    return changed;
}

static int file_exists(const struct mainui_frontend *fe, const char *device_path) {
    char path[PATH_BYTES];
    path_under(fe, path, device_path);
    return access(path, F_OK) == 0;
}

static void *power_worker(void *context) {
    struct mainui_frontend *fe = context;
    struct mainui_effect effect = { .kind = fe->power_reboot ? MAINUI_EFFECT_REBOOT : MAINUI_EFFECT_POWER_OFF };
    fe->power_failed = !fe->dry && fe->power_runner.run &&
        !(fe->power_source && file_exists(fe, "/tmp/poweroff_flag")) &&
        mainui_actions_apply(&fe->power_runner, &effect) < 0;
    __atomic_store_n(&fe->power_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void power_close(struct mainui_frontend *fe) {
    if (!fe->power_pending) return;
    if (fe->power_running) pthread_join(fe->power_thread, NULL);
    else if (fe->power_pending == 1) power_worker(fe);
    fe->effects_failed += fe->power_failed;
    note(fe, "power off close source=%d failed=%d", fe->power_source, fe->power_failed);
    fe->power_pending = fe->power_running = 0;
    // A bounded diagnostic shutdown cancels the UI wait, not the confirmed command.
}

static int power_request(struct mainui_frontend *fe) {
    if (!fe->power_pending && !fe->exit_requested && file_exists(fe, "/tmp/poweroff_flag")) {
        if (mainui_model_event(&fe->model, MAINUI_EVENT_POWER_REQUEST)) {
            carry_out_all(fe);
            return 1;
        }
    }
    return 0;
}

static int power_tick(struct mainui_frontend *fe, uint64_t now_ms) {
    if (power_request(fe)) return 1; // Keep the new progress for this UI iteration.
    if (fe->power_pending == 1) {
        fe->power_pending = 2;
        if (fe->dry || !fe->power_runner.run) power_worker(fe);
        else if (pthread_create(&fe->power_thread, NULL, power_worker, fe) == 0) fe->power_running = 1;
        else {
            // Stock's failed SDL_CreateThread runs no action; quit has no thread to join.
            fe->power_failed = 1;
            fe->power_wait_ms = 0;
            note(fe, "power off worker unavailable source=%d", fe->power_source);
            __atomic_store_n(&fe->power_done, 1, __ATOMIC_RELEASE);
        }
        return 1;
    }
    if (fe->power_pending == 2 && __atomic_load_n(&fe->power_done, __ATOMIC_ACQUIRE)) {
        if (fe->power_running) pthread_join(fe->power_thread, NULL);
        fe->power_running = 0;
        fe->power_pending = 3;
        fe->power_wait_since = now_ms;
        note(fe, "power off command complete source=%d failed=%d", fe->power_source, fe->power_failed);
        fe->effects_failed += fe->power_failed;
        fe->power_failed = 0;
    }
    if (fe->power_pending == 3 && now_ms >= fe->power_wait_since &&
        now_ms - fe->power_wait_since >= (uint64_t)fe->power_wait_ms) {
        const struct mainui_window *top = mainui_model_top(&fe->model);
        if (top->kind != MAINUI_WIN_PROGRESS ||
            (top->progress != MAINUI_PROGRESS_POWER_OFF && top->progress != MAINUI_PROGRESS_REBOOT)) return 0;
        note(fe, "power off wait complete source=%d", fe->power_source);
        fe->power_pending = 0;
        mainui_model_event(&fe->model, MAINUI_EVENT_PROGRESS_DONE);
        return 1;
    }
    return 0;
}

// Bluetooth: the manager opens its result FIFO for writing and only then reports ready, so the
// frontend holds the reading end while Bluetooth is on (static: trimui_btmanager's "before open
// fifo2", "opened fifo", /tmp/bluetooth_ready). Once ready it asks for the paired devices, and
// scans while the Bluetooth screen is in front (static: BTMenu sends scan on, and scan off when
// it closes).
static int bt_tick(struct mainui_frontend *fe, uint64_t now, int shown, const struct mainui_window *top) {
    int changed = 0, on = fe->settings.values[MAINUI_SET_BLUETOOTH];
    int waiting = top->kind == MAINUI_WIN_PROGRESS &&
                  (top->progress == MAINUI_PROGRESS_BLUETOOTH_ON || top->progress == MAINUI_PROGRESS_BLUETOOTH_OFF);
    int ready = file_exists(fe, "/tmp/bluetooth_ready");
    if (!fe->dry && on && fe->bt_fd < 0) {
        // Read-write, as bt_thread opens it: reads never see an end of file.
        char path[PATH_BYTES];
        path_under(fe, path, "/tmp/bluetooth_result");
        fe->bt_fd = open(path, O_RDWR | O_NONBLOCK);
        if (fe->bt_fd >= 0) note(fe, "bluetooth result open");
    }
    if (fe->bt_fd >= 0) changed |= bt_read(fe);
    if (waiting) {
        if (!fe->progress_ms) fe->progress_ms = now ? now : 1;
        int turning_on = top->progress == MAINUI_PROGRESS_BLUETOOTH_ON;
        uint64_t limit = fe->dry ? 1000 : turning_on ? BT_ON_LIMIT_MS : BT_OFF_LIMIT_MS;
        // Open: whether hardwareservice removes the ready file when Bluetooth goes off.
        int done = !fe->dry && (turning_on ? ready : !ready);
        if (done || now - fe->progress_ms >= limit) {
            note(fe, "bluetooth %s ready=%d ms=%llu", turning_on ? "on" : "off", ready, (unsigned long long)(now - fe->progress_ms));
            fe->progress_ms = 0;
            mainui_model_event(&fe->model, MAINUI_EVENT_PROGRESS_DONE);
            changed = 1;
        }
        return changed;
    }
    if (!on) {
        if (fe->bt_fd >= 0) { close(fe->bt_fd); fe->bt_fd = -1; }
        changed = fe->paired_count || fe->found_count;
        fe->paired_count = fe->found_count = fe->bt_listed = fe->bt_scanning = fe->bt_shown = fe->bt_queued = 0;
        fe->bt_length = 0;
        fe->bt_pending[0] = 0;
        bt_pairable(fe, 0);
        return changed;
    }
    if (ready && !fe->bt_listed) { bt_queue(fe, "getdevs"); fe->bt_listed = 1; fe->bt_info_ms = now; }
    // Leaving the screen sends scan off first.
    if (!shown && fe->bt_shown) {
        bt_queue(fe, "scan off");
        fe->bt_shown = fe->bt_scanning = 0;
        fe->bt_info_ms = 0;
    }
    // MainUI asks each saved device's state every 5 s, except while its Bluetooth screen is open
    // (c7: info every 5 s, none between scan on and scan off, and one right after scan off).
    if (ready && fe->bt_listed && !shown && now - fe->bt_info_ms >= BT_INFO_MS &&
        fe->bt_queued + fe->paired_count <= MAINUI_BT_QUEUE) {
        char request[64];
        // A round still waiting behind a connect is not queued twice (a5: six info requests went
        // out at once after a connect timed out).
        for (int i = 0; i < fe->paired_count; ++i) {
            snprintf(request, sizeof(request), "info %.17s", fe->paired[i].addr);
            if (!bt_is_queued(fe, request)) bt_queue(fe, request);
        }
        fe->bt_info_ms = now;
    }
    bt_audio_sync(fe);
    // Opening the screen sends power on, then scan on (c7). The manager runs one request at a
    // time and ends a scan when another arrives (c5), so the scan is asked again once the queue
    // is empty (static: BTMenu sends scan on after its events).
    if (ready && shown && !fe->bt_shown) { bt_queue(fe, "power on"); fe->bt_shown = 1; }
    // A connect or remove runs until its answer, and another request would end it in the manager
    // (p1: a scan on sent 0.5 s after connect aborted the pairing), so nothing else goes out while
    // one is pending. With no answer after 20 s, the manager is told to stop, with no message
    // (static: "connect bluetooth device timeout", then the request stop); a connect answer with
    // connected 0 that follows shows Connection Failed.
    if (fe->bt_pending[0] && now - fe->bt_pending_ms >= BT_PENDING_LIMIT_MS) {
        note(fe, "bluetooth %s timed out", fe->bt_pending);
        bt_request(fe, "stop");
        fe->bt_sent_ms = now;
        if (strcmp(fe->bt_pending, "connect") == 0) bt_pairable(fe, 0);
        fe->bt_pending[0] = 0;
    }
    if (fe->bt_pending[0]) return changed;
    if (ready && shown && !fe->bt_scanning && !fe->bt_queued) { bt_queue(fe, "scan on"); fe->bt_scanning = 1; }
    if (fe->bt_queued && now - fe->bt_sent_ms >= BT_REQUEST_GAP_MS) {
        if (strncmp(fe->bt_queue[0], "scan", 4) != 0) fe->bt_scanning = 0;
        if (strncmp(fe->bt_queue[0], "connect ", 8) == 0 || strncmp(fe->bt_queue[0], "remove ", 7) == 0) {
            snprintf(fe->bt_pending, sizeof(fe->bt_pending), "%.*s", (int)strcspn(fe->bt_queue[0], " "), fe->bt_queue[0]);
            fe->bt_pending_ms = now;
            if (strcmp(fe->bt_pending, "connect") == 0) bt_pairable(fe, 1);
        }
        bt_request(fe, fe->bt_queue[0]);
        memmove(fe->bt_queue[0], fe->bt_queue[1], (size_t)(--fe->bt_queued) * sizeof(fe->bt_queue[0]));
        fe->bt_sent_ms = now;
    }
    return changed;
}

// Unpairs a saved device (static: the manager's remove, which runs bluetoothctl remove; its
// answer brings the list again, and "result":1 shows Device Removed).
static void bt_remove(struct mainui_frontend *fe, int index) {
    char request[64];
    if (index < 0 || index >= fe->paired_count) { ++fe->effects_failed; return; }
    note(fe, "bluetooth remove device=%d", index);
    snprintf(request, sizeof(request), "remove %.17s", fe->paired[index].addr);
    bt_queue(fe, request);
}

static void bt_select(struct mainui_frontend *fe, enum mainui_list list, int index) {
    const struct mainui_bt_device *device = list == MAINUI_LIST_PAIRED ? (index < fe->paired_count ? &fe->paired[index] : NULL)
                                                                       : (index < fe->found_count ? &fe->found[index] : NULL);
    char request[64];
    if (index < 0 || !device) { ++fe->effects_failed; return; }
    // The official connects at once (the user: no question). Open: what A does on a connected
    // device.
    if (device->connected) return;
    snprintf(request, sizeof(request), "connect %.17s", device->addr);
    if (!bt_is_queued(fe, request)) bt_queue(fe, request);
}

// The clock reads the zone again: glibc reloads the zone file only when TZ changes, so TZ
// alternates between two names of the same link (/etc/localtime points to /tmp/localtime).
static void reload_zone(struct mainui_frontend *fe) {
    fe->tz_flip ^= 1;
    setenv("TZ", fe->tz_flip ? ":/tmp/localtime" : ":/etc/localtime", 1);
    tzset();
}

static void select_zone(struct mainui_frontend *fe, int level, int index) {
    char command[512], zone[256];
    struct mainui_zone_folder *f = zone_folder(fe, level);
    if (!f || index < 0 || index >= f->count) { ++fe->effects_failed; return; }
    snprintf(zone, sizeof(zone), "%s%s%s", f->path, f->path[0] ? "/" : "", f->zones[index].name);
    if (f->zones[index].folder) {
        // The model opened the folder's list one level down.
        if (level + 1 >= MAINUI_ZONE_LEVELS || strlen(zone) >= sizeof(fe->zones[0].path)) return;
        drop_zones(fe, level + 1);
        snprintf(fe->zones[level + 1].path, sizeof(fe->zones[0].path), "%s", zone);
        return;
    }
    note(fe, "timezone %s", zone);
    if (mainui_settings_set_timezone(&fe->settings, zone) < 0 || mainui_zone_command(zone, command, sizeof(command)) < 0) {
        ++fe->effects_failed;
        return;
    }
    if (run(fe, command) == 0 && !fe->dry && fe->runner.run) reload_zone(fe);
}

static void files_back(struct mainui_frontend *fe, int level) {
    struct mainui_window *w = &fe->model.stack[fe->model.depth - 1];
    if (w->kind != MAINUI_WIN_FILES || w->system != level || level <= 0 || level != fe->file_depth - 1) return;
    --fe->file_depth;
    --w->system;
    w->focus = w->first = 0;
}

static void select_file(struct mainui_frontend *fe, int level, int index) {
    struct mainui_window *w = &fe->model.stack[fe->model.depth - 1];
    const struct mainui_file_folder *folder = mainui_frontend_file_folder(fe, level);
    if (w->kind != MAINUI_WIN_FILES || w->system != level || level != fe->file_depth - 1 ||
        !folder || index < 0 || index >= folder->count) { ++fe->effects_failed; return; }
    char path[1024], full[PATH_BYTES];
    struct mainui_file row = folder->files[index];
    size_t length = strlen(folder->path);
    if (snprintf(path, sizeof(path), "%s%s%s", folder->path,
                 length && folder->path[length - 1] == '/' ? "" : "/", row.name) >= (int)sizeof(path)) {
        ++fe->effects_failed; return;
    }
    if (row.folder) {
        size_t n = strlen(path);
        if ((n >= 2 && !strcmp(path + n - 2, "..")) || (n >= 3 && !strcmp(path + n - 3, "../"))) {
            files_back(fe, level);
            return;
        }
        if (!file_path_inside(fe, path)) { ++fe->effects_failed; return; }
        int cached = file_cache(fe, path, 1);
        if (cached < 0 || reserve_files(fe, fe->file_depth + 1) < 0) { ++fe->effects_failed; return; }
        fe->file_history[fe->file_depth++] = cached;
        ++w->system;
        w->focus = w->first = 0;
        return;
    }
    struct stat st;
    path_under(fe, full, path);
    if (!file_path_inside(fe, path) || !full[0] || stat(full, &st) < 0 || !S_ISREG(st.st_mode)) {
        ++fe->effects_failed; return;
    }
    const char *zone = strstr(path, "/usr/share/zoneinfo/");
    if (zone) {
        char command[512];
        zone += strlen("/usr/share/zoneinfo/");
        if (mainui_zone_command(zone, command, sizeof(command)) < 0 || mainui_settings_set_timezone(&fe->settings, zone) < 0) {
            ++fe->effects_failed; return;
        }
        if (run(fe, command) == 0 && !fe->dry && fe->runner.run) reload_zone(fe);
        if (!fe->dry) {
            path_under(fe, full, "/mnt/UDISK/system.json");
            if (mainui_settings_save(&fe->settings, full) < 0) ++fe->effects_failed;
        }
        --fe->model.depth; // FileAction returns the close-window event 62.
        return;
    }
    // FileAction checks the trimui_ prefix before either ROM launch branch.
    if (!strncasecmp(row.name, "trimui_", 7) && strrchr(row.name, '.')) {
        struct statvfs sd;
        path_under(fe, full, "/mnt/SDCARD");
        uint64_t total = 0, available = 0;
        if (full[0] && statvfs(full, &sd) == 0) {
            total = (uint64_t)sd.f_bsize * sd.f_blocks;
            available = (uint64_t)sd.f_bsize * sd.f_bfree;
        }
        if (mainui_frontend_offer_upgrade(fe, path, total, available) < 0) ++fe->effects_failed;
        return;
    }
    int type = mainui_catalog_file_type(row.name);
    int system = type < 0 ? mainui_catalog_file_system(fe->systems, fe->system_count, row.name) : -1;
    if (type < 0 && system < 0) return; // COPYING/LICENSE rows have no default action.
    struct mainui_list_entry entry = {.valid = 1, .type = type < 0 ? MAINUI_ENTRY_GAME : type};
    copy_value(entry.label, sizeof(entry.label), row.name);
    copy_value(entry.rompath, sizeof(entry.rompath), path);
    entry.rompath_length = strlen(path);
    char basename[256];
    copy_value(basename, sizeof(basename), row.name);
    char *dot = strrchr(basename, '.');
    if (dot) *dot = 0;
    if (system >= 0) {
        const struct mainui_system *s = &fe->systems[system];
        copy_value(entry.launch, sizeof(entry.launch), s->launch);
        copy_value(entry.sublabel, sizeof(entry.sublabel), s->label);
        // FileAction retains the slash in its filename substring. These bytes
        // also enter stock history; do not normalize its duplicate separators.
        snprintf(entry.imgpath, sizeof(entry.imgpath), "%s//%s.png", s->imgpath, basename);
    } else if (type >= 5 && type <= 15) {
        static const char *const directories[] = {"ARCADE", "NEOGEO", "FC", "SFC", "GB", "MD", "GBA", "MS", "PCE", "PCE", ""};
        if (directories[type - 5][0])
            snprintf(entry.imgpath, sizeof(entry.imgpath), "/mnt/SDCARD/Imgs/%s///%s.png", directories[type - 5], basename);
    }
    launch_entry(fe, &entry, NULL, NULL);
}

// The files the stock OSD and the music server read for their skins and hints (static: main,
// ThemeMenu::select, and OSDShortcutHandler::changeValue; s1 left "../res/" in the OSD's file,
// and the music server removes its own after reading it). Written as the official does: the
// theme files only when their folders exist, the OSD folder made for the hotkey name.
static void write_text(struct mainui_frontend *fe, const char *device_path, const char *text) {
    char path[PATH_BYTES];
    path_under(fe, path, device_path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

// ---- The idle music picture for the official OSD (docs/mainui.md) ----------------------------

#define MUSIC_PICTURES "/tmp/brickos/music"
#define MUSIC_STOCK_PICTURE "/usr/trimui/osd/widgets/app_music/music_default.png"
#define BRICKOS_OSDD_START "/mnt/SDCARD/System/starts/brickos-osdd.sh"

// One picture to write: the stock one, recoloured for a look, into that look's folder.
struct mainui_music_job {
    char id[32], source[PATH_BYTES], directory[PATH_BYTES], destination[PATH_BYTES + 32];
    unsigned char seed[3];
    double level;
    int (*write)(const char *source, const char *destination, const unsigned char seed[3], double level);
    int result;
};

// A look's folder name: its accent seed and background level, such as "6750a4-100".
static void music_look(const struct mainui_frontend *fe, unsigned char seed[3], double *level, char *id, size_t size) {
    struct mainui_rgb rgb = mainui_appearance_seed(&fe->appearance);
    seed[0] = rgb.r;
    seed[1] = rgb.g;
    seed[2] = rgb.b;
    *level = mainui_appearance_background_level(&fe->appearance);
    snprintf(id, size, "%02x%02x%02x-%03d", rgb.r, rgb.g, rgb.b, (int)(*level * 100 + 0.5));
}

// The picture is for the official OSD only: the BrickOS OSD recolours the widget itself, and
// would recolour this picture a second time.
static int music_wanted(const struct mainui_frontend *fe) {
    if (fe->dry || !fe->music.write || fe->model.presentation != MAINUI_PRESENTATION_MATERIAL_YOU) return 0;
    char start[PATH_BYTES];
    path_under(fe, start, BRICKOS_OSDD_START);
    return access(start, F_OK) != 0;
}

static void *music_worker(void *context) {
    struct mainui_frontend *fe = context;
    struct mainui_music_job *job = fe->music.job;
    // The folder and its parents; the ones that exist already are kept.
    char path[PATH_BYTES];
    snprintf(path, sizeof(path), "%s", job->directory);
    for (char *slash = strchr(path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        mkdir(path, 0755);
        *slash = '/';
    }
    mkdir(path, 0755);
    job->result = job->write(job->source, job->destination, job->seed, job->level);
    __atomic_store_n(&fe->music.done, 1, __ATOMIC_RELEASE);
    return NULL;
}

// Starts the picture for the current look, unless the theme file already names it. A request
// while a picture is being written runs after it.
static void music_picture(struct mainui_frontend *fe) {
    if (!music_wanted(fe)) return;
    if (fe->music.running) {
        fe->music.again = 1;
        return;
    }
    fe->music.again = 0;
    if (!fe->music.job && !(fe->music.job = calloc(1, sizeof(*fe->music.job)))) return;
    struct mainui_music_job *job = fe->music.job;
    music_look(fe, job->seed, &job->level, job->id, sizeof(job->id));
    if (!strcmp(job->id, fe->music.named)) return;
    char device[96];
    snprintf(device, sizeof(device), MUSIC_PICTURES "/%s", job->id);
    path_under(fe, job->directory, device);
    path_under(fe, job->source, MUSIC_STOCK_PICTURE);
    snprintf(job->destination, sizeof(job->destination), "%s/music_default.png", job->directory);
    job->write = fe->music.write;
    job->result = -1;
    __atomic_store_n(&fe->music.done, 0, __ATOMIC_RELEASE);
    if (pthread_create(&fe->music.worker, NULL, music_worker, fe) != 0) {
        note(fe, "music picture worker not started");
        return;
    }
    fe->music.running = 1;
}

// Removes the folders of the other looks.
static void music_prune(struct mainui_frontend *fe, const char *keep) {
    char base[PATH_BYTES], folder[PATH_BYTES + 256], file[PATH_BYTES + 512];
    path_under(fe, base, MUSIC_PICTURES);
    DIR *dir = opendir(base);
    if (!dir) return;
    for (struct dirent *entry; (entry = readdir(dir));) {
        if (entry->d_name[0] == '.' || !strcmp(entry->d_name, keep)) continue;
        snprintf(folder, sizeof(folder), "%s/%s", base, entry->d_name);
        DIR *inner = opendir(folder);
        if (!inner) continue;
        for (struct dirent *item; (item = readdir(inner));) {
            if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
            snprintf(file, sizeof(file), "%s/%s", folder, item->d_name);
            unlink(file);
        }
        closedir(inner);
        rmdir(folder);
    }
    closedir(dir);
}

// A finished picture is named in the music server's theme file while it is still the look's
// and still wanted, and the other looks' folders go. A failure keeps the stock theme.
static int music_tick(struct mainui_frontend *fe) {
    if (!fe->music.running || !__atomic_load_n(&fe->music.done, __ATOMIC_ACQUIRE)) return 0;
    pthread_join(fe->music.worker, NULL);
    fe->music.running = 0;
    const struct mainui_music_job *job = fe->music.job;
    unsigned char seed[3];
    double level;
    char id[32];
    music_look(fe, seed, &level, id, sizeof(id));
    if (job->result != 0) {
        // An earlier look's picture may be named: the stock theme takes its place.
        char stock[600];
        if (fe->music.named[0] && mainui_sound_theme_file(fe->settings.theme, 0, stock, sizeof(stock)) == 0)
            write_text(fe, "/tmp/trimui_music/theme_file", stock);
        fe->music.named[0] = 0;
        note(fe, "music picture %s not written: stock theme kept", job->id);
    } else if (music_wanted(fe) && !strcmp(id, job->id)) {
        char text[96];
        snprintf(text, sizeof(text), MUSIC_PICTURES "/%s/", job->id);
        write_text(fe, "/tmp/trimui_music/theme_file", text);
        snprintf(fe->music.named, sizeof(fe->music.named), "%s", job->id);
        music_prune(fe, job->id);
        note(fe, "music theme file %s", text);
    }
    if (fe->music.again) music_picture(fe);
    return 0;
}

void mainui_frontend_music_writer(struct mainui_frontend *fe,
                                  int (*write)(const char *source, const char *destination,
                                               const unsigned char seed[3], double level)) {
    fe->music.write = write;
    music_picture(fe);
}

// BrickOS gives the official OSD the Default theme's look; the music server keeps the stock
// theme, until the BrickOS idle picture replaces it when that is wanted.
static void osd_theme_files(struct mainui_frontend *fe, int chosen) {
    char text[600], music[600];
    if (fe->dry || mainui_sound_theme_file(mainui_frontend_look_theme(fe), chosen, text, sizeof(text)) < 0 ||
        mainui_sound_theme_file(fe->settings.theme, chosen, music, sizeof(music)) < 0) return;
    write_text(fe, "/tmp/trimui_osd/theme_file", text);
    write_text(fe, "/tmp/trimui_music/theme_file", music);
    fe->music.named[0] = 0;
    note(fe, "theme file %s", text);
    if (strcmp(text, music)) note(fe, "music theme file %s", music);
    music_picture(fe);
}

static void osd_hotkey_file(struct mainui_frontend *fe) {
    char dir[PATH_BYTES];
    int v = fe->settings.values[MAINUI_SET_OSDSHORTCUT];
    if (fe->dry || v < 0 || v >= MAINUI_OSD_SHORTCUTS) return;
    path_under(fe, dir, "/tmp/trimui_osd");
    if (access(dir, F_OK) != 0) mkdir(dir, 0777);
    write_text(fe, "/tmp/trimui_osd/hotkeyshow", mainui_osd_shortcut_names[v]);
    note(fe, "hotkey file %s", mainui_osd_shortcut_names[v]);
}

static void persist_settings(struct mainui_frontend *fe);

// The official MainUI applies system.json each time it starts (static: loadSystemState):
// keymon's shared memory, then the display enhancement. It puts back what a child left.
static void apply_startup_state(struct mainui_frontend *fe) {
    const struct mainui_startup_state *st = &fe->settings.startup;
    note(fe, "startup state shm=%d colours=%d", st->shm_count, st->colour_count);
    if (fe->dry) return;
    for (int i = 0; i < st->shm_count && fe->ops.shm_set; ++i)
        if (fe->ops.shm_set(fe->ops.context, st->shm_index[i], st->shm_value[i]) != 0) ++fe->effects_failed;
    for (int i = 0; i < st->colour_count; ++i) {
        struct mainui_effect colour = {.kind=MAINUI_EFFECT_SET, .setting=st->colour_setting[i], .value=st->colour_value[i]};
        if (mainui_effects_apply(&fe->ops, &colour, fe->settings.values) < 0) ++fe->effects_failed;
    }
}

// The rest of the official sunxi_set_volume, after the card 0 mixer the VOL effect sets: card 1
// control 9 when /dev/dsp1 exists (0..20 as 0..175), the Bluetooth control named in
// /tmp/bt_alsa_volume_dev (0..127), and the speaker amplifier muted at 0 and unmuted above,
// unless /tmp/system/muted exists.
static void volume_outputs(struct mainui_frontend *fe, int v) {
    char path[PATH_BYTES], control[160] = "", command[256];
    if (fe->dry) return;
    path_under(fe, path, "/dev/dsp1");
    if (access(path, R_OK) == 0 && fe->ops.mixer_card_set &&
        fe->ops.mixer_card_set(fe->ops.context, 1, 9, (v < 0 ? 0 : v > 20 ? 20 : v) * 175 / 20) != 0)
        ++fe->effects_failed;
    path_under(fe, path, "/tmp/bt_alsa_volume_dev");
    FILE *file = fopen(path, "r");
    if (file) {
        if (!fgets(control, 128, file)) control[0] = 0;
        fclose(file);
        control[strcspn(control, "\n")] = 0;
        snprintf(command, sizeof(command), "amixer -D bluealsa sset \"%s\" %d", control, v * 127 / 20);
        run(fe, command);
    }
    path_under(fe, path, "/tmp/system/muted");
    if (access(path, F_OK) != 0 && fe->ops.write_file &&
        fe->ops.write_file(fe->ops.context, "/sys/class/speaker/mute", v > 0 ? "0" : "1") != 0)
        ++fe->effects_failed;
}

// The official check_process_exist: the first /proc entry whose argv[0] holds name, followed
// by its end or a space, gives its pid; -1 when there is none.
static int process_pid(struct mainui_frontend *fe, const char *name) {
    char path[PATH_BYTES], file[PATH_BYTES + 300], line[512];
    path_under(fe, path, "/proc");
    DIR *dir = opendir(path);
    if (!dir) return -1;
    size_t length = strlen(name);
    int pid = -1;
    for (struct dirent *entry; pid < 0 && (entry = readdir(dir));) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        snprintf(file, sizeof(file), "%s/%s/cmdline", path, entry->d_name);
        int fd = open(file, O_RDONLY | O_CLOEXEC);
        ssize_t n = fd >= 0 ? read(fd, line, sizeof(line) - 1) : 0;
        if (fd >= 0) close(fd);
        line[n > 0 ? n : 0] = 0;
        const char *found = strlen(line) >= length ? strstr(line, name) : NULL;
        if (found && (found[length] == 0 || found[length] == ' ')) pid = (int)strtol(entry->d_name, NULL, 10);
    }
    closedir(dir);
    return pid;
}

// The official start's flags from features.json, on the card or else in the firmware:
// "bluetooth" allows Bluetooth (0 when absent), "suspend" 0 keeps the console awake.
static void startup_features(struct mainui_frontend *fe, int *bluetooth) {
    static const char *const paths[] = {"/mnt/SDCARD/features.json", "/usr/trimui/features.json"};
    static struct mainui_json json;
    char path[PATH_BYTES], text[4096];
    *bluetooth = 0;
    for (int i = 0; i < 2; ++i) {
        path_under(fe, path, paths[i]);
        FILE *file = fopen(path, "rb");
        if (!file) continue;
        size_t n = fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
        text[n] = 0;
        if (mainui_json_parse(&json, text, n) < 0) return;
        const struct mainui_json_member *m = mainui_json_get(&json, "bluetooth");
        if (m && m->type == MAINUI_JSON_NUMBER) *bluetooth = (int)m->number;
        m = mainui_json_get(&json, "suspend");
        if (m && m->type == MAINUI_JSON_NUMBER && (int)m->number == 0) run(fe, "touch /tmp/stay_awake");
        return;
    }
}

static int keymon_or(struct mainui_frontend *fe, int index, int fallback) {
    int value;
    return fe->ops.shm_get && fe->ops.shm_get(fe->ops.context, index, &value) == 0 ? value : fallback;
}

#define CPU "/sys/devices/system/cpu/cpu0/cpufreq/"
#define WIFI_MAC "ifconfig | grep HWaddr | sed 's/[ ]\\+/ /g' | cut -d' ' -f5 > /tmp/wifimac"

// What the official start does after loadSystemState (static: main): SSH off, a Wi-Fi client
// or hotspot that should run but does not, Bluetooth off when unused, the volume report
// cleared, the LEDs, network time and the CPU frequency policy. These put back what a child
// changed. The official kills some programs by pid; BrickOS sends them the same signal with
// kill, through the same runner as the other commands.
static void startup_services(struct mainui_frontend *fe, int bluetooth_feature) {
    char command[64];
    const int *v = fe->settings.values;
    // Stopping SSH runs its init script; with no daemon running it would stop nothing.
    if (!v[MAINUI_SET_ENABLESSH] && process_pid(fe, "sshd") > 0) run(fe, "/etc/init.d/sshd stop");
    int pid;
    if (keymon_or(fe, MAINUI_SHM_WIFI, v[MAINUI_SET_WIFI])) {
        if (process_pid(fe, "wpa_supplicant") < 0) {
            note(fe, "startup wifi client restart");
            if ((pid = process_pid(fe, "udhcpd")) > 0) { snprintf(command, sizeof(command), "kill -9 %d", pid); run(fe, command); }
            run(fe, "ifconfig wlan0 down");
            run(fe, "killall -15 wpa_supplicant");
            run(fe, "killall -9 udhcpc");
            run(fe, "ifconfig wlan0 up");
            run(fe, "wpa_supplicant -B -D nl80211 -iwlan0 -c /etc/wifi/wpa_supplicant.conf -O /etc/wifi/sockets");
            run(fe, "udhcpc -i wlan0 &");
            run(fe, WIFI_MAC);
        } else if (process_pid(fe, "udhcpc") < 0) {
            // A stock defect: at boot the firmware's init starts wpa_supplicant, which joins the
            // network, and nothing starts its DHCP client, so the Brick has no address until Wi-Fi
            // is switched off and on. The client starts here, as that switch starts it.
            note(fe, "startup wifi address");
            run(fe, "udhcpc -i wlan0 &");
        }
    } else if (keymon_or(fe, MAINUI_SHM_HOSTAP, v[MAINUI_SET_HOSTAP]) && process_pid(fe, "hostapd") < 0) {
        note(fe, "startup hotspot restart");
        if ((pid = process_pid(fe, "wpa_supplicant")) > 0) { snprintf(command, sizeof(command), "kill -15 %d", pid); run(fe, command); }
        if ((pid = process_pid(fe, "udhcpc")) > 0) { snprintf(command, sizeof(command), "kill -15 %d", pid); run(fe, command); }
        run(fe, "ifconfig wlan0 down");
        run(fe, "killall  -9 udhcpd");
        run(fe, "killall  -15 hostapd");
        run(fe, "ifconfig wlan0 192.168.0.18 up");
        run(fe, "sleep 0.5");
        run(fe, "hostapd /etc/wifi/hostapd.conf -B");
        run(fe, "udhcpd /etc/wifi/udhcpd.conf -f &");
        run(fe, WIFI_MAC);
    }
    if (!bluetooth_feature || keymon_or(fe, MAINUI_SHM_BLUETOOTH, v[MAINUI_SET_BLUETOOTH]) != 1)
        run(fe, "touch /tmp/system/bluetooth_turn_off");
    if (fe->dry) return;
    // Dpad to Analog Key(hold) acts only outside the menu: its inputd marker is removed here
    // and made again on exit (mainui_frontend_finish).
    char path[PATH_BYTES];
    path_under(fe, path, "/tmp/trimui_inputd");
    mkdir(path, 0777);
    if (v[MAINUI_SET_DPAD2AXIS] > 0 && v[MAINUI_SET_DPAD2AXIS] < 7) {
        path_under(fe, path, mainui_dpad2axis_values[v[MAINUI_SET_DPAD2AXIS]]);
        unlink(path);
    }
    // A volume report from before the start makes no click (SetKeyShm(4, 0) before the loop).
    if (fe->ops.shm_set && fe->ops.shm_set(fe->ops.context, MAINUI_SHM_VOL_CHANGED, 0) != 0) ++fe->effects_failed;
    // The LED switch and scales as keymon holds them (the switch effect also stores the
    // same switch value again).
    struct mainui_effect led = {.kind=MAINUI_EFFECT_SET, .setting=MAINUI_SET_LEDSWITCH,
                                .value=keymon_or(fe, MAINUI_SHM_LEDSWITCH, v[MAINUI_SET_LEDSWITCH]) == 1};
    if (mainui_effects_apply(&fe->ops, &led, fe->settings.values) < 0) ++fe->effects_failed;
}

// The official exit, a launch handoff included (static: main after runWindows): every
// Dpad to Analog Key(hold) marker is removed, and the chosen one is touched for the child.
void mainui_frontend_finish(struct mainui_frontend *fe) {
    char path[PATH_BYTES], command[128];
    ext_stop(fe);
    if (fe->dry) return;
    for (int i = 1; i < 7; ++i) {
        path_under(fe, path, mainui_dpad2axis_values[i]);
        unlink(path);
    }
    int v = fe->settings.values[MAINUI_SET_DPAD2AXIS];
    if (v > 0 && v < 7) {
        snprintf(command, sizeof(command), "touch %s", mainui_dpad2axis_values[v]);
        run(fe, command);
    }
}

// The official start's card folders (static: main before initLang): each built-in system's
// ROM folder with its emulator license, then its image folder.
static const struct { const char *roms, *license, *copy, *source, *imgs; } start_folders[] = {
    {"/mnt/SDCARD/Roms/ARCADE/", "/mnt/SDCARD/Roms/ARCADE//COPYING",
     "cp /usr/trimui/res/licenses/mame/COPYING /mnt/SDCARD/Roms/ARCADE/",
     "/usr/trimui/res/licenses/mame/COPYING", "/mnt/SDCARD/Imgs/ARCADE/"},
    {"/mnt/SDCARD/Roms/NEOGEO/", "/mnt/SDCARD/Roms/NEOGEO//COPYING",
     "cp /usr/trimui/res/licenses/mame/COPYING /mnt/SDCARD/Roms/NEOGEO/",
     "/usr/trimui/res/licenses/mame/COPYING", "/mnt/SDCARD/Imgs/NEOGEO/"},
    {"/mnt/SDCARD/Roms/FC/", "/mnt/SDCARD/Roms/FC//COPYING",
     "cp /usr/trimui/res/licenses/fceux/COPYING /mnt/SDCARD/Roms/FC/",
     "/usr/trimui/res/licenses/fceux/COPYING", "/mnt/SDCARD/Imgs/FC/"},
    {"/mnt/SDCARD/Roms/SFC/", "/mnt/SDCARD/Roms/SFC//LICENSE",
     "cp /usr/trimui/res/licenses/bsnes/LICENSE /mnt/SDCARD/Roms/SFC/",
     "/usr/trimui/res/licenses/bsnes/LICENSE", "/mnt/SDCARD/Imgs/SFC/"},
    {"/mnt/SDCARD/Roms/GB/", "/mnt/SDCARD/Roms/GB//COPYING",
     "cp /usr/trimui/res/licenses/gamebatte/COPYING /mnt/SDCARD/Roms/GB/",
     "/usr/trimui/res/licenses/gamebatte/COPYING", "/mnt/SDCARD/Imgs/GB/"},
    {"/mnt/SDCARD/Roms/MD/", "/mnt/SDCARD/Roms/MD//COPYING",
     "cp /usr/trimui/res/licenses/picodrive/COPYING /mnt/SDCARD/Roms/MD/",
     "/usr/trimui/res/licenses/picodrive/COPYING", "/mnt/SDCARD/Imgs/MD/"},
    {"/mnt/SDCARD/Roms/GBA/", "/mnt/SDCARD/Roms/GBA//COPYING.DOC",
     "cp /usr/trimui/res/licenses/gPsp/COPYING.DOC /mnt/SDCARD/Roms/GBA/",
     "/usr/trimui/res/licenses/gPsp/COPYING.DOC", "/mnt/SDCARD/Imgs/GBA/"},
    {"/mnt/SDCARD/Roms/MS/", "/mnt/SDCARD/Roms/MS//license",
     "cp /usr/trimui/res/licenses/sms/license /mnt/SDCARD/Roms/MS/",
     "/usr/trimui/res/licenses/sms/license", "/mnt/SDCARD/Imgs/MS/"},
    {"/mnt/SDCARD/Roms/PCE/", "/mnt/SDCARD/Roms/PCE//COPYING",
     "cp /usr/trimui/res/licenses/temper/COPYING /mnt/SDCARD/Roms/PCE/",
     "/usr/trimui/res/licenses/temper/COPYING", "/mnt/SDCARD/Imgs/PCE/"},
    {"/mnt/SDCARD/Roms/PS", "/mnt/SDCARD/Roms/PS/COPYING",
     "cp /usr/trimui/res/licenses/pcsx_rearmed/COPYING /mnt/SDCARD/Roms/PS",
     "/usr/trimui/res/licenses/pcsx_rearmed/COPYING", "/mnt/SDCARD/Imgs/PS"},
};

static void folder_when_missing(struct mainui_frontend *fe, const char *folder) {
    char path[PATH_BYTES];
    path_under(fe, path, folder);
    if (access(path, F_OK) != 0) mkdir(path, 0777);
}

// Unless the card belongs to another platform, Apps, Roms and Imgs and each system's folders
// are made when missing, and a license the ROM folder lacks is copied into it. A card that
// gained its folders elsewhere keeps them. A firmware without the license folder copies
// nothing: the official's copies fail there at every start.
static void startup_folders(struct mainui_frontend *fe) {
    char path[PATH_BYTES];
    if (mainui_launch_other_platform_card(fe->fs.root)) return;
    folder_when_missing(fe, "/mnt/SDCARD/Apps");
    folder_when_missing(fe, "/mnt/SDCARD/Roms");
    folder_when_missing(fe, "/mnt/SDCARD/Imgs");
    int count = (int)(sizeof(start_folders) / sizeof(start_folders[0]));
    for (int i = 0; i < count; ++i) {
        folder_when_missing(fe, start_folders[i].roms);
        path_under(fe, path, start_folders[i].license);
        if (access(path, F_OK) == 0) continue;
        path_under(fe, path, start_folders[i].source);
        if (access(path, F_OK) == 0) run_unchecked(fe, start_folders[i].copy);
    }
    for (int i = 0; i < count; ++i) folder_when_missing(fe, start_folders[i].imgs);
}

// Network time and the CPU policy come after the time zone link, as in the official.
static void startup_services_late(struct mainui_frontend *fe) {
    int ntpd = process_pid(fe, "ntpd");
    if (fe->settings.values[MAINUI_SET_USENETWORKTIME] && ntpd < 0) {
        run(fe, "/etc/init.d/sysntpd enable");
        run(fe, "/etc/init.d/ntpd restart");
    } else if (!fe->settings.values[MAINUI_SET_USENETWORKTIME] && ntpd > 0) {
        run(fe, "/etc/init.d/sysntpd disable");
        run(fe, "/etc/init.d/ntpd stop");
    }
    if (fe->dry) return;
    startup_folders(fe);
    // The volume in keymon's memory is applied again (static: sunxi_set_volume after
    // loadEmusApps): a negative value sets the mixer's 63, one above 20 its 0.
    int vol = keymon_or(fe, MAINUI_SHM_VOL, fe->settings.values[MAINUI_SET_VOL]);
    if (fe->ops.mixer_set && fe->ops.mixer_set(fe->ops.context, MAINUI_MIXER_DIGITAL_VOLUME,
                                               vol < 0 ? 63 : vol > 20 ? 0 : mainui_volume_mixer[vol]) != 0)
        ++fe->effects_failed;
    volume_outputs(fe, vol);
    // The buttons return to their normal order, whatever keymap the last game ran with, and
    // inputd's rotation marker goes (static: main after sunxi_set_volume). The order is written
    // only when the parameter opens, so a failure is not counted.
    static const char normal_keys[] = "A,B,X,Y,L,R";
    if (fe->ops.write_bytes)
        fe->ops.write_bytes(fe->ops.context, "/sys/module/gpio_keys_polled/parameters/button_config",
                            normal_keys, sizeof(normal_keys));
    char marker[PATH_BYTES];
    path_under(fe, marker, "/var/trimui_inputd/rotate_270");
    if (access(marker, F_OK) == 0) run_unchecked(fe, "rm /var/trimui_inputd/rotate_270");
    // The store's internal folders, made when missing.
    static const char *const folders[] = {"/mnt/UDISK/Store/", "/mnt/UDISK/Apps", "/mnt/UDISK/Downloads"};
    for (int i = 0; i < 3; ++i) {
        char path[PATH_BYTES];
        path_under(fe, path, folders[i]);
        if (access(path, F_OK) != 0) mkdir(path, 0777);
    }
    if (!fe->ops.write_file) return;
    if (fe->ops.write_file(fe->ops.context, CPU "scaling_governor", "ondemand") != 0 ||
        fe->ops.write_file(fe->ops.context, CPU "scaling_min_freq", "1200000") != 0 ||
        fe->ops.write_file(fe->ops.context, CPU "scaling_max_freq", "2000000") != 0) ++fe->effects_failed;
}

void mainui_frontend_start(struct mainui_frontend *fe) {
    char command[512];
    int bluetooth_feature;
    // The init consumed the preceding handoff before starting this frontend. A host
    // advertisement belongs only to that game, never to a later launch or menu session.
    if (!fe->dry) mainui_launch_clear(fe->fs.root);
    startup_features(fe, &bluetooth_feature);
    apply_startup_state(fe);
    startup_services(fe, bluetooth_feature);
    if (fe->settings.changed & (1ULL << MAINUI_SET_OSDSHORTCUT)) persist_settings(fe);
    fe->model.settings[MAINUI_SET_OSDSHORTCUT] = fe->settings.values[MAINUI_SET_OSDSHORTCUT];
    if (!fe->dry) {
        struct mainui_effect shortcut = {.kind=MAINUI_EFFECT_SET, .setting=MAINUI_SET_OSDSHORTCUT,
                                         .value=fe->settings.values[MAINUI_SET_OSDSHORTCUT]};
        if (mainui_effects_apply(&fe->ops, &shortcut, fe->settings.values) < 0) ++fe->effects_failed;
    }
    osd_theme_files(fe, 0);
    osd_hotkey_file(fe);
    // MainUI relinks a stored zone when it starts (static: main), unless the link already names
    // it; the clock reads the zone either way.
    if (fe->settings.timezone[0]) {
        char link[PATH_BYTES], target[PATH_BYTES], current[PATH_BYTES];
        path_under(fe, link, "/tmp/localtime");
        snprintf(target, sizeof(target), MAINUI_ZONEINFO "/%s", fe->settings.timezone);
        ssize_t length = readlink(link, current, sizeof(current) - 1);
        int linked = length > 0 && (size_t)length == strlen(target) && memcmp(current, target, (size_t)length) == 0;
        note(fe, "timezone start %s%s", fe->settings.timezone, linked ? " (linked)" : "");
        if (linked ? !fe->dry && fe->runner.run :
            mainui_zone_command(fe->settings.timezone, command, sizeof(command)) == 0 && run(fe, command) == 0 && !fe->dry &&
                fe->runner.run)
            reload_zone(fe);
    }
    startup_services_late(fe);
    power_request(fe); // A startup marker precedes the first ordinary screen, as in runWindows.
}

// The hotspot's name or password: /etc/wifi/hostapd.conf is rewritten (static:
// HotspotWriteAction replaces the lines). An empty name is ignored. A new name restarts a
// running hotspot so it takes effect at once; a new password waits for the next start.
static void edit_hotspot(struct mainui_frontend *fe, int password, const char *value) {
    static char text[8192], out[8400];
    char path[PATH_BYTES], temporary[PATH_BYTES + 8];
    note(fe, "hotspot %s", password ? "password" : "ssid");
    if (!password && !value[0]) return;
    path_under(fe, path, "/etc/wifi/hostapd.conf");
    FILE *f = fopen(path, "rb");
    if (!f) { ++fe->effects_failed; return; }
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    if (mainui_hostapd_edit(text, password ? "wpa_passphrase" : "ssid", value, out, sizeof(out)) < 0) { ++fe->effects_failed; return; }
    copy_value(password ? fe->hotspot_password : fe->hotspot_ssid, sizeof(fe->hotspot_ssid), value);
    if (fe->dry) return;
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    f = fopen(temporary, "wb");
    int ok = f && fwrite(out, 1, strlen(out), f) == strlen(out);
    if (f && fclose(f) != 0) ok = 0;
    if (!ok || rename(temporary, path) != 0) { remove(temporary); ++fe->effects_failed; return; }
    if (!password && fe->settings.values[MAINUI_SET_HOSTAP]) {
        const char *lines[8];
        int count = mainui_hotspot_commands(1, fe->settings.values[MAINUI_SET_WIFI], lines, 8);
        for (int i = 0; i < count && i < 8; ++i) run(fe, lines[i]);
    }
}

static void carry_out_all(struct mainui_frontend *fe);

#define NOTICE_MS 5000

static void netplay_cover_remove(struct mainui_frontend *fe, const char *path) {
    char full[PATH_BYTES];
    if (!path[0]) return;
    path_under(fe, full, path);
    unlink(full);
}

static void *netplay_cover_worker(void *context) {
    struct mainui_frontend *fe = context;
    char full[PATH_BYTES];
    path_under(fe, full, fe->cover_file);
    fe->cover_result = mainui_netplay_cover(fe->cover_host.hostip, fe->cover_host.iconpath,
                                           fe->cover_port, full, 2500, &fe->cover_stop);
    fe->cover_cached = !fe->cover_result && !__atomic_load_n(&fe->cover_stop, __ATOMIC_ACQUIRE) &&
        mainui_netplay_cover_cache(fe->fs.root, fe->cover_host.iconpath, full,
                                   fe->cover_cache_file, sizeof(fe->cover_cache_file)) == 0;
    __atomic_store_n(&fe->cover_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int netplay_cover_tick(struct mainui_frontend *fe, uint64_t now_ms, int active) {
    int changed = 0;
    if (fe->cover_running && __atomic_load_n(&fe->cover_done, __ATOMIC_ACQUIRE)) {
        pthread_join(fe->cover_thread, NULL);
        fe->cover_running = 0;
        int found = 0;
        for (int i = 0; i < fe->host_count; ++i) {
            struct mainui_netplay_host *host = &fe->hosts[i];
            if (strcmp(host->label, fe->cover_host.label) || strcmp(host->hostip, fe->cover_host.hostip) ||
                strcmp(host->iconpath, fe->cover_host.iconpath)) continue;
            if (!fe->cover_result) {
                snprintf(host->coverpath, sizeof(host->coverpath), "%s", fe->cover_cached ? fe->cover_cache_file : fe->cover_file);
                host->cover_temporary = !fe->cover_cached;
                changed = found = 1;
            } else {
                host->cover_attempted = 0;
                host->cover_after_ms = now_ms + 3000;
            }
            break;
        }
        if (!found || fe->cover_cached) netplay_cover_remove(fe, fe->cover_file);
    }
    if (active) {
        for (int i = 0; i < fe->host_count; ++i) {
            struct mainui_netplay_host *host = &fe->hosts[i];
            if (!host->coverpath[0] && !host->cover_attempted &&
                mainui_netplay_cover_local(fe->fs.root, host->iconpath, host->coverpath, sizeof(host->coverpath)) == 0) {
                host->cover_temporary = 0;
                host->cover_attempted = 1;
                changed = 1;
            }
        }
    }
    if (!active || fe->dry || !fe->cover_enabled || fe->cover_running || !fe->host_count) return changed;
    for (int offset = 0; offset < fe->host_count; ++offset) {
        unsigned at = (fe->cover_next + (unsigned)offset) % (unsigned)fe->host_count;
        struct mainui_netplay_host *host = &fe->hosts[at];
        if (!host->iconpath[0] || host->cover_attempted || now_ms < host->cover_after_ms) continue;
        if (!fe->cover_directory[0]) {
            char full[PATH_BYTES];
            path_under(fe, full, "/tmp/brickos-netplay-XXXXXX");
            if (!mkdtemp(full)) return changed;
            snprintf(fe->cover_directory, sizeof(fe->cover_directory), "%s", full + strlen(fe->fs.root));
        }
        int written = snprintf(fe->cover_file, sizeof(fe->cover_file), "%s/%u", fe->cover_directory, ++fe->cover_sequence);
        if (written < 0 || written >= (int)sizeof(fe->cover_file)) return changed;
        fe->cover_host = *host;
        __atomic_store_n(&fe->cover_done, 0, __ATOMIC_RELEASE);
        if (pthread_create(&fe->cover_thread, NULL, netplay_cover_worker, fe) != 0) return changed;
        fe->cover_running = host->cover_attempted = 1;
        fe->cover_next = at + 1;
        break;
    }
    return changed;
}

static void netplay_cover_close(struct mainui_frontend *fe) {
    __atomic_store_n(&fe->cover_stop, 1, __ATOMIC_RELEASE);
    if (fe->cover_running) pthread_join(fe->cover_thread, NULL);
    fe->cover_running = 0;
    for (int i = 0; i < fe->host_count; ++i)
        if (fe->hosts[i].cover_temporary) netplay_cover_remove(fe, fe->hosts[i].coverpath);
    netplay_cover_remove(fe, fe->cover_file);
    if (fe->cover_directory[0]) {
        char full[PATH_BYTES];
        path_under(fe, full, fe->cover_directory);
        rmdir(full);
    }
}

static int netplay_tick(struct mainui_frontend *fe, uint64_t now_ms) {
    int active = fe->model.tab == MAINUI_TAB_NETPLAY && fe->model.stack[0].kind == MAINUI_WIN_TABS;
    int changed = netplay_cover_tick(fe, now_ms, 0);
    if (!active) {
        if (fe->netplay_fd >= 0) close(fe->netplay_fd);
        fe->netplay_fd = -1;
        return changed;
    }
    if (fe->netplay_fd < 0 && !fe->dry && !fe->fs.root[0] && now_ms >= fe->netplay_retry_ms) {
        fe->netplay_fd = mainui_netplay_listen(MAINUI_NETPLAY_PORT);
        fe->netplay_retry_ms = now_ms + 1000;
    }
    for (int i = 0; i < fe->host_count; ++i)
        if (fe->hosts[i].cover_temporary && now_ms >= fe->hosts[i].seen_ms && now_ms - fe->hosts[i].seen_ms > 3000)
            netplay_cover_remove(fe, fe->hosts[i].coverpath);
    changed |= mainui_netplay_expire(fe->hosts, &fe->host_count, now_ms);
    for (int i = 0; fe->netplay_fd >= 0 && i < 16; ++i) {
        char text[1025];
        ssize_t length = recv(fe->netplay_fd, text, sizeof(text), MSG_DONTWAIT);
        if (length < 0) break;
        struct mainui_netplay_host host;
        if (mainui_netplay_parse(text, (size_t)length, &host) < 0 || !host_system(fe, &host)) continue;
        changed |= mainui_netplay_receive(fe->hosts, &fe->host_count, text, (size_t)length, now_ms);
    }
    struct mainui_tab_state *t = &fe->model.tabs[MAINUI_TAB_NETPLAY];
    if (t->focus >= fe->host_count) t->focus = fe->host_count ? fe->host_count - 1 : 0;
    if (t->first > t->focus) t->first = t->focus;
    changed |= netplay_cover_tick(fe, now_ms, 1);
    return changed;
}

// ---- Backdrop and card motion ---------------------------------------------------------------

// The stock cards jump, and the background waits until the focus has stayed 500 ms, then fades
// over 500 ms: about 1.3 s from the key on the device. In BrickOS the one-row cards slide to the
// new focus and the background fades in with them, both over MAINUI_MOTION_MS on one easing.

float mainui_frontend_motion_progress_over(uint64_t start_ms, uint64_t now_ms, int duration_ms) {
    if (!start_ms || duration_ms <= 0 || now_ms >= start_ms + (uint64_t)duration_ms) return 1;
    if (now_ms <= start_ms) return 0;
    float left = 1.f - (float)(now_ms - start_ms) / (float)duration_ms;
    return 1.f - left * left * left;
}

float mainui_frontend_motion_progress(uint64_t start_ms, uint64_t now_ms) {
    return mainui_frontend_motion_progress_over(start_ms, now_ms, MAINUI_MOTION_MS);
}

int mainui_frontend_motion_ms(const struct mainui_frontend *fe) {
    if (fe->model.presentation != MAINUI_PRESENTATION_MATERIAL_YOU) return MAINUI_MOTION_MS;
    return (int)(MAINUI_MOTION_MS * mainui_appearance_motion(&fe->appearance) + 0.5);
}

int mainui_frontend_backdrop_alpha(const struct mainui_frontend *fe, uint64_t now_ms) {
    return (int)(255.f * mainui_frontend_motion_progress(fe->backdrop.start_ms, now_ms));
}

float mainui_frontend_carousel_shift(const struct mainui_frontend *fe, uint64_t now_ms) {
    if (!fe->carousel.valid) return 0;
    return fe->carousel.from * (1.f - mainui_frontend_motion_progress_over(fe->carousel.start_ms, now_ms,
                                                                            fe->carousel.duration_ms));
}

// static: BGEffectWorker::work runs system() on the script's path alone.
static void *backdrop_worker(void *context) {
    struct mainui_frontend *fe = context;
    char command[sizeof(fe->backdrop.queued)];
    pthread_mutex_lock(&fe->backdrop.lock);
    for (;;) {
        while (!fe->backdrop.has_queued && !fe->backdrop.stop) pthread_cond_wait(&fe->backdrop.wake, &fe->backdrop.lock);
        if (fe->backdrop.stop) break;
        memcpy(command, fe->backdrop.queued, sizeof(command));
        fe->backdrop.has_queued = 0;
        pthread_mutex_unlock(&fe->backdrop.lock);
        (void)fe->runner.run(fe->runner.context, command);
        pthread_mutex_lock(&fe->backdrop.lock);
    }
    pthread_mutex_unlock(&fe->backdrop.lock);
    return NULL;
}

static void backdrop_effect(struct mainui_frontend *fe, const char *script) {
    if (fe->dry || !fe->runner.run) return;
    if (!fe->backdrop.thread_running) {
        if (pthread_mutex_init(&fe->backdrop.lock, NULL) != 0) { ++fe->effects_failed; return; }
        if (pthread_cond_init(&fe->backdrop.wake, NULL) != 0) {
            pthread_mutex_destroy(&fe->backdrop.lock);
            ++fe->effects_failed;
            return;
        }
        fe->backdrop.stop = fe->backdrop.has_queued = 0;
        if (pthread_create(&fe->backdrop.thread, NULL, backdrop_worker, fe) != 0) {
            pthread_cond_destroy(&fe->backdrop.wake);
            pthread_mutex_destroy(&fe->backdrop.lock);
            ++fe->effects_failed;
            return;
        }
        fe->backdrop.thread_running = 1;
    }
    pthread_mutex_lock(&fe->backdrop.lock);
    snprintf(fe->backdrop.queued, sizeof(fe->backdrop.queued), "%s", script);
    fe->backdrop.has_queued = 1;
    pthread_cond_signal(&fe->backdrop.wake);
    pthread_mutex_unlock(&fe->backdrop.lock);
}

// A script still waiting is dropped; one already running finishes first.
static void backdrop_close(struct mainui_frontend *fe) {
    if (!fe->backdrop.thread_running) return;
    pthread_mutex_lock(&fe->backdrop.lock);
    fe->backdrop.stop = 1;
    pthread_cond_signal(&fe->backdrop.wake);
    pthread_mutex_unlock(&fe->backdrop.lock);
    pthread_join(fe->backdrop.thread, NULL);
    pthread_cond_destroy(&fe->backdrop.wake);
    pthread_mutex_destroy(&fe->backdrop.lock);
    fe->backdrop.thread_running = 0;
}

// Without an effect script, the colour flashes once on each LED zone (static: setLEDLR, M,
// F1 and F2 from draw_back_internal). The official ignores failed writes.
static void backdrop_leds(struct mainui_frontend *fe, const char *color) {
    static const char *const zones[4] = {"lr", "m", "f1", "f2"};
    char path[64];
    if (fe->dry || !fe->ops.write_file) return;
    for (int i = 0; i < 4; ++i) {
        (void)fe->ops.write_file(fe->ops.context, "/sys/class/led_anim/effect_enable", "1");
        (void)fe->ops.write_file(fe->ops.context, "/sys/class/led_anim/anim_frames_enable", "0");
        snprintf(path, sizeof(path), "/sys/class/led_anim/effect_rgb_hex_%s", zones[i]);
        (void)fe->ops.write_file(fe->ops.context, path, color);
        snprintf(path, sizeof(path), "/sys/class/led_anim/effect_cycles_%s", zones[i]);
        (void)fe->ops.write_file(fe->ops.context, path, "1");
        snprintf(path, sizeof(path), "/sys/class/led_anim/effect_duration_%s", zones[i]);
        (void)fe->ops.write_file(fe->ops.context, path, "500");
        snprintf(path, sizeof(path), "/sys/class/led_anim/effect_%s", zones[i]);
        (void)fe->ops.write_file(fe->ops.context, path, "1");
    }
}

// The focused item's backdrop on a home tab: a system on Games and Bests, an app on Apps, a
// port on Ports, none on the others.
static void tab_backdrop(const struct mainui_frontend *fe, struct mainui_backdrop *out) {
    enum mainui_tab tab = fe->model.tab;
    int focus = fe->model.tabs[tab].focus;
    const struct mainui_system *s = NULL;
    memset(out, 0, sizeof(*out));
    if (focus < 0) return;
    if (tab == MAINUI_TAB_GAMES) s = mainui_frontend_system(fe, focus);
    else if (tab == MAINUI_TAB_BESTS) s = mainui_frontend_system(fe, MAINUI_BEST_SYSTEM_BASE + focus);
    if (s) *out = s->backdrop;
    else if (tab == MAINUI_TAB_APPS && focus < fe->shown_app_count) *out = fe->apps[fe->shown_apps[focus]].backdrop;
    else if (tab == MAINUI_TAB_PORTS && focus < fe->shown_port_count) *out = fe->ports[fe->shown_ports[focus]].backdrop;
}

// What the window under any popup draws behind it (static: callers of draw_back). The home
// tabs take the focused item's backdrop at once and keep it while another window is open; a
// system's ROM list and search take the system's; the other windows show the theme's. A new
// background fades in with the cards. Its effect script runs, or else, with the LEDs following
// the theme, its colour shows, once it has stayed for the whole motion: a held direction
// changes the picture with each card but runs only the effect of the item it stops on.
static int backdrop_tick(struct mainui_frontend *fe, const struct mainui_window *screen, uint64_t now_ms) {
    struct mainui_backdrop want;
    memset(&want, 0, sizeof(want));
    if (screen->kind == MAINUI_WIN_TABS) {
        tab_backdrop(fe, &want);
    } else if ((screen->kind == MAINUI_WIN_ROMS || screen->kind == MAINUI_WIN_SEARCH) && screen->system >= 0) {
        const struct mainui_system *s = mainui_frontend_system(fe, screen->system);
        if (s) want = s->backdrop;
    }
    int changed = 0;
    if (strcmp(want.background, fe->backdrop.path) != 0) {
        // A fade cut short leaves the picture it mostly showed to fade from; the official keeps
        // the exact blend.
        if (mainui_frontend_backdrop_alpha(fe, now_ms) >= 128)
            memcpy(fe->backdrop.from, fe->backdrop.path, sizeof(fe->backdrop.from));
        memcpy(fe->backdrop.path, want.background, sizeof(fe->backdrop.path));
        fe->backdrop.start_ms = now_ms ? now_ms : 1;
        ++fe->backdrop.serial;
        fe->backdrop.shown = want;
        fe->backdrop.effect_due = 1;
        note(fe, "backdrop %s effect=%s color=%s", want.background[0] ? want.background : "theme", want.effectsh,
             want.themecolor);
        changed = 1;
    }
    if (fe->backdrop.effect_due && now_ms >= fe->backdrop.start_ms + MAINUI_MOTION_MS) {
        const struct mainui_backdrop *shown = &fe->backdrop.shown;
        fe->backdrop.effect_due = 0;
        if (shown->effectsh[0]) backdrop_effect(fe, shown->effectsh);
        else if (shown->themecolor[0] && keymon_or(fe, MAINUI_SHM_LEDPOLICY, fe->settings.values[MAINUI_SET_LEDPOLICY]) == 1)
            backdrop_leds(fe, shown->themecolor);
    }
    return changed;
}

static int tab_item_count(const struct mainui_frontend *fe, enum mainui_tab tab) {
    return tab == MAINUI_TAB_GAMES ? fe->visible_count : tab == MAINUI_TAB_BESTS ? fe->shown_best_count
         : tab == MAINUI_TAB_APPS  ? fe->shown_app_count : tab == MAINUI_TAB_PORTS ? fe->shown_port_count : 0;
}

// The one-row cards (Stock++, with the focus on the cards) slide when the focus moves to a
// neighbor, from wherever a slide still under way has brought them, never more than two cards
// behind. A jump, another tab or another window puts them in place at once.
// The carousel the screen shows, with its item count and focus; 0 when it shows none. Stock++:
// the one-row layout of the tabs. BrickOS: the one-row layout of Games, Bests, Apps and Ports,
// and a game list, Recent or Favorites in the Carousel view.
static int carousel_screen(struct mainui_frontend *fe, const struct mainui_window *screen, int *n, int *focus) {
    struct mainui_model *m = &fe->model;
    enum mainui_tab tab = m->tab;
    int brickos = m->presentation == MAINUI_PRESENTATION_MATERIAL_YOU;
    if (m->presentation != MAINUI_PRESENTATION_STOCK && !brickos) return 0;
    if (screen->kind == MAINUI_WIN_TABS && !m->bar_focused) {
        *focus = m->tabs[tab].focus;
        if (brickos && mainui_model_view(m, screen) == MAINUI_VIEW_CAROUSEL) {
            *n = tab == MAINUI_TAB_RECENT ? fe->recent_count : fe->favorites_count;
            return 1;
        }
        if (brickos && tab != MAINUI_TAB_GAMES && tab != MAINUI_TAB_BESTS && tab != MAINUI_TAB_APPS && tab != MAINUI_TAB_PORTS)
            return 0;
        *n = tab_item_count(fe, tab);
        return mainui_model_tab_layout(m, tab) == 0;
    }
    if (brickos && screen->kind == MAINUI_WIN_ROMS && mainui_model_view(m, screen) == MAINUI_VIEW_CAROUSEL) {
        *focus = screen->focus;
        *n = mainui_frontend_roms(fe, screen->system);
        return 1;
    }
    return 0;
}

static int carousel_follow(struct mainui_frontend *fe, const struct mainui_window *screen, uint64_t now_ms) {
    enum mainui_tab tab = fe->model.tab;
    int n = 0, focus = 0, jump = fe->carousel.jump;
    int depth = (int)(screen - fe->model.stack);
    fe->carousel.jump = 0;
    if (!carousel_screen(fe, screen, &n, &focus) || n < 2) {
        fe->carousel.valid = 0;
        return 0;
    }
    if (!fe->carousel.valid || fe->carousel.presentation != (int)fe->model.presentation || fe->carousel.kind != (int)screen->kind ||
        fe->carousel.depth != depth || fe->carousel.tab != tab || fe->carousel.system != screen->system || fe->carousel.count != n) {
        fe->carousel.valid = 1;
        fe->carousel.presentation = (int)fe->model.presentation;
        fe->carousel.kind = (int)screen->kind;
        fe->carousel.depth = depth;
        fe->carousel.system = screen->system;
        fe->carousel.tab = tab;
        fe->carousel.count = n;
        fe->carousel.focus = focus;
        fe->carousel.from = 0;
        fe->carousel.start_ms = 0;
        return 0;
    }
    if (focus == fe->carousel.focus) return 0;
    fe->carousel.duration_ms = mainui_frontend_motion_ms(fe);
    if (jump || fe->carousel.duration_ms <= 0) {
        fe->carousel.focus = focus;
        fe->carousel.from = 0;
        fe->carousel.start_ms = 0;
        return 1;
    }
    // The row wraps; with two cards both neighbors are the same one, and the key tells the way.
    int step = ((focus - fe->carousel.focus) % n + n) % n;
    if (step * 2 > n || (step * 2 == n && fe->carousel.step < 0)) step -= n;
    fe->carousel.focus = focus;
    if (step != 1 && step != -1) {
        fe->carousel.from = 0;
        fe->carousel.start_ms = 0;
        return 1;
    }
    float from = mainui_frontend_carousel_shift(fe, now_ms) + (float)step;
    fe->carousel.from = from > 2 ? 2 : from < -2 ? -2 : from;
    fe->carousel.start_ms = now_ms ? now_ms : 1;
    return 1;
}

int mainui_frontend_background_system(struct mainui_frontend *fe) {
    const struct mainui_window *screen = mainui_model_top(&fe->model);
    for (int i = fe->model.depth - 1; i > 0 && (screen->kind == MAINUI_WIN_POPUP || screen->kind == MAINUI_WIN_DIALOG ||
                                                 screen->kind == MAINUI_WIN_NOTICE || screen->kind == MAINUI_WIN_PROGRESS ||
                                                 screen->kind == MAINUI_WIN_KEYBOARD); --i)
        screen = &fe->model.stack[i - 1];
    if (screen->kind == MAINUI_WIN_TABS) {
        int focus = fe->model.tabs[fe->model.tab].focus;
        if (fe->model.tab == MAINUI_TAB_GAMES) return focus >= 0 && focus < fe->visible_count ? focus : -1;
        if (fe->model.tab == MAINUI_TAB_BESTS)
            return focus >= 0 && focus < fe->shown_best_count ? MAINUI_BEST_SYSTEM_BASE + focus : -1;
        return -1;
    }
    if ((screen->kind == MAINUI_WIN_ROMS || screen->kind == MAINUI_WIN_SEARCH) && screen->system >= 0) return screen->system;
    return -1;
}

float mainui_frontend_background_fade(const struct mainui_frontend *fe, uint64_t now_ms) {
    if (!fe->system_background.valid) return 1;
    return mainui_frontend_motion_progress_over(fe->system_background.start_ms, now_ms, fe->system_background.duration_ms);
}

// BrickOS's system background follows the focus on the cards' clock: a new system starts its fade
// on the turn the cards start sliding. A fade cut short goes on from the picture that showed most.
static int system_background_follow(struct mainui_frontend *fe, uint64_t now_ms) {
    if (fe->model.presentation != MAINUI_PRESENTATION_MATERIAL_YOU) {
        fe->system_background.valid = 0;
        return 0;
    }
    int want = mainui_frontend_background_system(fe);
    int root = fe->model.stack[0].kind == MAINUI_WIN_HOME ? fe->model.home_focus : (int)fe->model.tab;
    if (!fe->system_background.valid || fe->system_background.root != root) {
        fe->system_background.valid = 1;
        fe->system_background.root = root;
        fe->system_background.shown = want;
        fe->system_background.from = -1;
        fe->system_background.start_ms = 0;
        return 0;
    }
    if (want == fe->system_background.shown) return 0;
    if (mainui_frontend_background_fade(fe, now_ms) >= 0.5f) fe->system_background.from = fe->system_background.shown;
    fe->system_background.shown = want;
    fe->system_background.duration_ms = mainui_frontend_motion_ms(fe);
    fe->system_background.start_ms = fe->system_background.duration_ms > 0 ? (now_ms ? now_ms : 1) : 0;
    return 1;
}

int mainui_frontend_motion(struct mainui_frontend *fe, uint64_t now_ms) {
    const struct mainui_window *screen = mainui_model_top(&fe->model);
    for (int i = fe->model.depth - 1; i > 0 && (screen->kind == MAINUI_WIN_POPUP || screen->kind == MAINUI_WIN_DIALOG ||
                                                 screen->kind == MAINUI_WIN_NOTICE || screen->kind == MAINUI_WIN_PROGRESS); --i)
        screen = &fe->model.stack[i - 1];
    int changed = carousel_follow(fe, screen, now_ms);
    changed |= system_background_follow(fe, now_ms);
    return backdrop_tick(fe, screen, now_ms) | changed;
}

// A clock still running at from_ms starts stall_ms later, so it goes on from where it stood then.
static void stall_clock(uint64_t *start_ms, int duration_ms, uint64_t from_ms, uint64_t stall_ms) {
    if (!*start_ms || duration_ms <= 0 || from_ms >= *start_ms + (uint64_t)duration_ms) return;
    *start_ms += stall_ms;
}

void mainui_frontend_motion_stall(struct mainui_frontend *fe, uint64_t from_ms, uint64_t stall_ms) {
    if (!stall_ms) return;
    if (fe->carousel.valid) stall_clock(&fe->carousel.start_ms, fe->carousel.duration_ms, from_ms, stall_ms);
    if (fe->system_background.valid)
        stall_clock(&fe->system_background.start_ms, fe->system_background.duration_ms, from_ms, stall_ms);
    stall_clock(&fe->backdrop.start_ms, MAINUI_MOTION_MS, from_ms, stall_ms);
}

// ---- Button Mapping -------------------------------------------------------------------------

#define MAPPING_DIR "/mnt/SDCARD/BrickOS/inputd"
#define MAPPING_PROFILE MAPPING_DIR "/inputd.json"
#define MAPPING_HEALTH "/tmp/brickos-inputd.health"
#define MAPPING_OFFICIAL "/usr/trimui/bin/trimui_inputd"
#define MAPPING_SETTLE_MS 500

// The key rows' profile sources, from MAINUI_SET_MAP_SELECT on, and each row's own key.
static const char *const mapping_sources[6] = {"SELECT", "START", "F1", "F2", "VOLUME_UP", "VOLUME_DOWN"};
static const int mapping_own[6] = {MAINUI_MAP_SELECT, MAINUI_MAP_START, MAINUI_MAP_F1, MAINUI_MAP_F2, MAINUI_MAP_VOLUP, MAINUI_MAP_VOLDOWN};
// What a destination writes: the alias of the code the named button sends by default. The
// printed A sends BTN_EAST, B BTN_SOUTH, X BTN_WEST and Y BTN_NORTH. Default writes nothing.
static const char *const mapping_aliases[MAINUI_MAP_DESTS] = {
    NULL, "BTN_EAST", "BTN_SOUTH", "BTN_WEST", "BTN_NORTH", "BTN_TL", "BTN_TR", "BTN_START", "BTN_SELECT",
    "BTN_MODE", "BTN_THUMBL", "BTN_THUMBR", "KEY_VOLUMEDOWN", "KEY_VOLUMEUP", "disabled",
};
// Xbox swaps the codes of A and B, and of the profile's X and Y (the printed Y and X).
static const char *const mapping_faces[4] = {"A", "B", "X", "Y"};
static const char *const mapping_xbox[4] = {"BTN_SOUTH", "BTN_EAST", "BTN_WEST", "BTN_NORTH"};

static int mapping_same(const char *alias, const char *other) {
    struct inputd_dest a, b;
    return inputd_config_resolve(alias, &a) == 0 && inputd_config_resolve(other, &b) == 0 &&
           a.type == b.type && a.code == b.code && a.value == b.value;
}

// A key row's value: the offered destination whose code the bound alias resolves to (BTN_B
// reads as A), or Custom.
static int mapping_value(const struct inputd_config_doc *doc, int row) {
    const char *alias = inputd_config_doc_get(doc, inputd_config_source(mapping_sources[row]));
    if (!alias) return MAINUI_MAP_DEFAULT;
    for (int d = MAINUI_MAP_DEFAULT + 1; d < MAINUI_MAP_DESTS; ++d)
        if (d != mapping_own[row] && mapping_same(alias, mapping_aliases[d])) return d;
    return MAINUI_MAP_CUSTOM;
}

// Nintendo without any face binding, Xbox with exactly the swap, else Custom.
static int mapping_layout(const struct inputd_config_doc *doc) {
    int bound = 0, xbox = 0;
    for (int i = 0; i < 4; ++i) {
        const char *alias = inputd_config_doc_get(doc, inputd_config_source(mapping_faces[i]));
        bound += alias != NULL;
        xbox += alias && mapping_same(alias, mapping_xbox[i]);
    }
    return !bound ? MAINUI_MAP_NINTENDO : xbox == 4 ? MAINUI_MAP_XBOX : MAINUI_MAP_CUSTOM;
}

// The BrickOS input daemon, from the health file only it writes: ready, alive, named
// trimui_inputd, and not the official executable, so neither a reused PID nor the official
// daemon, which a SIGHUP may stop, is ever signalled. Returns its pid, or -1.
static int mapping_daemon(struct mainui_frontend *fe) {
    char path[PATH_BYTES], text[1024], link[PATH_BYTES], *saved = NULL;
    path_under(fe, path, MAPPING_HEALTH);
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    size_t n = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[n] = 0;
    int ready = 0;
    long pid = -1;
    for (char *line = strtok_r(text, "\n", &saved); line; line = strtok_r(NULL, "\n", &saved)) {
        if (strcmp(line, "state=ready") == 0) ready = 1;
        else if (strncmp(line, "pid=", 4) == 0) pid = strtol(line + 4, NULL, 10);
    }
    if (!ready || pid <= 1 || pid > INT_MAX || kill((pid_t)pid, 0) != 0) return -1;
    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/%ld/comm", pid);
    path_under(fe, path, proc);
    file = fopen(path, "rb");
    if (!file) return -1;
    n = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[n] = 0;
    text[strcspn(text, "\n")] = 0;
    if (strcmp(text, "trimui_inputd") != 0) return -1;
    snprintf(proc, sizeof(proc), "/proc/%ld/exe", pid);
    path_under(fe, path, proc);
    ssize_t length = readlink(path, link, sizeof(link) - 1);
    if (length <= 0) return -1;
    link[length] = 0;
    return strcmp(link, MAPPING_OFFICIAL) == 0 ? -1 : (int)pid;
}

// Reads the active profile and the daemon's status into the model. An invalid file leaves the
// rows as they were, read-only; a change still settling keeps what the screen shows. Returns 1
// when something shown changed.
static int mapping_read(struct mainui_frontend *fe) {
    static struct inputd_config_doc doc;
    if (fe->mapping_pending) return 0;
    char path[PATH_BYTES], error[256];
    path_under(fe, path, MAPPING_PROFILE);
    int loaded = inputd_config_doc_load(&doc, path, error, sizeof(error));
    int *s = fe->model.settings, before[MAINUI_SET_MAP_VOLDOWN - MAINUI_SET_MAP_LAYOUT + 1];
    memcpy(before, s + MAINUI_SET_MAP_LAYOUT, sizeof(before));
    enum mainui_mapping_status status = MAINUI_MAPPING_INVALID;
    if (loaded >= 0) {
        s[MAINUI_SET_MAP_LAYOUT] = mapping_layout(&doc);
        for (int row = 0; row < 6; ++row) s[MAINUI_SET_MAP_SELECT + row] = mapping_value(&doc, row);
        status = mapping_daemon(fe) > 0 ? MAINUI_MAPPING_ACTIVE : MAINUI_MAPPING_INACTIVE;
    }
    int changed = status != fe->mapping_status || memcmp(before, s + MAINUI_SET_MAP_LAYOUT, sizeof(before)) != 0;
    if (status != fe->mapping_status)
        note(fe, "mapping status %s%s%s", status == MAINUI_MAPPING_ACTIVE ? "active" : status == MAINUI_MAPPING_INACTIVE ? "inactive" : "invalid",
             loaded < 0 ? ": " : "", loaded < 0 ? error : "");
    fe->mapping_status = status;
    fe->model.mapping_locked = loaded < 0;
    return changed;
}

static int write_synced(const char *directory, const char *path, const char *text, size_t length) {
    char temporary[PATH_BYTES + 16];
    snprintf(temporary, sizeof(temporary), "%s.brickos-tmp", path);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    int ok = 1;
    for (size_t done = 0; ok && done < length;) {
        ssize_t written = write(fd, text + done, length - done);
        if (written < 0 && errno == EINTR) continue;
        ok = written > 0;
        if (ok) done += (size_t)written;
    }
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    int folder = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folder >= 0) {
        fsync(folder);
        close(folder);
    }
    return 0;
}

// Writes the screen's values into the active profile; the other profiles and bindings stay,
// and Custom keeps its binding. Reset removes the ten sources, and replaces an invalid file
// with one profile without bindings. Returns 0, or -1 with the file as it was.
static int mapping_write(struct mainui_frontend *fe, int reset) {
    static struct inputd_config_doc doc;
    static char text[65536];
    char directory[PATH_BYTES], path[PATH_BYTES], error[256];
    path_under(fe, directory, MAPPING_DIR);
    path_under(fe, path, MAPPING_PROFILE);
    int loaded = inputd_config_doc_load(&doc, path, error, sizeof(error));
    if (loaded < 0 && !reset) {
        note(fe, "mapping kept, the profile is invalid: %s", error);
        return -1;
    }
    if (loaded <= 0) inputd_config_doc_new(&doc);
    const int *s = fe->model.settings;
    for (int row = 0; row < 6; ++row) {
        int source = inputd_config_source(mapping_sources[row]), v = s[MAINUI_SET_MAP_SELECT + row];
        if (reset || v == MAINUI_MAP_DEFAULT) inputd_config_doc_unset(&doc, source);
        else if (v > MAINUI_MAP_DEFAULT && v < MAINUI_MAP_DESTS) inputd_config_doc_set(&doc, source, mapping_aliases[v]);
    }
    for (int i = 0; i < 4; ++i) {
        int source = inputd_config_source(mapping_faces[i]), layout = s[MAINUI_SET_MAP_LAYOUT];
        if (reset || layout == MAINUI_MAP_NINTENDO) inputd_config_doc_unset(&doc, source);
        else if (layout == MAINUI_MAP_XBOX) inputd_config_doc_set(&doc, source, mapping_xbox[i]);
    }
    int length = inputd_config_doc_format(&doc, text, sizeof(text));
    note(fe, "mapping write%s", reset ? " reset" : "");
    if (length < 0) return -1;
    if (fe->dry) return 0;
    char parent[PATH_BYTES];
    path_under(fe, parent, "/mnt/SDCARD/BrickOS");
    mkdir(parent, 0755);
    mkdir(directory, 0755);
    return write_synced(directory, path, text, (size_t)length);
}

// Writes now, then signals the BrickOS daemon and reads the profile back. A failure shows its
// notice, and the rows return to what the file holds.
static void mapping_save(struct mainui_frontend *fe, int reset) {
    fe->mapping_pending = 0;
    fe->mapping_pending_since = 0;
    if (mapping_write(fe, reset) < 0) {
        ++fe->effects_failed;
        mainui_model_notice(&fe->model, MAINUI_NOTICE_MAPPING_SAVE_FAILED);
    } else if (fe->dry) {
        return;  // nothing was written to read back
    } else {
        int pid = mapping_daemon(fe);
        if (pid > 0 && kill((pid_t)pid, SIGHUP) == 0) note(fe, "mapping reload signalled to %d", pid);
    }
    mapping_read(fe);
}

// A change is written once it has settled; the profile is read again each second while
// Button Mapping is in front. Returns 1 when the screen changed.
static int mapping_tick(struct mainui_frontend *fe, uint64_t now_ms, int open) {
    int changed = 0;
    if (fe->mapping_pending) {
        if (!fe->mapping_pending_since) {
            fe->mapping_pending_since = now_ms ? now_ms : 1;
        } else if (now_ms - fe->mapping_pending_since >= MAPPING_SETTLE_MS) {
            mapping_save(fe, 0);
            changed = 1;
        }
    }
    if (open && !fe->dry && now_ms - fe->mapping_read_ms >= 1000) {
        fe->mapping_read_ms = now_ms;
        changed |= mapping_read(fe);
    }
    return changed;
}

// ---- BrickOS Appearance ----------------------------------------------------------------------

#define APPEARANCE_SETTLE_MS 500

// Reads the fonts folder again. A chosen font that is gone or no longer valid draws as Default;
// the file keeps its name until the next change is written. The open font list's focus goes to
// the font in use: row 0 is Default and row n the folder's font n - 1.
static void appearance_fonts(struct mainui_frontend *fe) {
    free(fe->fonts);
    fe->fonts = NULL;
    fe->font_count = mainui_appearance_fonts(&fe->fs, &fe->fonts);
    int row = 0;
    for (int i = 0; fe->appearance.font[0] && i < fe->font_count; ++i)
        if (strcmp(fe->fonts[i].file, fe->appearance.font) == 0) row = i + 1;
    if (fe->appearance.font[0] && !row) {
        note(fe, "appearance font %s unavailable: Default", fe->appearance.font);
        fe->appearance.font[0] = 0;
    }
    if (fe->model.depth <= 0) return;
    struct mainui_window *w = &fe->model.stack[fe->model.depth - 1];
    if (w->kind != MAINUI_WIN_SETTINGS || w->screen != MAINUI_SCREEN_APPEARANCE_FONT) return;
    w->focus = row;
    w->first = row > 5 ? row - 5 : 0;
}

// The choices at start: the file's, or the defaults, into the model's rows.
static void appearance_open(struct mainui_frontend *fe) {
    char path[PATH_BYTES];
    path_under(fe, path, MAINUI_APPEARANCE_PATH);
    int read = mainui_appearance_load(&fe->appearance, path);
    if (read < 0) note(fe, "appearance file invalid: defaults");
    for (int setting = MAINUI_SET_APPEAR_ACCENT; setting <= MAINUI_SET_APPEAR_LAST; ++setting)
        fe->model.settings[setting] = mainui_appearance_get(&fe->appearance, setting);
    appearance_fonts(fe);
    fe->appearance_pending = 0;
    fe->appearance_pending_since = 0;
}

// Writes the choices now, keeping the members the file holds that are not choices (another
// tool may have added some since the start). A failure keeps the choices and shows its notice;
// the next change tries again.
static void appearance_save(struct mainui_frontend *fe) {
    fe->appearance_pending = 0;
    fe->appearance_pending_since = 0;
    char directory[PATH_BYTES], parent[PATH_BYTES], path[PATH_BYTES];
    path_under(fe, directory, "/mnt/SDCARD/BrickOS/mainui/brickos");
    path_under(fe, path, MAINUI_APPEARANCE_PATH);
    static struct mainui_appearance disk;
    if (mainui_appearance_load(&disk, path) == 1) fe->appearance.document = disk.document;
    static char text[MAINUI_APPEARANCE_BYTES + 1];
    size_t length;
    int failed = mainui_appearance_format(&fe->appearance, text, sizeof(text), &length) < 0;
    if (failed) {
        // Members that no longer fit are dropped rather than losing the change.
        memset(&fe->appearance.document, 0, sizeof(fe->appearance.document));
        failed = mainui_appearance_format(&fe->appearance, text, sizeof(text), &length) < 0;
    }
    note(fe, "appearance write accent=%s text=%s font=%s", mainui_appearance_name(MAINUI_SET_APPEAR_ACCENT,
         mainui_appearance_get(&fe->appearance, MAINUI_SET_APPEAR_ACCENT)), mainui_appearance_name(MAINUI_SET_APPEAR_TEXT_SIZE,
         mainui_appearance_get(&fe->appearance, MAINUI_SET_APPEAR_TEXT_SIZE)), fe->appearance.font[0] ? fe->appearance.font : "Default");
    if (!failed && fe->dry) return;
    if (!failed) {
        path_under(fe, parent, "/mnt/SDCARD/BrickOS/mainui");
        mkdir(parent, 0755);
        mkdir(directory, 0755);
        failed = write_synced(directory, path, text, length) < 0;
    }
    if (failed) {
        ++fe->effects_failed;
        mainui_model_notice(&fe->model, MAINUI_NOTICE_APPEARANCE_SAVE_FAILED);
    } else {
        music_picture(fe); // a new accent or background gives the official OSD a new picture
    }
}

// A change is written once it has settled. Returns 1 when the screen changed.
static int appearance_tick(struct mainui_frontend *fe, uint64_t now_ms) {
    if (!fe->appearance_pending) return 0;
    if (!fe->appearance_pending_since) {
        fe->appearance_pending_since = now_ms ? now_ms : 1;
        return 0;
    }
    if (now_ms - fe->appearance_pending_since < APPEARANCE_SETTLE_MS) return 0;
    appearance_save(fe);
    return 1;
}

// ---- LED Policy -----------------------------------------------------------------------------

#define LED_DIR "/mnt/SDCARD/BrickOS/ledd"

// The LED daemon's mode for an option: Default and Follow Theme leave the LEDs to the firmware.
static enum ledd_mode led_mode(int option) {
    return option >= MAINUI_LED_BATTERY && option <= MAINUI_LED_CUSTOM ? (enum ledd_mode)(option - 1) : LEDD_MODE_DEFAULT;
}

// Reads the daemon's health: running when its file names a live brickos-ledd that has not
// stopped or failed; BrickTools when it yields to BrickTools' LED daemon.
static enum mainui_led_status led_health(struct mainui_frontend *fe) {
    char path[PATH_BYTES], text[1024], state[32] = "", reason[32] = "", *saved = NULL;
    path_under(fe, path, LEDD_HEALTH_PATH);
    FILE *file = fopen(path, "rb");
    if (!file) return MAINUI_LED_NOT_RUNNING;
    size_t n = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[n] = 0;
    long pid = -1;
    for (char *line = strtok_r(text, "\n", &saved); line; line = strtok_r(NULL, "\n", &saved)) {
        if (strncmp(line, "state=", 6) == 0) snprintf(state, sizeof(state), "%s", line + 6);
        else if (strncmp(line, "reason=", 7) == 0) snprintf(reason, sizeof(reason), "%s", line + 7);
        else if (strncmp(line, "pid=", 4) == 0) pid = strtol(line + 4, NULL, 10);
    }
    if (strcmp(state, LEDD_STATE_ACTIVE) != 0 && strcmp(state, LEDD_STATE_IDLE) != 0 &&
        strcmp(state, LEDD_STATE_YIELDING) != 0) return MAINUI_LED_NOT_RUNNING;
    if (pid <= 1 || pid > INT_MAX || kill((pid_t)pid, 0) != 0) return MAINUI_LED_NOT_RUNNING;
    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/%ld/comm", pid);
    path_under(fe, path, proc);
    file = fopen(path, "rb");
    if (!file) return MAINUI_LED_NOT_RUNNING;
    n = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[n] = 0;
    text[strcspn(text, "\n")] = 0;
    if (strcmp(text, LEDD_PROCESS_NAME) != 0) return MAINUI_LED_NOT_RUNNING;
    return strcmp(state, LEDD_STATE_YIELDING) == 0 && strcmp(reason, LEDD_REASON_BRICKTOOLS) == 0 ? MAINUI_LED_BRICKTOOLS
                                                                                                    : MAINUI_LED_RUNNING;
}

// LED Policy shows ledpolicy's Follow Theme whatever the configuration holds, otherwise the
// configured mode; a missing or invalid configuration shows the stock Default. The Custom rows
// show the configured values, or their defaults. Returns 1 when something shown changed.
static int led_read(struct mainui_frontend *fe) {
    struct ledd_config config;
    char path[PATH_BYTES], error[128];
    path_under(fe, path, LEDD_CONFIG_PATH);
    enum ledd_config_status loaded = ledd_config_load(&config, path, NULL, error, sizeof(error));
    if (loaded == LEDD_CONFIG_INVALID) note(fe, "led configuration invalid: %s", error);
    int *s = fe->model.settings;
    const int before[5] = {s[MAINUI_SET_LEDPOLICY], s[MAINUI_SET_LED_OPTION], s[MAINUI_SET_LED_EFFECT], s[MAINUI_SET_LED_COLOUR],
                           s[MAINUI_SET_LED_TIME]};
    int policy = keymon_or(fe, MAINUI_SHM_LEDPOLICY, fe->settings.values[MAINUI_SET_LEDPOLICY]) == 1;
    s[MAINUI_SET_LEDPOLICY] = policy;
    s[MAINUI_SET_LED_OPTION] = policy ? MAINUI_LED_FOLLOW_THEME :
                               config.mode == LEDD_MODE_DEFAULT ? MAINUI_LED_DEFAULT : (int)config.mode + 1;
    s[MAINUI_SET_LED_EFFECT] = (int)config.effect;
    s[MAINUI_SET_LED_COLOUR] = (int)config.colour;
    s[MAINUI_SET_LED_TIME] = config.time;
    enum mainui_led_status status = led_health(fe);
    const int after[5] = {s[MAINUI_SET_LEDPOLICY], s[MAINUI_SET_LED_OPTION], s[MAINUI_SET_LED_EFFECT], s[MAINUI_SET_LED_COLOUR],
                          s[MAINUI_SET_LED_TIME]};
    int changed = status != fe->led_status || memcmp(before, after, sizeof(before)) != 0;
    fe->led_status = status;
    return changed;
}

// Writes the rows' values to the configuration, keeping the members MainUI does not use.
// Returns 0, or -1 with the file as it was.
static int led_write(struct mainui_frontend *fe) {
    static struct mainui_json document;
    struct ledd_config config, ignored;
    char directory[PATH_BYTES], parent[PATH_BYTES], path[PATH_BYTES], text[LEDD_CONFIG_BYTES + 1], error[128];
    const int *s = fe->model.settings;
    path_under(fe, directory, LED_DIR);
    path_under(fe, path, LEDD_CONFIG_PATH);
    // Any JSON object's members are kept, even when its values are not valid.
    (void)ledd_config_load(&ignored, path, &document, error, sizeof(error));
    ledd_config_defaults(&config);
    config.mode = led_mode(s[MAINUI_SET_LED_OPTION]);
    if (s[MAINUI_SET_LED_EFFECT] >= 0 && s[MAINUI_SET_LED_EFFECT] < LEDD_EFFECT_COUNT) config.effect = (enum ledd_effect)s[MAINUI_SET_LED_EFFECT];
    if (s[MAINUI_SET_LED_COLOUR] >= 0 && s[MAINUI_SET_LED_COLOUR] < LEDD_COLOUR_COUNT) config.colour = (enum ledd_colour)s[MAINUI_SET_LED_COLOUR];
    if (s[MAINUI_SET_LED_TIME] >= LEDD_TIME_MIN && s[MAINUI_SET_LED_TIME] <= LEDD_TIME_MAX) config.time = s[MAINUI_SET_LED_TIME];
    size_t length;
    // Members that no longer fit are dropped rather than losing the change.
    if (ledd_config_format(&config, &document, text, sizeof(text), &length) < 0 &&
        ledd_config_format(&config, NULL, text, sizeof(text), &length) < 0) return -1;
    note(fe, "led write mode=%s effect=%s colour=%s time=%d", ledd_mode_name(config.mode), ledd_effect_name(config.effect),
         ledd_colour_name(config.colour), config.time);
    if (fe->dry) return 0;
    path_under(fe, parent, "/mnt/SDCARD/BrickOS");
    mkdir(parent, 0755);
    mkdir(directory, 0755);
    return write_synced(directory, path, text, length);
}

// A change of LED Policy writes ledpolicy through the stock path (system.json and keymon's
// index 19) and the mode to the configuration; a Custom row writes the configuration.
static void led_set(struct mainui_frontend *fe, const struct mainui_effect *e) {
    note(fe, "set led setting%d=%d", e->setting, e->value);
    if (e->setting == MAINUI_SET_LED_OPTION) {
        struct mainui_effect policy = *e;
        policy.setting = MAINUI_SET_LEDPOLICY;
        policy.value = e->value == MAINUI_LED_FOLLOW_THEME;
        carry_out(fe, &policy);
    }
    if (led_write(fe) < 0) {
        note(fe, "led configuration not written");
        ++fe->effects_failed;
    }
    if (!fe->dry) fe->led_status = led_health(fe);
}

// The health is read again each second while Display is in front. Returns 1 when the row's
// status changed.
static int led_tick(struct mainui_frontend *fe, uint64_t now_ms, int open) {
    if (!open || fe->dry || now_ms - fe->led_read_ms < 1000) return 0;
    fe->led_read_ms = now_ms;
    enum mainui_led_status status = led_health(fe);
    int changed = status != fe->led_status;
    fe->led_status = status;
    return changed;
}

// Settings > GameSwitcher's switch, read each second: turning GameSwitcher off or on hides or
// shows its app and desktop card at once. Returns 1 when it changed.
static int gameswitcher_tick(struct mainui_frontend *fe, uint64_t now_ms) {
    if (now_ms - fe->gameswitcher_read_ms < 1000) return 0;
    fe->gameswitcher_read_ms = now_ms;
    int off = gameswitcher_off(fe);
    if (off == fe->gameswitcher_off) return 0;
    fe->gameswitcher_off = off;
    show_apps(fe);
    note(fe, "gameswitcher %s", off ? "off" : "on");
    mainui_model_catalog_changed(&fe->model);
    return 1;
}

int mainui_frontend_tick(struct mainui_frontend *fe, uint64_t now_ms) {
    if (fe->rumble_running && __atomic_load_n(&fe->rumble_done, __ATOMIC_ACQUIRE)) rumble_finish(fe, 1);
    int powered = power_tick(fe, now_ms);
    int refreshed = refresh_tick(fe);
    const struct mainui_window *top = mainui_model_top(&fe->model);
    // A notice closes by itself after 5 s (d1; static: NotifyWindow's 5000 ms timer). The timer
    // starts again with the next notice, however the last one closed.
    int notice = 0;
    if (top->kind == MAINUI_WIN_NOTICE) {
        if (!fe->notice_ms) fe->notice_ms = now_ms ? now_ms : 1;
        else if (now_ms - fe->notice_ms >= NOTICE_MS) notice = mainui_model_event(&fe->model, MAINUI_EVENT_NOTICE_DONE);
        top = mainui_model_top(&fe->model);
    }
    if (top->kind != MAINUI_WIN_NOTICE) fe->notice_ms = 0;
    // A popup, dialog, or notice leaves the screen under it open (d1: the Bluetooth screen stays
    // under Remove); a progress wait is handled by the Bluetooth adapter itself.
    const struct mainui_window *screen = top;
    for (int i = fe->model.depth - 1; i > 0 && (screen->kind == MAINUI_WIN_POPUP || screen->kind == MAINUI_WIN_DIALOG ||
                                                 screen->kind == MAINUI_WIN_NOTICE); --i)
        screen = &fe->model.stack[i - 1];
    int settings = screen->kind == MAINUI_WIN_SETTINGS;
    int changed = status_tick(fe, now_ms);
    changed |= netplay_tick(fe, now_ms);
    changed |= wifi_job_tick(fe, now_ms);
    int wifi_open = 0;
    for (int i = 0; i < fe->model.depth; ++i)
        wifi_open |= fe->model.stack[i].kind == MAINUI_WIN_SETTINGS && fe->model.stack[i].screen == MAINUI_SCREEN_WIFI;
    changed |= wifi_tick(fe, now_ms, wifi_open, top == screen && settings && screen->screen == MAINUI_SCREEN_WIFI);
    changed |= bt_tick(fe, now_ms, settings && screen->screen == MAINUI_SCREEN_BLUETOOTH, top);
    changed |= mapping_tick(fe, now_ms, settings && screen->screen == MAINUI_SCREEN_BUTTON_MAPPING);
    changed |= led_tick(fe, now_ms, settings && screen->screen == MAINUI_SCREEN_DISPLAY);
    changed |= gameswitcher_tick(fe, now_ms);
    changed |= appearance_tick(fe, now_ms);
    changed |= __atomic_exchange_n(&fe->counts_changed, 0, __ATOMIC_ACQ_REL);
    changed |= boot_rotation_tick(fe);
    changed |= music_tick(fe);
    fe->ext_now = now_ms;
    changed |= mainui_ext_runner_poll(&fe->ext_runner, now_ms);
    changed |= ext_settle(fe, now_ms, 0);
    changed |= fe->ext_changed;
    fe->ext_changed = 0;
    carry_out_all(fe);
    if (fe->model.presentation == MAINUI_PRESENTATION_MATERIAL_YOU && fe->brickos_view_touched) {
        fe->brickos_view_deadline_ms = now_ms + 750;
        fe->brickos_view_touched = 0;
    }
    if (fe->model.presentation == MAINUI_PRESENTATION_MATERIAL_YOU && !fe->dry &&
        fe->brickos_view_deadline_ms && now_ms >= fe->brickos_view_deadline_ms) {
        if (save_brickos_last_view(fe) < 0) note(fe, "brickos last view publication refused");
        fe->brickos_view_deadline_ms = 0;
    }
    changed |= mainui_frontend_motion(fe, now_ms);
    return changed | notice | refreshed | powered;
}

static void persist_settings(struct mainui_frontend *fe) {
    note(fe, "persist");
    if (fe->dry) return;
    char path[PATH_BYTES];
    path_under(fe, path, "/mnt/UDISK/system.json");
    if (mainui_settings_save(&fe->settings, path) < 0) ++fe->effects_failed;
}

static int boot_visual_kind(enum mainui_list list, enum mainui_boot_visual_kind *kind) {
    if (list == MAINUI_LIST_BOOTLOGOS) { *kind = MAINUI_BOOTLOGO; return 1; }
    if (list == MAINUI_LIST_SPLASHSCREENS) { *kind = MAINUI_SPLASHSCREEN; return 1; }
    return 0;
}

static void boot_visuals_lock(struct mainui_frontend *fe) {
    if (fe->boot_visuals.lock_ready) pthread_mutex_lock(&fe->boot_visuals.lock);
}

static void boot_visuals_unlock(struct mainui_frontend *fe) {
    if (fe->boot_visuals.lock_ready) pthread_mutex_unlock(&fe->boot_visuals.lock);
}

// The applied image of a catalog, found by content. A dry run on the device does not mount the
// boot partition to read it.
static int boot_visual_active(const struct mainui_frontend *fe, enum mainui_boot_visual_kind kind,
                              const struct mainui_boot_visual *items, int count) {
    return fe->dry && !fe->fs.root[0] && kind == MAINUI_BOOTLOGO ? -2 :
           mainui_boot_visuals_current(fe->fs.root, kind, items, count);
}

static uint32_t boot_visual_random(struct mainui_frontend *fe) {
    if (fe->boot_visuals.random_value) return fe->boot_visuals.random_value(fe->boot_visuals.random_context);
    uint32_t value = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, &value, sizeof(value));
        close(fd);
        if (n == (ssize_t)sizeof(value)) return value;
    }
    return (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16);
}

// Holds the lock. The open selector's focus goes to the ticked row: Random (row 0) in random
// mode, else the applied image (row n is catalog index n - 1), else the top.
static void refresh_boot_visual_locked(struct mainui_frontend *fe, enum mainui_boot_visual_kind kind) {
    struct mainui_boot_visual **catalog = kind == MAINUI_BOOTLOGO ? &fe->bootlogos : &fe->splashscreens;
    int *catalog_count = kind == MAINUI_BOOTLOGO ? &fe->bootlogo_count : &fe->splashscreen_count;
    struct mainui_boot_visual *updated = NULL;
    int updated_count = mainui_boot_visuals_catalog(fe->fs.root, kind, &updated);
    if (updated_count >= 0) {
        free(*catalog);
        *catalog = updated;
        *catalog_count = updated_count;
    }
    struct mainui_boot_visual *items = *catalog;
    int count = *catalog_count;
    int current = boot_visual_active(fe, kind, items, count);
    if (kind == MAINUI_BOOTLOGO) fe->bootlogo_active = current;
    else fe->splashscreen_active = current;
    if (fe->model.depth <= 0) return;
    struct mainui_window *w = &fe->model.stack[fe->model.depth - 1];
    enum mainui_screen screen = kind == MAINUI_BOOTLOGO ? MAINUI_SCREEN_BOOTLOGO : MAINUI_SCREEN_SPLASHSCREEN;
    if (w->kind != MAINUI_WIN_SETTINGS || w->screen != screen) return;
    int row = 0;
    if (!fe->boot_visuals.random[kind] && current >= 0) {
        for (int i = 0; i < count; ++i) if (items[i].index == current) { row = i + 1; break; }
    }
    w->focus = row;
    w->first = row > 5 ? row - 5 : 0;
}

static void refresh_boot_visual(struct mainui_frontend *fe, enum mainui_list list) {
    enum mainui_boot_visual_kind kind;
    if (!boot_visual_kind(list, &kind)) return;
    boot_visuals_lock(fe);
    refresh_boot_visual_locked(fe, kind);
    boot_visuals_unlock(fe);
}

// Confirming an image writes it and makes the selector fixed; confirming Random writes an
// image picked at random, other than the applied one, and makes it random. The mode is stored
// only once the image is written. One image that is already applied needs no write.
static void apply_boot_visual(struct mainui_frontend *fe, const struct mainui_effect *e) {
    enum mainui_boot_visual_kind kind;
    if (!boot_visual_kind((enum mainui_list)e->list, &kind)) { ++fe->effects_failed; return; }
    boot_visuals_lock(fe);
    struct mainui_boot_visual *items = kind == MAINUI_BOOTLOGO ? fe->bootlogos : fe->splashscreens;
    int count = kind == MAINUI_BOOTLOGO ? fe->bootlogo_count : fe->splashscreen_count;
    int old_active = kind == MAINUI_BOOTLOGO ? fe->bootlogo_active : fe->splashscreen_active;
    int random = e->index == MAINUI_BOOT_VISUAL_RANDOM;
    if ((random && count <= 0) || (!random && (e->index < 0 || e->index >= count))) {
        boot_visuals_unlock(fe);
        ++fe->effects_failed;
        return;
    }
    int selected = random ? mainui_boot_visuals_pick(items, count, boot_visual_active(fe, kind, items, count),
                                                     boot_visual_random(fe))
                          : items[e->index].index;
    note(fe, "boot visual apply target=%s mode=%s index=%d file=%s", kind == MAINUI_BOOTLOGO ? "bootlogo" : "splashscreen",
         random ? "random" : "fixed", selected, selected >= 0 ? items[selected].name : "(applied)");
    if (fe->dry) {
        boot_visuals_unlock(fe);
        return;
    }
    int applied = selected < 0 || mainui_boot_visuals_apply(fe->fs.root, kind, items, count, selected) == 0;
    int stored = applied;
    if (applied && fe->boot_visuals.random[kind] != random) {
        int modes[2] = {fe->boot_visuals.random[0], fe->boot_visuals.random[1]};
        modes[kind] = random;
        stored = mainui_boot_visuals_modes_save(fe->fs.root, modes) == 0;
        if (stored) fe->boot_visuals.random[kind] = random;
        else note(fe, "boot visual modes not saved");
    }
    refresh_boot_visual_locked(fe, kind);
    boot_visuals_unlock(fe);
    if (!applied || !stored) {
        ++fe->effects_failed;
        // The adapter leaves the destination intact unless its verified rename succeeds.
        if (!applied) {
            if (kind == MAINUI_BOOTLOGO) fe->bootlogo_active = old_active;
            else fe->splashscreen_active = old_active;
        }
        mainui_model_notice(&fe->model, MAINUI_NOTICE_BOOT_VISUAL_FAILED);
    } else {
        mainui_model_notice(&fe->model, MAINUI_NOTICE_BOOT_VISUAL_APPLIED);
    }
    fe->notice_ms = 0;
}

#define BOOT_ROTATION_MARKER "/tmp/brickos-boot-visuals.rotated"

// The new image of one random selector at boot, read from the folder as it is now. A failure
// keeps the applied image and is only logged; the next boot tries again.
static void rotate_boot_visual(struct mainui_frontend *fe, enum mainui_boot_visual_kind kind, uint32_t random,
                               int write) {
    const char *target = kind == MAINUI_BOOTLOGO ? "bootlogo" : "splashscreen";
    struct mainui_boot_visual *items = NULL;
    int count = mainui_boot_visuals_catalog(fe->fs.root, kind, &items);
    if (count < 0) {
        note(fe, "boot rotation target=%s catalog unreadable", target);
        return;
    }
    int pick = mainui_boot_visuals_pick(items, count, boot_visual_active(fe, kind, items, count), random);
    if (pick < 0) note(fe, "boot rotation target=%s nothing to write images=%d", target, count);
    else if (!write) note(fe, "boot rotation target=%s file=%s (dry)", target, items[pick].name);
    else if (mainui_boot_visuals_apply(fe->fs.root, kind, items, count, pick) == 0)
        note(fe, "boot rotation target=%s file=%s", target, items[pick].name);
    else note(fe, "boot rotation target=%s file=%s failed; image kept", target, items[pick].name);
    free(items);
}

static void *boot_rotation_worker(void *context) {
    struct mainui_frontend *fe = context;
    for (int kind = MAINUI_BOOTLOGO; kind <= MAINUI_SPLASHSCREEN; ++kind) {
        if (!fe->boot_visuals.random[kind]) continue;
        boot_visuals_lock(fe);
        rotate_boot_visual(fe, (enum mainui_boot_visual_kind)kind, fe->boot_visuals.draws[kind], 1);
        boot_visuals_unlock(fe);
    }
    __atomic_store_n(&fe->boot_visuals.changed, 1, __ATOMIC_RELEASE);
    return NULL;
}

int mainui_frontend_boot_rotation(struct mainui_frontend *fe) {
    if (!fe || fe->boot_visuals.worker_running) return 0;
    if (fe->dry) {
        for (int kind = MAINUI_BOOTLOGO; kind <= MAINUI_SPLASHSCREEN; ++kind)
            if (fe->boot_visuals.random[kind])
                rotate_boot_visual(fe, (enum mainui_boot_visual_kind)kind, boot_visual_random(fe), 0);
        return 0;
    }
    char marker[PATH_BYTES];
    path_under(fe, marker, BOOT_ROTATION_MARKER);
    int fd = marker[0] ? open(marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644) : -1;
    if (fd < 0) {
        if (errno != EEXIST) note(fe, "boot rotation marker not created");
        return 0;
    }
    close(fd);
    if (!fe->boot_visuals.random[MAINUI_BOOTLOGO] && !fe->boot_visuals.random[MAINUI_SPLASHSCREEN]) return 0;
    for (int kind = MAINUI_BOOTLOGO; kind <= MAINUI_SPLASHSCREEN; ++kind) fe->boot_visuals.draws[kind] = boot_visual_random(fe);
    __atomic_store_n(&fe->boot_visuals.changed, 0, __ATOMIC_RELEASE);
    if (pthread_create(&fe->boot_visuals.worker, NULL, boot_rotation_worker, fe) != 0) {
        note(fe, "boot rotation worker not started");
        return 0;
    }
    fe->boot_visuals.worker_running = 1;
    return 1;
}

// After the boot rotation, Settings shows the images it applied; the focus stays where it is.
static int boot_rotation_tick(struct mainui_frontend *fe) {
    if (!__atomic_exchange_n(&fe->boot_visuals.changed, 0, __ATOMIC_ACQ_REL)) return 0;
    const struct mainui_window *w = mainui_model_top(&fe->model);
    if (w->kind != MAINUI_WIN_SETTINGS || (w->screen != MAINUI_SCREEN_THEMES && w->screen != MAINUI_SCREEN_BOOTLOGO &&
                                           w->screen != MAINUI_SCREEN_SPLASHSCREEN)) return 0;
    boot_visuals_lock(fe);
    fe->bootlogo_active = boot_visual_active(fe, MAINUI_BOOTLOGO, fe->bootlogos, fe->bootlogo_count);
    fe->splashscreen_active = boot_visual_active(fe, MAINUI_SPLASHSCREEN, fe->splashscreens, fe->splashscreen_count);
    boot_visuals_unlock(fe);
    return 1;
}

// Rebuilds the preset list, finds the installed set, and puts the open selector's focus on it:
// row 0 is TRIMUI and row n preset n - 1.
static void refresh_loading_screens(struct mainui_frontend *fe) {
    struct mainui_loading_screen *updated = NULL;
    int updated_count = mainui_loading_screens_catalog(fe->fs.root, &updated);
    if (updated_count >= 0) {
        free(fe->loading_screens);
        fe->loading_screens = updated;
        fe->loading_screen_count = updated_count;
    }
    fe->loading_screen_active = mainui_loading_screens_current(fe->fs.root, fe->loading_screens, fe->loading_screen_count);
    if (fe->model.depth <= 0) return;
    struct mainui_window *w = &fe->model.stack[fe->model.depth - 1];
    if (w->kind != MAINUI_WIN_SETTINGS || w->screen != MAINUI_SCREEN_LOADING_SCREEN) return;
    int row = 0;
    for (int i = 0; fe->loading_screen_active >= 0 && i < fe->loading_screen_count; ++i)
        if (fe->loading_screens[i].index == fe->loading_screen_active) { row = i + 1; break; }
    w->focus = row;
    w->first = row > 5 ? row - 5 : 0;
}

static void apply_loading_screen(struct mainui_frontend *fe, const struct mainui_effect *e) {
    if (e->index < 0 || e->index > fe->loading_screen_count) { ++fe->effects_failed; return; }
    const struct mainui_loading_screen *preset = e->index ? &fe->loading_screens[e->index - 1] : NULL;
    note(fe, "loading screen apply entry=%d preset=%s", e->index, preset ? preset->name : "TRIMUI");
    if (fe->dry) return;
    int old_active = fe->loading_screen_active;
    int applied = (preset ? mainui_loading_screens_apply(fe->fs.root, fe->loading_screens, fe->loading_screen_count,
                                                        preset->index)
                          : mainui_loading_screens_restore_default(fe->fs.root)) == 0;
    refresh_loading_screens(fe);
    if (!applied) {
        ++fe->effects_failed;
        // The module leaves the installed images as they were unless the whole set went in.
        fe->loading_screen_active = old_active;
        mainui_model_notice(&fe->model, MAINUI_NOTICE_LOADING_SCREEN_FAILED);
    } else {
        mainui_model_notice(&fe->model, MAINUI_NOTICE_LOADING_SCREEN_APPLIED);
    }
    fe->notice_ms = 0;
}

// ---- Settings extensions -------------------------------------------------------------------

// A select or range runs its execute once its value has stayed for this long.
#define EXT_SETTLE_MS 500

static struct mainui_ext_node *ext_node(struct mainui_frontend *fe, int node) {
    return node >= 0 && node < fe->ext.node_count ? &fe->ext.nodes[node] : NULL;
}

static int ext_tiles(void *context) {
    return mainui_ext_tile_count(&((struct mainui_frontend *)context)->ext);
}

static int ext_tile_node(void *context, int tile) {
    return mainui_ext_tile_node(&((struct mainui_frontend *)context)->ext, tile);
}

static int ext_children(void *context, int node, int *nodes, int max) {
    const struct mainui_ext_registry *r = &((struct mainui_frontend *)context)->ext;
    const struct mainui_ext_node *n = mainui_ext_node(r, node);
    int count = 0;
    for (int c = n ? n->first_child : -1; c >= 0; c = r->nodes[c].next_sibling) {
        if (count < max) nodes[count] = c;
        ++count;
    }
    return count;
}

static int ext_section_rows(void *context, int screen, struct mainui_extension_row *rows, int max) {
    const struct mainui_ext_registry *r = &((struct mainui_frontend *)context)->ext;
    int count = 0;
    for (int i = 0; i < r->node_count; ++i) {
        const struct mainui_ext_node *n = &r->nodes[i];
        if (n->section != screen || n->parent >= 0) continue;
        if (count < max) {
            rows[count].node = i;
            rows[count].anchor = n->anchor == MAINUI_EXT_ANCHOR_AFTER ? MAINUI_EXTENSION_AFTER
                               : n->anchor == MAINUI_EXT_ANCHOR_BEFORE ? MAINUI_EXTENSION_BEFORE : MAINUI_EXTENSION_END;
            rows[count].anchor_id = n->anchor_id;
        }
        ++count;
    }
    return count;
}

static int ext_info(void *context, int node, struct mainui_extension_info *info) {
    const struct mainui_ext_node *n = mainui_ext_node(&((struct mainui_frontend *)context)->ext, node);
    if (!n) return -1;
    static const enum mainui_extension_kind kinds[] = {
        [MAINUI_EXT_MENU] = MAINUI_EXTENSION_MENU, [MAINUI_EXT_CMD] = MAINUI_EXTENSION_CMD,
        [MAINUI_EXT_TOGGLE] = MAINUI_EXTENSION_TOGGLE, [MAINUI_EXT_SELECT] = MAINUI_EXTENSION_SELECT,
        [MAINUI_EXT_RANGE] = MAINUI_EXTENSION_RANGE, [MAINUI_EXT_INFO] = MAINUI_EXTENSION_INFO,
    };
    info->kind = kinds[n->kind];
    info->label = n->label ? n->label : "";
    info->value = n->value;
    info->min = n->min;
    info->max = n->max;
    info->step = n->step;
    info->item_count = n->item_count;
    info->output = n->output == MAINUI_EXT_OUTPUT_FULL ? MAINUI_EXTENSION_OUTPUT_FULL
                 : n->output == MAINUI_EXT_OUTPUT_LINE ? MAINUI_EXTENSION_OUTPUT_LINE : MAINUI_EXTENSION_OUTPUT_NONE;
    info->usable = !n->disabled && !n->busy;
    return 0;
}

// "<file> <row>" for the log and the diagnostic record: the row's id, else its index path, else
// its label.
static void ext_what(struct mainui_frontend *fe, int node, char *out, size_t size) {
    const struct mainui_ext_node *n = ext_node(fe, node);
    if (!n) { snprintf(out, size, "?"); return; }
    const struct mainui_ext_extension *e = &fe->ext.extensions[n->extension];
    const char *row = n->id ? n->id : n->path[0] ? n->path : n->label ? n->label : "";
    int n_written = e->app ? snprintf(out, size, "Apps/%.200s/settings.json %.40s", e->name, row)
                           : snprintf(out, size, "%.100s/%.100s.json %.40s", e->name, e->name, row);
    if (n_written < 0 && size) out[0] = 0;
}

static int ext_start(struct mainui_frontend *fe, int node, enum mainui_run_kind kind, const char *command, const char *argument) {
    char what[256];
    ext_what(fe, node, what, sizeof(what));
    int id = mainui_ext_runner_start(&fe->ext_runner, kind, node, fe->ext.generation, mainui_ext_node_dir(&fe->ext, node),
                                     command, argument, what, fe->ext_now);
    if (id < 0) {
        mainui_ext_log(&fe->ext, "%s: not run, too many scripts waiting", what);
        ++fe->effects_failed;
        return -1;
    }
    if (id > 0) {
        fe->ext.nodes[node].busy = 1;
        fe->ext.nodes[node].job = id;
    }
    return id;
}

// The loads of a screen's extension rows: an extension menu's, or those a built-in section got.
static void ext_run_loads(struct mainui_frontend *fe, int screen, int menu) {
    static int nodes[MAINUI_EXT_MAX_ITEMS];
    static struct mainui_extension_row rows[MAINUI_EXTENSION_SECTION_ROWS];
    int count;
    if (screen == MAINUI_SCREEN_EXTENSION) {
        count = ext_children(fe, menu, nodes, MAINUI_EXT_MAX_ITEMS);
    } else {
        count = ext_section_rows(fe, screen, rows, MAINUI_EXTENSION_SECTION_ROWS);
        if (count > MAINUI_EXTENSION_SECTION_ROWS) count = MAINUI_EXTENSION_SECTION_ROWS;
        for (int i = 0; i < count; ++i) nodes[i] = rows[i].node;
    }
    if (count > MAINUI_EXT_MAX_ITEMS) count = MAINUI_EXT_MAX_ITEMS;
    for (int i = 0; i < count; ++i) {
        const struct mainui_ext_node *n = ext_node(fe, nodes[i]);
        if (n && n->load && !n->job) ext_start(fe, nodes[i], MAINUI_RUN_LOAD, n->load, NULL);
    }
}

// After an execute or a language change, the screen in front reads its rows again, as
// BrickTools does after an execute.
static void ext_reload_screen(struct mainui_frontend *fe) {
    for (int i = fe->model.depth - 1; i >= 0; --i) {
        const struct mainui_window *w = &fe->model.stack[i];
        if (w->kind != MAINUI_WIN_SETTINGS) continue;
        if (w->screen == MAINUI_SCREEN_EXTENSION) ext_run_loads(fe, MAINUI_SCREEN_EXTENSION, w->index);
        else if (mainui_model_extension_section_id(w->screen)) ext_run_loads(fe, w->screen, -1);
        return;
    }
}

static void ext_view_append(struct mainui_frontend *fe, const char *text) {
    if (!fe->ext_view && !(fe->ext_view = malloc(MAINUI_EXT_RUNNER_TAIL))) return;
    size_t n = strlen(text);
    if (fe->ext_view_length + n + 1 > MAINUI_EXT_RUNNER_TAIL) n = MAINUI_EXT_RUNNER_TAIL - 1 - fe->ext_view_length;
    memcpy(fe->ext_view + fe->ext_view_length, text, n);
    fe->ext_view_length += n;
    fe->ext_view[fe->ext_view_length] = 0;
    fe->ext_view_lines = 0;
    for (size_t i = 0; i < fe->ext_view_length; ++i) fe->ext_view_lines += fe->ext_view[i] == '\n';
    if (fe->ext_view_length && fe->ext_view[fe->ext_view_length - 1] != '\n') ++fe->ext_view_lines;
}

static void ext_view_start(struct mainui_frontend *fe, int node, int job, int status) {
    fe->ext_view_node = node;
    fe->ext_view_job = job;
    fe->ext_view_status = status;
    fe->ext_view_length = 0;
    fe->ext_view_lines = 0;
    if (fe->ext_view) fe->ext_view[0] = 0;
}

static void ext_run_command(struct mainui_frontend *fe, int node) {
    const struct mainui_ext_node *n = ext_node(fe, node);
    fe->ext_line[0] = 0;
    int full = n && n->output == MAINUI_EXT_OUTPUT_FULL;
    int id = n && n->execute ? ext_start(fe, node, MAINUI_RUN_CMD, n->execute, NULL) : -1;
    if (full) ext_view_start(fe, node, id > 0 ? id : 0, id > 0 ? -1 : 0);
    if (id > 0) return;
    // Nothing runs (a dry run, a row without execute): its views close at once.
    mainui_model_extension_close(&fe->model, node);
    mainui_model_extension_close_view(&fe->model, node);
}

static void ext_apply_value(struct mainui_frontend *fe, int node) {
    struct mainui_ext_node *n = ext_node(fe, node);
    if (!n) return;
    n->pending = 0;
    n->pending_since = 0;
    if (!n->execute) { n->applied = n->value; return; }
    char argument[16];
    snprintf(argument, sizeof(argument), "%d", n->value);
    int id = ext_start(fe, node, MAINUI_RUN_VALUE, n->execute, argument);
    // A dry run keeps the value, which nothing can refuse.
    if (id == 0) n->applied = n->value;
}

static void ext_change(struct mainui_frontend *fe, int node, int value, int now) {
    struct mainui_ext_node *n = ext_node(fe, node);
    if (!n) return;
    mainui_ext_show_value(&fe->ext, node, value);
    if (now) {
        ext_apply_value(fe, node);
    } else {
        // The settle time starts again at the next tick.
        n->pending = 1;
        n->pending_since = 0;
    }
}

// Values whose time has come: settled, or all of them when a screen is left. A row whose
// script still runs keeps its newest value for after it.
static int ext_settle(struct mainui_frontend *fe, uint64_t now_ms, int all) {
    int started = 0;
    for (int i = 0; i < fe->ext.node_count; ++i) {
        struct mainui_ext_node *n = &fe->ext.nodes[i];
        if (!n->pending || n->job) continue;
        if (!all && !n->pending_since) { n->pending_since = now_ms ? now_ms : 1; continue; }
        if (all || now_ms - n->pending_since >= EXT_SETTLE_MS) {
            ext_apply_value(fe, i);
            started = 1;
        }
    }
    return started;
}

static void ext_cancel(struct mainui_frontend *fe, int node) {
    const struct mainui_ext_node *n = ext_node(fe, node);
    if (n && n->job) mainui_ext_runner_cancel(&fe->ext_runner, n->job, fe->ext_now);
}

static void ext_answer(struct mainui_frontend *fe, int node, int answer) {
    const struct mainui_ext_node *n = ext_node(fe, node);
    if (n && n->job && mainui_ext_runner_answer(&fe->ext_runner, n->job, (char)answer) < 0)
        note(fe, "extension answer not delivered");
    fe->ext_confirm[0] = 0;
}

static void ext_on_line(void *context, struct mainui_ext_job *job, const char *line) {
    struct mainui_frontend *fe = context;
    if (job->generation != fe->ext.generation || !ext_node(fe, job->node)) return;
    // A load's tags count only when it ends well: keep its lines until then.
    if (job->kind == MAINUI_RUN_LOAD) {
        mainui_ext_runner_keep(job, line);
        mainui_ext_runner_keep(job, "\n");
        return;
    }
    char visible[MAINUI_EXT_RUNNER_LINE * 2];
    switch (mainui_ext_apply_line(&fe->ext, job->node, line, visible, sizeof(visible))) {
    case MAINUI_EXT_LINE_HIDDEN:
        break;
    case MAINUI_EXT_LINE_CONFIRM:
        if (snprintf(fe->ext_confirm, sizeof(fe->ext_confirm), "%s", visible) >= (int)sizeof(fe->ext_confirm))
            fe->ext_confirm[sizeof(fe->ext_confirm) - 1] = 0;
        mainui_model_extension_confirm(&fe->model, job->node);
        break;
    case MAINUI_EXT_LINE_VISIBLE: {
        mainui_ext_runner_keep(job, visible);
        mainui_ext_runner_keep(job, "\n");
        const char *last = strrchr(visible, '\n');
        if (snprintf(fe->ext_line, sizeof(fe->ext_line), "%s", last ? last + 1 : visible) >= (int)sizeof(fe->ext_line))
            fe->ext_line[sizeof(fe->ext_line) - 1] = 0;
        if (fe->ext_view_job == job->id) {
            ext_view_append(fe, visible);
            ext_view_append(fe, "\n");
            mainui_model_text_lines(&fe->model, fe->ext_view_lines);
        }
        break;
    }
    }
    fe->ext_changed = 1;
}

// The last line of a job's visible output.
static void ext_last_line(const struct mainui_ext_job *job, char *out, size_t size) {
    out[0] = 0;
    if (!job->tail || !job->tail_length) return;
    size_t end = job->tail_length;
    while (end > 0 && job->tail[end - 1] == '\n') --end;
    size_t start = end;
    while (start > 0 && job->tail[start - 1] != '\n') --start;
    snprintf(out, size, "%.*s", (int)(end - start), job->tail + start);
}

static void ext_on_exit(void *context, struct mainui_ext_job *job) {
    struct mainui_frontend *fe = context;
    const char *kind = job->kind == MAINUI_RUN_LOAD ? "load" : "execute";
    if (job->cut_short) {
        mainui_ext_log(&fe->ext, "cut short: %s %s %s", kind, job->what, job->command);
        return;
    }
    if (job->timed_out)
        mainui_ext_log(&fe->ext, "%s stopped after %d s: %s", kind,
                       job->kind == MAINUI_RUN_LOAD ? MAINUI_EXT_RUNNER_LOAD_MS / 1000 : MAINUI_EXT_RUNNER_VALUE_MS / 1000, job->what);
    else if (job->status && !job->cancelled)
        mainui_ext_log(&fe->ext, "%s exited %d: %s", kind, job->status, job->what);
    // Results of a table that was read again belong to rows that no longer exist.
    struct mainui_ext_node *n = job->generation == fe->ext.generation ? ext_node(fe, job->node) : NULL;
    if (!n) return;
    fe->ext_changed = 1;
    n->busy = 0;
    if (n->job == job->id) n->job = 0;
    int failed = (job->status != 0 || job->timed_out) && !job->cancelled;
    if (job->kind == MAINUI_RUN_LOAD) {
        if (failed || job->cancelled || !job->tail) return;
        char visible[MAINUI_EXT_RUNNER_LINE * 2];
        for (char *line = job->tail, *end; *line; line = end + 1) {
            end = strchr(line, '\n');
            if (!end) break;
            *end = 0;
            mainui_ext_apply_line(&fe->ext, job->node, line, visible, sizeof(visible));
        }
        return;
    }
    // The prompt and the progress view of the script close with it.
    mainui_model_extension_close(&fe->model, job->node);
    fe->ext_confirm[0] = 0;
    if (job->kind == MAINUI_RUN_VALUE) {
        if (failed) mainui_ext_show_value(&fe->ext, job->node, n->applied);
        else n->applied = n->value;
    }
    if (failed) {
        // A failure shows everything, whatever the output mode.
        if (fe->ext_view_job != job->id) {
            ext_view_start(fe, job->node, job->id, job->status);
            if (job->tail) ext_view_append(fe, job->tail);
        }
        fe->ext_view_status = job->status;
        mainui_model_extension_output(&fe->model, job->node, fe->ext_view_lines);
    } else if (fe->ext_view_job == job->id) {
        fe->ext_view_status = job->cancelled ? 128 + 15 : 0;
        mainui_model_text_lines(&fe->model, fe->ext_view_lines);
    } else if (!job->cancelled && n->output == MAINUI_EXT_OUTPUT_LINE) {
        ext_last_line(job, fe->ext_notice, sizeof(fe->ext_notice));
        if (fe->ext_notice[0]) {
            mainui_model_notice(&fe->model, MAINUI_NOTICE_EXTENSION_LINE);
            fe->notice_ms = 0;
        }
    } else if (!job->cancelled && n->output == MAINUI_EXT_OUTPUT_FULL) {
        ext_view_start(fe, job->node, job->id, 0);
        if (job->tail) ext_view_append(fe, job->tail);
        mainui_model_extension_output(&fe->model, job->node, fe->ext_view_lines);
    }
    ext_reload_screen(fe);
}

// Reads the extension files again; scripts started for an older table end, and their results
// are ignored.
static void ext_scan(struct mainui_frontend *fe) {
    if (mainui_ext_scan(&fe->ext) != 1) return;
    for (int i = 0; i < MAINUI_EXT_RUNNER_JOBS; ++i) {
        const struct mainui_ext_job *job = &fe->ext_runner.jobs[i];
        if (job->id && job->generation != fe->ext.generation) mainui_ext_runner_cancel(&fe->ext_runner, job->id, fe->ext_now);
    }
    struct mainui_tab_state *t = &fe->model.tabs[MAINUI_TAB_SETTINGS];
    int tiles = mainui_model_settings_tiles(&fe->model);
    if (t->focus >= tiles) t->focus = tiles - 1;
}

// Entering the Settings grid, from another tab, Home, or a Settings screen, reads the files
// again: no extension window is open then.
static void ext_check_settings(struct mainui_frontend *fe) {
    const struct mainui_window *w = mainui_model_top(&fe->model);
    int on = fe->model.depth == 1 && w->kind == MAINUI_WIN_TABS && fe->model.tab == MAINUI_TAB_SETTINGS;
    if (on && !fe->ext_on_settings) ext_scan(fe);
    fe->ext_on_settings = on;
}

static void ext_open(struct mainui_frontend *fe) {
    mainui_ext_init(&fe->ext, fe->fs.root);
    if (fe->dry) mainui_ext_set_dry(&fe->ext, fe->record);
    mainui_ext_set_language(&fe->ext, fe->settings.language);
    struct mainui_ext_runner_ops ops = { fe, ext_on_line, ext_on_exit };
    mainui_ext_runner_init(&fe->ext_runner, &ops);
    struct mainui_ext_runner_ops picker_ops = { fe, picker_on_line, picker_on_exit };
    mainui_ext_runner_init(&fe->picker_runner, &picker_ops);
    mainui_ext_runner_set_language(&fe->ext_runner, fe->ext.language);
    if (fe->dry) mainui_ext_runner_set_dry(&fe->ext_runner, fe->record);
    mainui_ext_scan(&fe->ext);
    note(fe, "extensions tiles=%d rows=%d", mainui_ext_tile_count(&fe->ext), fe->ext.node_count);
    struct mainui_extensions callbacks = { fe, ext_tiles, ext_tile_node, ext_children, ext_section_rows, ext_info };
    mainui_model_set_extensions(&fe->model, &callbacks);
}

// Every script ends before the frontend hands over: a launch, power off, Flash Mode, an
// upgrade, a reset or the exit.
static void ext_stop(struct mainui_frontend *fe) {
    mainui_ext_runner_stop_all(&fe->ext_runner);
}

static void carry_out(struct mainui_frontend *fe, const struct mainui_effect *e) {
    struct mainui_list_entry entry;
    ++fe->effects_done;
    switch (e->kind) {
    case MAINUI_EFFECT_SAVE_PRESENTATION: {
        enum mainui_presentation presentation = (enum mainui_presentation)e->value;
        if (presentation != MAINUI_PRESENTATION_STOCK && presentation != MAINUI_PRESENTATION_MATERIAL_YOU) {
            ++fe->effects_failed;
            break;
        }
        note(fe, "frontend selection=%s dry=%d", presentation == MAINUI_PRESENTATION_STOCK ? "stock" : "material-you", fe->dry);
        char path[PATH_BYTES];
        path_under(fe, path, MAINUI_BRICKOS_PREFERENCES_PATH);
        if (!fe->dry && mainui_brickos_preferences_save_presentation(&fe->brickos_preferences, path, presentation) < 0) {
            ++fe->effects_failed;
            mainui_model_notice(&fe->model, MAINUI_NOTICE_PREFERENCE_SAVE_FAILED);
            fe->notice_ms = 0;
            break;
        }
        // Retain the Settings stack. Startup/launch-return processing only runs on startup.
        mainui_model_set_presentation(&fe->model, presentation, 1);
        osd_theme_files(fe, 0); // the OSD's and music server's files follow the frontend
        break;
    }
    case MAINUI_EFFECT_SAVE_BRICKOS_PREFERENCES: {
        if (e->value < MAINUI_BRICKOS_VIEW_DESKTOP || e->value >= MAINUI_BRICKOS_VIEW_COUNT) {
            ++fe->effects_failed;
            fe->model.brickos_initial_view = fe->brickos_preferences.initial_view;
            break;
        }
        if (fe->dry) break;
        char path[PATH_BYTES];
        path_under(fe, path, MAINUI_BRICKOS_PREFERENCES_PATH);
        enum mainui_brickos_initial_view view = (enum mainui_brickos_initial_view)e->value;
        if (mainui_brickos_preferences_save(&fe->brickos_preferences, path, view) < 0) {
            ++fe->effects_failed;
            fe->model.brickos_initial_view = fe->brickos_preferences.initial_view;
            mainui_model_notice(&fe->model, MAINUI_NOTICE_PREFERENCE_SAVE_FAILED);
            fe->notice_ms = 0;
        }
        break;
    }
    case MAINUI_EFFECT_REFRESH_BOOT_VISUALS:
        note(fe, "boot visual refresh target=%s", e->list == MAINUI_LIST_BOOTLOGOS ? "bootlogo" : "splashscreen");
        refresh_boot_visual(fe, (enum mainui_list)e->list);
        break;
    case MAINUI_EFFECT_APPLY_BOOT_VISUAL:
        apply_boot_visual(fe, e);
        break;
    case MAINUI_EFFECT_REFRESH_LOADING_SCREENS:
        note(fe, "loading screen refresh");
        refresh_loading_screens(fe);
        break;
    case MAINUI_EFFECT_APPLY_LOADING_SCREEN:
        apply_loading_screen(fe, e);
        break;
    case MAINUI_EFFECT_FILE_SELECT:
        select_file(fe, e->system, e->index);
        break;
    case MAINUI_EFFECT_FILE_BACK:
        files_back(fe, e->system);
        break;
    case MAINUI_EFFECT_UPGRADE: {
        note(fe, "upgrade confirmed candidate=%d", !!fe->upgrade_path[0]);
        ext_stop(fe);
        if (!fe->upgrade_path[0]) { ++fe->effects_failed; break; }
        if (fe->dry) break;
        char path[PATH_BYTES], temporary[PATH_BYTES + 8];
        path_under(fe, path, "/tmp/.try_upgrade_file");
        snprintf(temporary, sizeof(temporary), "%s.tmp", path);
        int fd = path[0] ? open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600) : -1;
        if (fd < 0) { ++fe->effects_failed; break; }
        FILE *f = fdopen(fd, "wb");
        if (!f) { close(fd); remove(temporary); ++fe->effects_failed; break; }
        size_t length = strlen(fe->upgrade_path);
        int ok = fwrite(fe->upgrade_path, 1, length, f) == length;
        if (fclose(f) != 0) ok = 0;
        if (!ok || rename(temporary, path) < 0) {
            remove(temporary); ++fe->effects_failed; break;
        }
        fe->exit_requested = 1;
        // Stock 64 is a window-loop event, not the firmware marker's process status.
        // The init consumes the marker on its next iteration after a clean exit.
        fe->exit_status = 0;
        break;
    }
    case MAINUI_EFFECT_JUMP_GROUP: {
        struct mainui_window *w = &fe->model.stack[fe->model.depth - 1];
        if (w->kind != MAINUI_WIN_ROMS || w->system != e->system || w->focus != e->index) break;
        int n = mainui_frontend_roms(fe, e->system);
        char letter;
        int next = mainui_catalog_letter_jump(fe->games, n, w->focus, e->value, &letter);
        // At the final group, stock restorePos rejects the out-of-range focus but
        // still moves the page to the bottom, potentially hiding the selection.
        // A missing adjacent group must leave both focus and page unchanged.
        if (next < 0) break;
        w->focus = next;
        if ((e->value < 0 && next <= w->first) || next >= w->first + MAINUI_ROM_LIST_ROWS) {
            int last = n > MAINUI_ROM_LIST_ROWS ? n - MAINUI_ROM_LIST_ROWS : 0;
            w->first = next < last ? next : last;
        }
        char initial[2] = {letter, 0};
        const char *message = letter ? initial : mainui_lang_get(&fe->lang, 219);
        if (!message || !message[0]) message = "Directory";
        note(fe, "letter group system%d row%d", e->system, next);
        if (!fe->dry && mainui_effects_letter_toast(&fe->ops, message, !letter) < 0) ++fe->effects_failed;
        break;
    }
    case MAINUI_EFFECT_SET_DATE: {
        time_t now = time(NULL);
        struct tm local;
        struct mainui_window *w = fe->model.depth ? &fe->model.stack[fe->model.depth - 1] : NULL;
        if (w && w->screen == MAINUI_SCREEN_SET_DATE && localtime_r(&now, &local)) {
            w->date[0] = local.tm_year + 1900; w->date[1] = local.tm_mon + 1;
            w->date[2] = local.tm_mday; w->date[3] = local.tm_hour;
            w->date[4] = local.tm_min; w->date[5] = local.tm_sec; w->date_dst = local.tm_isdst;
        }
        note(fe, "open date editor");
        break;
    }
    case MAINUI_EFFECT_APPLY_DATE:
        note(fe, "apply date %s", e->text);
        if (!fe->dry && (!fe->runner.run || mainui_actions_apply(&fe->runner, e) < 0)) ++fe->effects_failed;
        break;
    case MAINUI_EFFECT_SET:
        if (mainui_model_led_setting(e->setting)) {
            // LED Policy and its Custom rows go to the LED daemon's configuration at once.
            led_set(fe, e);
            break;
        }
        if (mainui_model_appearance_setting(e->setting)) {
            // BrickOS Appearance's rows apply at once and go to appearance.json once they settle.
            note(fe, "set appearance %s=%s", mainui_appearance_label(e->setting, 0), mainui_appearance_name(e->setting, e->value));
            mainui_appearance_set(&fe->appearance, e->setting, e->value);
            fe->appearance_pending = 1;
            fe->appearance_pending_since = 0;
            break;
        }
        if (mainui_model_mapping_setting(e->setting)) {
            // Button Mapping's rows go to the input profile once they settle, not to system.json.
            note(fe, "set mapping setting%d=%d", e->setting, e->value);
            fe->mapping_pending = 1;
            fe->mapping_pending_since = 0;
            break;
        }
        note(fe, "set setting%d=%d", e->setting, e->value);
        if (e->setting != MAINUI_SET_THEME && e->setting != MAINUI_SET_LANGUAGE)
            mainui_settings_set(&fe->settings, (enum mainui_setting)e->setting, e->value);
        if (e->setting == MAINUI_SET_VOL) mainui_settings_set(&fe->settings, MAINUI_SET_MUTE, 0);
        if (fe->dry) break;
        // Bluetooth off tells the manager first, then asks hardwareservice (static order).
        if (e->setting == MAINUI_SET_BLUETOOTH && !e->value) {
            bt_request(fe, "switchoff");
            bt_pairable(fe, 0);
            fe->bt_pending[0] = 0;
            fe->bt_queued = fe->bt_scanning = fe->bt_shown = 0;
            bt_audio_forget(fe);
        }
        if (e->setting == MAINUI_SET_RUMBLELEVEL) rumble_finish(fe, 0);
        if (mainui_effects_apply(&fe->ops, e, fe->settings.values) < 0) ++fe->effects_failed;
        if (e->setting == MAINUI_SET_VOL) volume_outputs(fe, e->value);
        if (e->setting == MAINUI_SET_BGMVOL && fe->bgm_volume) fe->bgm_volume(fe->bgm_context, e->value);
        if (e->setting == MAINUI_SET_RUMBLELEVEL) rumble_start(fe);
        if (fe->runner.run && mainui_actions_apply(&fe->runner, e) < 0) ++fe->effects_failed;
        if (e->setting == MAINUI_SET_OSDSHORTCUT) osd_hotkey_file(fe);
        break;
    case MAINUI_EFFECT_PERSIST:
        persist_settings(fe);
        break;
    case MAINUI_EFFECT_EXTENSION_OPEN:
        ext_run_loads(fe, e->value, e->index);
        break;
    case MAINUI_EFFECT_EXTENSION_RUN:
        ext_run_command(fe, e->index);
        break;
    case MAINUI_EFFECT_EXTENSION_CHANGE:
        ext_change(fe, e->index, e->value, e->launcher);
        break;
    case MAINUI_EFFECT_EXTENSION_CANCEL:
        ext_cancel(fe, e->index);
        break;
    case MAINUI_EFFECT_EXTENSION_ANSWER:
        ext_answer(fe, e->index, e->value);
        break;
    case MAINUI_EFFECT_EXTENSION_LEAVE:
        ext_settle(fe, fe->ext_now, 1);
        break;
    case MAINUI_EFFECT_REFRESH_MAPPING:
        fe->mapping_read_ms = fe->ext_now;
        mapping_read(fe);
        break;
    case MAINUI_EFFECT_SAVE_MAPPING:
        if (fe->mapping_pending) mapping_save(fe, 0);
        break;
    case MAINUI_EFFECT_RESET_MAPPING:
        mapping_save(fe, 1);
        break;
    case MAINUI_EFFECT_REFRESH_LEDS:
        fe->led_read_ms = fe->ext_now;
        led_read(fe);
        break;
    case MAINUI_EFFECT_REFRESH_FONTS:
        appearance_fonts(fe);
        break;
    case MAINUI_EFFECT_SAVE_APPEARANCE:
        if (fe->appearance_pending) appearance_save(fe);
        break;
    case MAINUI_EFFECT_RESET_APPEARANCE: {
        // Every choice goes back to its default; members the file holds besides them stay.
        static struct mainui_json document;
        document = fe->appearance.document;
        mainui_appearance_defaults(&fe->appearance);
        fe->appearance.document = document;
        note(fe, "reset appearance");
        appearance_save(fe);
        break;
    }
    case MAINUI_EFFECT_LAUNCH:
        if (e->list == MAINUI_LIST_ROMS) {
            int folder = open_rom_folder(fe, e->system, e->index);
            if (folder) {
                if (folder < 0) { ++fe->effects_failed; note(fe, "ROM folder refused system=%d index=%d", e->system, e->index); }
                break;
            }
        }
        // Clear before validation too: an absent target or ARE adapter must not
        // leave a stale competing handoff for a later clean exit to replay.
        if (!fe->dry) mainui_launch_clear(fe->fs.root);
        if (effect_entry(fe, e, &entry) < 0) {
            note(fe, "launch failed list=%d index=%d", e->list, e->index);
            ++fe->effects_failed;
            break;
        }
        {
            const char *launcher = NULL;
            if (e->list == MAINUI_LIST_ROMS && e->launcher >= 0 && mainui_frontend_system(fe, e->system)) {
                const struct mainui_system *s = mainui_frontend_system(fe, e->system);
                if (e->launcher < s->launcher_count) launcher = s->launchers[e->launcher];
            }
            launch_entry(fe, &entry, launcher,
                         e->list == MAINUI_LIST_NETPLAY ? fe->hosts[e->index].hostip : NULL);
        }
        break;
    case MAINUI_EFFECT_LAUNCH_GAMESWITCHER: {
        // The GameSwitcher card, or the Default destination at a fresh start: its app, by folder,
        // through the same launch and return as Apps.
        const struct mainui_app *app = gameswitcher_app(fe);
        if (!fe->dry) mainui_launch_clear(fe->fs.root);
        if (!app || !app->launch[0]) {
            note(fe, "GameSwitcher launch failed: no app");
            ++fe->effects_failed;
            break;
        }
        char launch[1024];
        snprintf(launch, sizeof(launch), "%s/%s", app->directory, app->launch);
        mainui_list_entry_set(&entry, app->label, "", "", launch, "", MAINUI_ENTRY_APP);
        launch_entry_recent(fe, &entry, NULL, NULL, e->value != 0);
        break;
    }
    case MAINUI_EFFECT_ADD_FAVORITE:
        if (effect_entry(fe, e, &entry) < 0) { ++fe->effects_failed; break; }
        note(fe, "add favorite %s", entry.label);
        mainui_favorites_add(&fe->favorites, &entry);
        save_list(fe, &fe->favorites, "/mnt/SDCARD/Roms/favourite2.json");
        break;
    case MAINUI_EFFECT_REMOVE_FAVORITE: {
        if (effect_entry(fe, e, &entry) < 0) { ++fe->effects_failed; break; }
        int at = mainui_favorites_find(&fe->favorites, &entry);
        note(fe, "remove favorite %s", entry.label);
        if (at >= 0) mainui_list_remove(&fe->favorites, (size_t)at);
        save_list(fe, &fe->favorites, "/mnt/SDCARD/Roms/favourite2.json");
        break;
    }
    case MAINUI_EFFECT_CLEAR_RECENT:
        note(fe, "clear recent");
        mainui_list_clear(&fe->recent);
        save_list(fe, &fe->recent, "/mnt/SDCARD/Roms/recentlist.json");
        break;
    case MAINUI_EFFECT_CLEAR_FAVORITES:
        note(fe, "clear favorites");
        mainui_list_clear(&fe->favorites);
        save_list(fe, &fe->favorites, "/mnt/SDCARD/Roms/favourite2.json");
        break;
    case MAINUI_EFFECT_DELETE_ROM:
        if (effect_entry(fe, e, &entry) < 0) { ++fe->effects_failed; break; }
        note(fe, "delete rom %s", entry.rompath);
        if (!fe->dry) {
            char path[PATH_BYTES];
            path_under(fe, path, entry.rompath);
            if (unlink(path) != 0) ++fe->effects_failed;
            drop_games(fe);
        }
        break;
    case MAINUI_EFFECT_REFRESH_ROMS:
        note(fe, "refresh roms");
        stop_counting(fe);
        fe->refresh_failed = 0;
        __atomic_store_n(&fe->refresh_done, 0, __ATOMIC_RELEASE);
        fe->refresh_pending = 1;
        break;
    case MAINUI_EFFECT_SEARCH:
        search(fe, e->text, e->system);
        note(fe, "search %s results=%d", e->text, fe->hit_count);
        break;
    case MAINUI_EFFECT_TOGGLE_EDIT:
        if (e->system == MAINUI_TAB_APPS && e->index >= 0 && e->index < fe->app_count) fe->apps[e->index].show = !fe->apps[e->index].show;
        else if (e->system != MAINUI_TAB_APPS && e->index >= 0 && e->index < fe->system_count) fe->systems[e->index].show = !fe->systems[e->index].show;
        break;
    case MAINUI_EFFECT_CANCEL_EDIT:
        note(fe, "cancel edit");
        load_catalog(fe);
        break;
    case MAINUI_EFFECT_SAVE_EDIT:
        note(fe, "save edit tab=%d", e->system);
        if (!fe->dry) save_show(fe, e->system);
        load_catalog(fe);
        break;
    case MAINUI_EFFECT_PREVIEW_THEME:
        if (e->index > 0 && e->index < fe->theme_count && fe->themes[e->index].preview[0]) {
            char path[PATH_BYTES]; struct stat st;
            path_under(fe, path, fe->themes[e->index].preview);
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                note(fe, "theme preview index=%d", e->index);
                if (!fe->dry && mainui_effects_theme_preview(&fe->ops, fe->themes[e->index].preview) < 0)
                    ++fe->effects_failed;
            }
        }
        break;
    case MAINUI_EFFECT_SELECT:
        note(fe, "select list=%d index=%d", e->list, e->index);
        if (e->list == MAINUI_LIST_FONTS && e->index >= 0 && e->index <= fe->font_count) {
            // Entry 0 is Default, the system font; the model saves when the list closes.
            const char *file = e->index ? fe->fonts[e->index - 1].file : "";
            snprintf(fe->appearance.font, sizeof(fe->appearance.font), "%s", file);
            fe->appearance_pending = 1;
            fe->appearance_pending_since = 0;
        }
        if (e->list == MAINUI_LIST_THEMES && e->index >= 0 && e->index < fe->theme_count) {
            const char *theme = fe->themes[e->index].path;
            if (strcmp(fe->settings.theme, theme) != 0) {
                if (mainui_settings_set_theme(&fe->settings, theme) < 0) { ++fe->effects_failed; break; }
                if (!fe->dry && fe->theme_audio) fe->theme_audio(fe->theme_context, &fe->fs, fe->settings.theme);
                persist_settings(fe);
                osd_theme_files(fe, 1);
                // The next background counts as new, so the home tabs fade and run their effect
                // again (static: ThemeMenu::select marks it __background_changed__).
                snprintf(fe->backdrop.path, sizeof(fe->backdrop.path), "__background_changed__");
            }
        }
        if (e->list == MAINUI_LIST_LANGUAGES && e->index >= 0 && e->index < fe->language_count) {
            mainui_settings_set_language(&fe->settings, fe->languages[e->index]);
            load_language(fe);
            drop_games(fe);
            mainui_ext_set_language(&fe->ext, fe->settings.language);
            mainui_ext_runner_set_language(&fe->ext_runner, fe->ext.language);
            // Values the scripts printed in the old language are read again.
            ext_reload_screen(fe);
        }
        if (e->list == MAINUI_LIST_TIMEZONES) select_zone(fe, e->system, e->index);
        if (e->list == MAINUI_LIST_PAIRED || e->list == MAINUI_LIST_BLUETOOTH) bt_select(fe, (enum mainui_list)e->list, e->index);
        break;
    case MAINUI_EFFECT_WIFI_CONNECT:
    case MAINUI_EFFECT_WIFI_RECONNECT:
    case MAINUI_EFFECT_WIFI_FORGET:
        wifi_start(fe, e);
        break;
    case MAINUI_EFFECT_EDIT_TEXT:
        edit_hotspot(fe, e->value, e->text);
        break;
    case MAINUI_EFFECT_REMOVE_DEVICE:
        bt_remove(fe, e->index);
        break;
    case MAINUI_EFFECT_FACTORY_RESET:
        ext_stop(fe);
        note(fe, "factory reset");
        if (fe->dry || !fe->runner.run) break;
        if (mainui_launch_other_platform_card(fe->fs.root)) {
            if (fe->runner.run(fe->runner.context, "rm -rf /mnt/P2EXT/.dbs/*") != 0) ++fe->effects_failed;
        } else {
            for (int i = 0; i < fe->system_count; ++i)
                if (mainui_catalog_clear_caches(&fe->fs, fe->systems[i].rompath) < 0) ++fe->effects_failed;
            for (int i = 0; i < fe->best_count; ++i)
                if (mainui_catalog_clear_caches(&fe->fs, fe->bests[i].rompath) < 0) ++fe->effects_failed;
        }
        if (fe->runner.run(fe->runner.context, "sync") != 0 || mainui_actions_apply(&fe->runner, e) < 0) ++fe->effects_failed;
        break;
    case MAINUI_EFFECT_POWER_OFF:
    case MAINUI_EFFECT_REBOOT:
        ext_stop(fe);
        // The POWER callback (415e60) leaves the keymon-owned marker intact
        // and skips its command when it exists. Settings (496070) always runs it. Reboot
        // follows the same rules with its own command.
        note(fe, "%s source=%s", e->kind == MAINUI_EFFECT_REBOOT ? "reboot" : "power off",
             e->value == 2 ? "marker" : e->value == 1 ? "hold" : "settings");
        if (fe->power_pending) { ++fe->effects_failed; break; }
        fe->power_runner = fe->runner;
        fe->power_source = e->value;
        fe->power_reboot = e->kind == MAINUI_EFFECT_REBOOT;
        fe->power_wait_ms = MAINUI_POWER_WAIT_MS;
        fe->power_failed = 0;
        __atomic_store_n(&fe->power_done, 0, __ATOMIC_RELAXED);
        fe->power_pending = 1;
        break;
    case MAINUI_EFFECT_IO_TEST:
        io_shortcut(fe, e->value != 0);
        note(fe, "effect kind=%d", (int)e->kind);
        if (!fe->dry && fe->runner.run && mainui_actions_apply(&fe->runner, e) < 0) ++fe->effects_failed;
        break;
    case MAINUI_EFFECT_FLASH_MODE:
        ext_stop(fe);
        note(fe, "effect kind=%d", (int)e->kind);
        if (!fe->dry && fe->runner.run && mainui_actions_apply(&fe->runner, e) < 0) ++fe->effects_failed;
        break;
    default:
        // Text entries go to the runner.
        note(fe, "effect kind=%d", (int)e->kind);
        if (!fe->dry && fe->runner.run && mainui_actions_apply(&fe->runner, e) < 0) ++fe->effects_failed;
        break;
    }
}

static void carry_out_all(struct mainui_frontend *fe) {
    struct mainui_effect e;
    int theme_selection = 0;
    // A launch ends the frontend's work: later effects of the same key are not carried out.
    while (!fe->exit_requested && mainui_model_next_effect(&fe->model, &e)) {
        // SELECT owns theme audio, conditional persistence and hint publication, in that
        // order. Skip the model's subsequent PERSIST; same-theme selection saves nothing.
        if (theme_selection && e.kind == MAINUI_EFFECT_PERSIST) { theme_selection = 0; continue; }
        if (e.kind == MAINUI_EFFECT_SELECT && e.list == MAINUI_LIST_THEMES)
            theme_selection = e.index >= 0 && e.index < fe->theme_count;
        else if (e.kind != MAINUI_EFFECT_SET || e.setting != MAINUI_SET_THEME) theme_selection = 0;
        carry_out(fe, &e);
    }
    if (fe->exit_requested) rumble_finish(fe, 1);
    int browser = 0;
    for (int i = 0; i < fe->model.depth; ++i) browser |= fe->model.stack[i].kind == MAINUI_WIN_FILES;
    if (!browser && fe->file_depth) drop_files(fe);
}

int mainui_frontend_sync(struct mainui_frontend *fe) {
    // keymon changes the volume and mute itself (the buttons); its values are the current ones,
    // and a later save must not write back an older value of ours (effects x1).
    static const struct { enum mainui_setting setting; int index; } owned[] = {
        {MAINUI_SET_VOL, MAINUI_SHM_VOL}, {MAINUI_SET_MUTE, MAINUI_SHM_MUTE},
    };
    int changed = 0;
    if (!fe->ops.shm_get) return 0;
    for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); ++i) {
        int value;
        if (fe->ops.shm_get(fe->ops.context, owned[i].index, &value) != 0) continue;
        if (value == fe->settings.values[owned[i].setting]) continue;
        fe->settings.values[owned[i].setting] = value;
        fe->settings.changed &= ~(1ULL << owned[i].setting);
        fe->model.settings[owned[i].setting] = value;
        changed = 1;
    }
    return changed;
}

int mainui_frontend_key_at(struct mainui_frontend *fe, enum mainui_key key, uint64_t now_ms) {
    if (fe->exit_requested) return 0;
    return mainui_frontend_key(fe, mainui_model_timed_key(&fe->model, key, now_ms));
}

int mainui_frontend_key(struct mainui_frontend *fe, enum mainui_key key) {
    if (fe->exit_requested) return 0;
    mainui_frontend_sync(fe);
    if (key == MAINUI_KEY_LEFT || key == MAINUI_KEY_RIGHT) fe->carousel.step = key == MAINUI_KEY_LEFT ? -1 : 1;
    // Page and group keys move by more than a card: the next move puts the cards in place.
    if (key == MAINUI_KEY_L1 || key == MAINUI_KEY_R1 || key == MAINUI_KEY_L2 || key == MAINUI_KEY_R2) fe->carousel.jump = 1;
    int changed = mainui_model_key(&fe->model, key);
    carry_out_all(fe);
    ext_check_settings(fe);
    const struct mainui_window *w = mainui_model_top(&fe->model);
    if (!w || w->kind != MAINUI_WIN_DIALOG || w->dialog != MAINUI_DIALOG_UPGRADE)
        fe->upgrade_path[0] = 0;
    track_positions(fe);
    if (changed && fe->model.presentation == MAINUI_PRESENTATION_MATERIAL_YOU) fe->brickos_view_touched = 1;
    return changed;
}

int mainui_frontend_event(struct mainui_frontend *fe, enum mainui_event event) {
    int changed = mainui_model_event(&fe->model, event);
    carry_out_all(fe);
    ext_check_settings(fe);
    track_positions(fe);
    if (changed && fe->model.presentation == MAINUI_PRESENTATION_MATERIAL_YOU) fe->brickos_view_touched = 1;
    return changed;
}

int mainui_frontend_offer_upgrade(struct mainui_frontend *fe, const char *path,
                                 uint64_t total, uint64_t free_bytes) {
    if (!fe || fe->exit_requested || fe->model.depth >= MAINUI_STACK_MAX || !path || path[0] != '/' ||
        strlen(path) >= sizeof(fe->upgrade_path) || strchr(path, '\n') || strchr(path, '\r')) return -1;
    const struct mainui_window *top = mainui_model_top(&fe->model);
    if (!top || (top->kind == MAINUI_WIN_DIALOG && top->dialog == MAINUI_DIALOG_UPGRADE)) return -1;
    const char *base = strrchr(path, '/') + 1;
    if (strncasecmp(base, "trimui_", 7) || !strrchr(base, '.')) return -1;
    char full[PATH_BYTES]; struct stat st;
    path_under(fe, full, path);
    if (!full[0] || !file_path_inside(fe, path) || stat(full, &st) < 0 || !S_ISREG(st.st_mode)) return -1;
    fe->upgrade_path[0] = 0;
    // FileAction checks total > 0xfffffff, then whole free MiB > 499.
    uint64_t megabytes = total > 0xfffffff ? free_bytes >> 20 : 0;
    if (total <= 0xfffffff || megabytes <= 499) {
        mainui_model_notice(&fe->model, MAINUI_NOTICE_UPGRADE_SPACE);
        struct mainui_window *notice = &fe->model.stack[fe->model.depth - 1];
        if (notice->kind != MAINUI_WIN_NOTICE) return -1;
        notice->index = (int)megabytes;
        fe->notice_ms = 0;
        return 0;
    }
    if (!mainui_frontend_event(fe, MAINUI_EVENT_UPGRADE_FOUND)) return -1;
    snprintf(fe->upgrade_path, sizeof(fe->upgrade_path), "%s", path);
    return 1;
}

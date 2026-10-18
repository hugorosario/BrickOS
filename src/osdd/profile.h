#ifndef BRICKOS_OSDD_PROFILE_H
#define BRICKOS_OSDD_PROFILE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

// The optional toast theme profile an installed frontend writes (osdd-theme). The daemon only
// reads it; without a usable profile every toast keeps the stock look.
#define OSDD_PROFILE_PATH "/mnt/SDCARD/BrickOS/osd/theme-profile.json"
#define OSDD_PROFILE_MAX_BYTES 8192
#define OSDD_PROFILE_STOCK_FONT "/usr/trimui/osd/regular.ttf"
#define OSDD_PROFILE_DEFAULT_FONT_SIZE 32  // The stock size of a record without a fontsize.
#define OSDD_PROFILE_DEFAULT_PADDING 24    // The stock inset of a wide toast with an icon.
#define OSDD_PROFILE_CHECK_MS 1000         // At most one stat of the profile per second.
// Replaced profiles kept while toasts use them: one more than the toast slots, so a reload always
// fits once the unused ones are freed.
#define OSDD_PROFILE_RETIRED 17

// Opens the profile's typeface and background image: SDL_ttf and SDL_image in the daemon,
// fakes in the host tests. A NULL handle means the file cannot be used.
struct osdd_profile_assets {
    void *context;
    void *(*open_font)(void *context, const char *path, int size);
    void (*close_font)(void *context, void *font);
    void *(*open_image)(void *context, const char *path);
    void (*close_image)(void *context, void *image);
};

// One loaded profile. It never changes once loaded, so a toast composed with it keeps its look;
// the loader frees it when no toast uses it any more.
struct osdd_profile {
    char font_path[256];              // "" for the stock typeface.
    int font_size;                    // Pixels.
    int has_font_color;               // fontColor was given; font_color is white otherwise.
    unsigned char font_color[4];      // Red, green, blue, alpha.
    char background_path[256];        // "" without an image.
    int margins[4];                   // The image's nine-patch margins: left, top, right, bottom.
    int has_background_color;
    unsigned char background_color[4];
    int padding;
    // Draws every toast icon as a silhouette in this color, for icons the background would hide.
    int has_icon_color;
    unsigned char icon_color[4];
    // In-game menu members, which the OSD ignores: the fill behind the focused row, the muted
    // text of labels and values, and the text over the focused row and card.
    int has_selector_color;
    unsigned char selector_color[4];
    int has_secondary_color;
    unsigned char secondary_color[4];
    int has_selected_color;
    unsigned char selected_color[4];
    void *font;                       // The open typeface, font_path's or the stock one.
    void *background;                 // The open background image, or NULL.
};

// Parses a profile: every member is optional, unknown members are ignored, and a member of the
// wrong type or out of range rejects the whole profile. Returns 0, or -1 with an error.
int osdd_profile_parse(const unsigned char *data, size_t length, struct osdd_profile *profile,
                       char *error, size_t error_size);
// Reads and parses a profile file without opening its assets, for a consumer that opens them its
// own way (the in-game menu). Returns 0, or -1 with an error.
int osdd_profile_read(const char *path, struct osdd_profile *profile, char *error, size_t error_size);
// Reads, parses, and opens a profile file. Returns 0 with a new profile, or -1 with an error
// and no profile: then toasts keep the stock look.
int osdd_profile_load(const char *path, const struct osdd_profile_assets *assets,
                      struct osdd_profile **profile, char *error, size_t error_size);
void osdd_profile_free(struct osdd_profile *profile, const struct osdd_profile_assets *assets);

// Whether a toast still uses a profile: the daemon looks through its occupied toast slots.
typedef int (*osdd_profile_in_use)(void *context, const struct osdd_profile *profile);

struct osdd_profile_loader {
    const char *path;
    struct osdd_profile_assets assets;
    osdd_profile_in_use in_use;
    void *in_use_context;
    int (*stat_file)(const char *path, struct stat *st);  // stat(2); tests count the calls.
    struct osdd_profile *current;     // NULL while toasts keep the stock look.
    // Profiles replaced while toasts still used them; freed once no toast does.
    struct osdd_profile *retired[OSDD_PROFILE_RETIRED];
    size_t retired_count;
    uint64_t next_check_ms;
    int checked;                      // The file was looked at once.
    int present;                      // It existed at the last look.
    struct timespec mtime;            // Its signature at the last look.
    off_t size;
    ino_t inode;
    char error[160];                  // Why the last load gave no profile.
};

void osdd_profile_loader_init(struct osdd_profile_loader *loader, const char *path,
                              const struct osdd_profile_assets *assets,
                              osdd_profile_in_use in_use, void *in_use_context);
// Looks at the file at most once per OSDD_PROFILE_CHECK_MS of the daemon's clock and loads it
// again when its modification time, size, or inode changed. Returns 1 when the active profile
// changed (loaded, replaced, or dropped), else 0.
int osdd_profile_loader_tick(struct osdd_profile_loader *loader, uint64_t now_ms);
// Frees the replaced profiles no toast uses any more.
void osdd_profile_loader_collect(struct osdd_profile_loader *loader);
void osdd_profile_loader_close(struct osdd_profile_loader *loader);

#endif

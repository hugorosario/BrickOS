#ifndef BRICKOS_LEDD_CONFIG_H
#define BRICKOS_LEDD_CONFIG_H

#include "../mainui/json.h"

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

// The LED daemon's configuration: /mnt/SDCARD/BrickOS/ledd/config.json, strict JSON of at most
// 4 KiB. MainUI writes it; the daemon reads it again whenever its inode, size or mtime changes.

#define LEDD_CONFIG_PATH "/mnt/SDCARD/BrickOS/ledd/config.json"
#define LEDD_CONFIG_BYTES 4096
#define LEDD_CONFIG_VERSION 1
#define LEDD_TIME_MIN 1
#define LEDD_TIME_MAX 10

enum ledd_mode {
    LEDD_MODE_DEFAULT,
    LEDD_MODE_BATTERY,
    LEDD_MODE_CPU,
    LEDD_MODE_TEMPERATURE,
    LEDD_MODE_CHARGING,
    LEDD_MODE_CUSTOM,
    LEDD_MODE_COUNT
};

// Numbered as BrickTools' EFFECT; 1 to 7 are also the driver's effect numbers.
enum ledd_effect {
    LEDD_EFFECT_OFF,
    LEDD_EFFECT_LINEAR,
    LEDD_EFFECT_BREATH,
    LEDD_EFFECT_SNIFF,
    LEDD_EFFECT_STATIC,
    LEDD_EFFECT_BLINK1,
    LEDD_EFFECT_BLINK2,
    LEDD_EFFECT_BLINK3,
    LEDD_EFFECT_COUNT
};

// Numbered as BrickTools' COLOR: 0 picks one of the seven others for each cycle.
enum ledd_colour {
    LEDD_COLOUR_RANDOM,
    LEDD_COLOUR_RED,
    LEDD_COLOUR_GREEN,
    LEDD_COLOUR_BLUE,
    LEDD_COLOUR_YELLOW,
    LEDD_COLOUR_PURPLE,
    LEDD_COLOUR_CYAN,
    LEDD_COLOUR_WHITE,
    LEDD_COLOUR_COUNT
};

// BrickTools' defaults for a Custom member the file leaves out.
#define LEDD_DEFAULT_EFFECT LEDD_EFFECT_LINEAR
#define LEDD_DEFAULT_COLOUR LEDD_COLOUR_RED
#define LEDD_DEFAULT_TIME 1

struct ledd_config {
    enum ledd_mode mode;
    enum ledd_effect effect;
    enum ledd_colour colour;
    int time;  // seconds per effect cycle, LEDD_TIME_MIN to LEDD_TIME_MAX
};

enum ledd_config_status { LEDD_CONFIG_OK, LEDD_CONFIG_MISSING, LEDD_CONFIG_INVALID };

const char *ledd_mode_name(enum ledd_mode mode);
const char *ledd_effect_name(enum ledd_effect effect);
const char *ledd_colour_name(enum ledd_colour colour);
int ledd_mode_parse(const char *name, enum ledd_mode *mode);
int ledd_effect_parse(const char *name, enum ledd_effect *effect);
int ledd_colour_parse(const char *name, enum ledd_colour *colour);

// Mode default with the Custom defaults.
void ledd_config_defaults(struct ledd_config *config);

// Parses a document. Members other than version, mode, effect, colour and time are allowed and
// ignored. On failure config holds the defaults and error names the problem. When document is
// not NULL it receives the parsed members whenever the text is a JSON object, valid or not, so a
// writer can keep members it does not know.
enum ledd_config_status ledd_config_parse(struct ledd_config *config, const char *text, size_t length,
                                          struct mainui_json *document, char *error, size_t error_size);
// Reads and parses path: LEDD_CONFIG_MISSING when it does not exist (config holds the defaults),
// LEDD_CONFIG_INVALID when it is not a regular file, is larger than LEDD_CONFIG_BYTES, or does
// not parse.
enum ledd_config_status ledd_config_load(struct ledd_config *config, const char *path,
                                         struct mainui_json *document, char *error, size_t error_size);

// Formats config as the file's text: the five known members first, then every other member of
// document (may be NULL) in its order. Returns 0, or -1 when it does not fit in size or exceeds
// LEDD_CONFIG_BYTES.
int ledd_config_format(const struct ledd_config *config, const struct mainui_json *document, char *text,
                       size_t size, size_t *length);

// The file's signature at the last look.
struct ledd_config_stamp {
    int checked;
    int present;
    dev_t device;
    ino_t inode;
    off_t size;
    struct timespec mtime;
};

// Returns 1 when the file appeared, disappeared, or changed inode, size or mtime since the last
// call (and on the first call), updating the stamp; 0 when it is unchanged.
int ledd_config_changed(struct ledd_config_stamp *stamp, const char *path);

#endif

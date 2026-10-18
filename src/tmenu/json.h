#ifndef BRICKOS_TMENU_JSON_H
#define BRICKOS_TMENU_JSON_H

#include <stddef.h>

// A reader for the flat JSON objects the menu uses: /mnt/UDISK/system.json and the
// language .short files. Top-level members are indexed; nested values are skipped.
#define TMENU_JSON_MEMBERS 128

enum tmenu_json_type { TMENU_JSON_OTHER, TMENU_JSON_NUMBER, TMENU_JSON_STRING };

struct tmenu_json_member {
    char key[48];
    enum tmenu_json_type type;
    // cJSON's valueint for numbers: the double, saturated to the int range.
    int number;
    char text[256];
};

struct tmenu_json {
    struct tmenu_json_member members[TMENU_JSON_MEMBERS];
    size_t count;
};

// Parses a top-level object. Returns 0, or -1 when the text is not one.
int tmenu_json_parse(struct tmenu_json *json, const char *text, size_t length);
// Returns the first member whose key matches, ignoring case as cJSON_GetObjectItem does.
const struct tmenu_json_member *tmenu_json_get(const struct tmenu_json *json, const char *key);

#endif

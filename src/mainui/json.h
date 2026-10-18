#ifndef BRICKOS_MAINUI_JSON_H
#define BRICKOS_MAINUI_JSON_H

#include <stddef.h>

// Flat JSON objects as the official MainUI reads and writes them with cJSON: the lines of
// recentlist.json and favourite2.json, and system.json. Members keep their order and
// duplicates (favourite2.json lines repeat "order"); a value that is not a string or a number
// keeps its raw text. Strings are decoded to bytes; \u escapes below 0x100 become one byte,
// as cJSON's output of the stock files needs (their app entries hold raw memory bytes).

#define MAINUI_JSON_MEMBERS 96
#define MAINUI_JSON_KEY 64
#define MAINUI_JSON_TEXT 1024

enum mainui_json_type { MAINUI_JSON_STRING, MAINUI_JSON_NUMBER, MAINUI_JSON_RAW };

struct mainui_json_member {
    char key[MAINUI_JSON_KEY];
    enum mainui_json_type type;
    double number;
    char text[MAINUI_JSON_TEXT];  // STRING: decoded bytes; RAW: the value's source text
    size_t text_length;           // STRING may hold NUL bytes
};

struct mainui_json {
    struct mainui_json_member members[MAINUI_JSON_MEMBERS];
    size_t count;
};

// Parses one object. Returns 0, or -1 when the text is not a flat object that fits.
int mainui_json_parse(struct mainui_json *json, const char *text, size_t length);
// Like mainui_json_parse, but a member whose value is too long to keep is left out instead of
// failing the parse, as the official, which reads with cJSON, still loads such a file.
int mainui_json_parse_fitting(struct mainui_json *json, const char *text, size_t length);
// Parses one object of any size, handing each member to callback, which returns -1 to stop.
// Returns 0, or -1 when the text is not an object or the callback stopped.
int mainui_json_each(const char *text, size_t length, int (*callback)(void *, const struct mainui_json_member *),
                     void *context);

// Finds a member of an object without copying it (for values of any size, such as the list of
// /tmp/state.json): sets value to its source text. Returns 0, or -1 when absent or malformed.
int mainui_json_find(const char *text, size_t length, const char *key, const char **value, size_t *value_length);
// Hands each element of an array, as its source text, to callback, which returns -1 to stop.
// Returns 0, or -1 when the text is not an array or the callback stopped.
int mainui_json_array_each(const char *text, size_t length, int (*callback)(void *, const char *, size_t), void *context);

// The first member with this key, compared without case as cJSON_GetObjectItem does; NULL if
// none. mainui_json_last returns the last one.
const struct mainui_json_member *mainui_json_get(const struct mainui_json *json, const char *key);
const struct mainui_json_member *mainui_json_last(const struct mainui_json *json, const char *key);

// Appends bytes as a JSON string with cJSON's escapes: quote, backslash, \b \f \n \r \t, and
// \u00XX for other bytes below 0x20. Returns the length written, or -1 if out is too small.
int mainui_json_escape(char *out, size_t size, const char *bytes, size_t length);

#endif

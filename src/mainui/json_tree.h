#ifndef BRICKOS_MAINUI_JSON_TREE_H
#define BRICKOS_MAINUI_JSON_TREE_H

#include <stddef.h>

// Nested JSON documents, for the settings extension files (BrickTools menu.json, EmulationStation
// menus, their language catalogs). json.c stays the reader of the stock flat files. The whole
// document lives in one arena that mainui_json_tree_free releases. Strings are UTF-8 with \u
// escapes decoded, surrogate pairs included; \u0000 and lone surrogates are errors, so every
// string is also a C string. The input is limited to 512 KiB and the nesting to 32 levels.

#define MAINUI_JSON_TREE_MAX_BYTES (512 * 1024)
#define MAINUI_JSON_TREE_MAX_DEPTH 32

enum mainui_json_tree_type {
    MAINUI_JSON_TREE_NULL, MAINUI_JSON_TREE_FALSE, MAINUI_JSON_TREE_TRUE, MAINUI_JSON_TREE_NUMBER,
    MAINUI_JSON_TREE_STRING, MAINUI_JSON_TREE_ARRAY, MAINUI_JSON_TREE_OBJECT,
};

struct mainui_json_value {
    enum mainui_json_tree_type type;
    double number;                   // NUMBER
    const char *string;              // STRING, NUL-terminated
    size_t length;                   // STRING: bytes; ARRAY and OBJECT: elements or members
    const char *key;                 // the member's key inside an object, else NULL
    struct mainui_json_value *first; // ARRAY and OBJECT: the first element or member
    struct mainui_json_value *next;  // the next sibling
};

struct mainui_json_tree_chunk;

struct mainui_json_tree {
    struct mainui_json_value *root;
    struct mainui_json_tree_chunk *chunks;
};

// Parses text into tree. Returns 0, or -1 with "offset N: reason" in error (size may be 0).
// On failure tree holds nothing to free.
int mainui_json_tree_parse(struct mainui_json_tree *tree, const char *text, size_t length, char *error, size_t size);
// Reads and parses a file; a missing file, a read error or a file over the limit fail as above.
int mainui_json_tree_load(struct mainui_json_tree *tree, const char *path, char *error, size_t size);
void mainui_json_tree_free(struct mainui_json_tree *tree);

// The last member of an object with this exact key (a later duplicate wins), or NULL.
const struct mainui_json_value *mainui_json_tree_get(const struct mainui_json_value *object, const char *key);
// The member's string, or NULL when absent or not a string.
const char *mainui_json_tree_string(const struct mainui_json_value *object, const char *key);
// The member's number, a boolean as 0 or 1, or a string holding a whole number; returns 0 and sets
// value, or -1.
int mainui_json_tree_number(const struct mainui_json_value *object, const char *key, double *value);

#endif

#define _POSIX_C_SOURCE 200809L
#include "appearance_fonts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int compare_fonts(const void *a, const void *b) {
    return strcmp(((const struct mainui_font_entry *)a)->file, ((const struct mainui_font_entry *)b)->file);
}

// A TrueType or OpenType file starts with 00 01 00 00, "true", "OTTO" or, for a collection, "ttcf".
static int font_signature(const char *path) {
    unsigned char head[4];
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    size_t n = fread(head, 1, sizeof(head), file);
    fclose(file);
    return n == 4 && (memcmp(head, "\0\1\0\0", 4) == 0 || memcmp(head, "true", 4) == 0 || memcmp(head, "OTTO", 4) == 0 ||
                      memcmp(head, "ttcf", 4) == 0);
}

int mainui_appearance_fonts(const struct mainui_fs *fs, struct mainui_font_entry **fonts) {
    *fonts = NULL;
    char directory[1024], path[1300];
    if (snprintf(directory, sizeof(directory), "%s%s", fs && fs->root ? fs->root : "", MAINUI_FONTS_DIR) >= (int)sizeof(directory))
        return 0;
    char (*names)[256] = malloc(sizeof(*names) * 512);
    if (!names) return 0;
    int n = (fs && fs->list ? fs->list : mainui_readdir)(fs ? fs->context : NULL, directory, names, 512);
    if (n > 512) n = 512;
    struct mainui_font_entry *list = n > 0 ? calloc((size_t)n, sizeof(*list)) : NULL;
    int count = 0;
    for (int i = 0; list && i < n; ++i) {
        size_t length = strlen(names[i]);
        if (!mainui_appearance_font_name_valid(names[i], length) || length < 5 ||
            (strcasecmp(names[i] + length - 4, ".ttf") != 0 && strcasecmp(names[i] + length - 4, ".otf") != 0)) continue;
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > MAINUI_FONT_FILE_BYTES ||
            !font_signature(path)) continue;
        memcpy(list[count].file, names[i], length + 1);
        memcpy(list[count].name, names[i], length - 4);
        list[count].name[length - 4] = 0;
        ++count;
    }
    free(names);
    if (!count) {
        free(list);
        return 0;
    }
    qsort(list, (size_t)count, sizeof(*list), compare_fonts);
    *fonts = list;
    return count;
}

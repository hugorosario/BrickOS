#include "names.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int mainui_names_open(struct mainui_names *names, const char *root) {
    if (!names) return -1;
    memset(names, 0, sizeof(*names));
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/usr/trimui/lib/libgamename.so", root ? root : "") >= (int)sizeof(path)) return -1;
    names->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!names->handle) return 0;
    void *symbol = dlsym(names->handle, "_Z11GetGameNamePKcS0_");
    _Static_assert(sizeof(names->lookup) == sizeof(symbol), "POSIX function pointer representation");
    memcpy(&names->lookup, &symbol, sizeof(symbol));
    if (!names->lookup) { mainui_names_close(names); return 0; }
    return 1;
}

void mainui_names_close(struct mainui_names *names) {
    if (!names) return;
    if (names->handle) dlclose(names->handle);
    memset(names, 0, sizeof(*names));
}

const char *mainui_names_get(const struct mainui_names *names, const char *language, const char *filename) {
    if (!names || !names->lookup || !filename) return NULL;
    char basename[128];
    size_t length = strlen(filename);
    if (length >= sizeof(basename)) length = sizeof(basename) - 1;
    memcpy(basename, filename, length);
    basename[length] = 0;
    char *dot = strrchr(basename, '.');
    if (dot) *dot = 0;
    return mainui_names_label(names, language, basename);
}

const char *mainui_names_label(const struct mainui_names *names, const char *language, const char *basename) {
    if (!names || !names->lookup || !basename) return NULL;
    return names->lookup(language && language[0] ? language : "en.lang", basename);
}

#define _DEFAULT_SOURCE
#include "presentation.h"
#include "../mainui/brickos_prefs.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int choice(const char *name) {
    if (!name) return -1;
    if (!strcmp(name, "material-you")) return 1;
    return !strcmp(name, "stock") ? 0 : -1;
}

int tmenu_material_selected(const char *preferences, const char *current, const char *override) {
    int explicit = choice(override);
    if (explicit >= 0) return explicit;
    struct mainui_brickos_preferences saved;
    if (preferences && mainui_brickos_preferences_load(&saved, preferences) == 0 && saved.presentation_present)
        return saved.presentation == MAINUI_PRESENTATION_MATERIAL_YOU;
    FILE *file = current ? fopen(current, "r") : NULL;
    if (!file) return 0;
    char line[512], version[65], binary[65], wrapper[65], name[32], extra[2];
    int fields = fgets(line, sizeof(line), file) ? sscanf(line, "%64s %64s %64s %31s %1s",
                        version, binary, wrapper, name, extra) : 0;
    fclose(file);
    return fields == 4 && choice(name) == 1;
}

int tmenu_emulationstation_running(const char *proc) {
    DIR *tree = proc ? opendir(proc) : NULL;
    if (!tree) return 0;
    int running = 0;
    struct dirent *entry;
    while (!running && (entry = readdir(tree)) != NULL) {
        const char *pid = entry->d_name;
        if (!*pid || strspn(pid, "0123456789") != strlen(pid)) continue;
        char link[512], program[512];
        if (snprintf(link, sizeof(link), "%s/%s/exe", proc, pid) >= (int)sizeof(link)) continue;
        ssize_t length = readlink(link, program, sizeof(program) - 1);
        if (length <= 0) continue;
        program[length] = 0;
        // A program replaced while it runs reads with this suffix.
        running = !strcmp(program, TMENU_EMULATIONSTATION) || !strcmp(program, TMENU_EMULATIONSTATION " (deleted)");
    }
    closedir(tree);
    return running;
}

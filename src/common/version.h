#ifndef BRICKOS_COMPONENT_VERSION_H
#define BRICKOS_COMPONENT_VERSION_H
#include "versions.generated.h"
#include <stdio.h>
#include <string.h>

// A private ELF section: no extra public library symbols and no dependency on Git or the SD.
// Only the component version is embedded, so a release-only bump leaves other payloads intact.
#define BRICKOS_DECLARE_COMPONENT(id, version) \
    static const char brickos_component_version[] __attribute__((used, section(".brickos.version"))) = \
        "BrickOS ComponentVersion/1\ncomponent=" id "\nversion=" version "\n"
static inline int brickos_version_option(int argc, char **argv, const char *record) {
    if (argc != 2 || strcmp(argv[1], "--version")) return 0;
    fputs(record, stdout); return 1;
}
#define BRICKOS_HANDLE_VERSION(argc, argv) \
    do { if (brickos_version_option(argc, argv, brickos_component_version)) return 0; } while (0)
#endif

#ifndef BRICKOS_UPDATE_VERSIONS_H
#define BRICKOS_UPDATE_VERSIONS_H
#include "update.h"
#define UPDATE_COMPONENTS 10
extern const char *const update_component_ids[UPDATE_COMPONENTS];
struct update_versions {
    char release[64];
    char version[UPDATE_COMPONENTS][64];
    char fingerprint[UPDATE_COMPONENTS][65];
};
int update_versions_load(const char *path, struct update_versions *versions);
int update_versions_stage(const char *directory, const char *installed, const struct update_release *release);
#endif

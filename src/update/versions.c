#define _POSIX_C_SOURCE 200809L
#include "versions.h"
#include "../common/semver.h"
#include "../mainui/json_tree.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

const char *const update_component_ids[UPDATE_COMPONENTS]={
    "mainui","inputd","osdd","ledd","tmenu","gameswitcher","uninstall","updater","setup","assets"
};
// Schema 1 with the ten known components, each with a version and a fingerprint. A later release
// may add components: those are accepted and not compared, so an installed updater still takes it.
int update_versions_load(const char *path,struct update_versions *versions) {
    struct mainui_json_tree tree;char error[160];memset(versions,0,sizeof(*versions));
    if(mainui_json_tree_load(&tree,path,error,sizeof(error)))return update_error("Invalid component versions: %s",error);
    int result=-1;struct brickos_semver parsed;
    const struct mainui_json_value *schema=mainui_json_tree_get(tree.root,"schema"),
        *components=mainui_json_tree_get(tree.root,"components");
    const char *release=mainui_json_tree_string(tree.root,"release");
    if(!schema || schema->type!=MAINUI_JSON_TREE_NUMBER || schema->number!=1 ||
        !components || components->type!=MAINUI_JSON_TREE_OBJECT || components->length<UPDATE_COMPONENTS ||
        brickos_semver_parse(release,&parsed))goto done;
    strcpy(versions->release,release);
    for(int i=0;i<UPDATE_COMPONENTS;++i) {
        const struct mainui_json_value *component=mainui_json_tree_get(components,update_component_ids[i]);
        const char *version=mainui_json_tree_string(component,"version"),*hash=mainui_json_tree_string(component,"fingerprint");
        if(brickos_semver_parse(version,&parsed) || !hash || !update_hex(hash,64))goto done;
        strcpy(versions->version[i],version);strcpy(versions->fingerprint[i],hash);
    }
    result=0;
done:
    mainui_json_tree_free(&tree);return result?update_error("Malformed component version manifest"):0;
}
// INSTALLED is the installed metadata, or NULL for a legacy install. On the device only downgrades
// are refused: a card running a development package may hold other bytes under the same component
// versions, and the rule that a changed component needs a newer version is checked when the release
// is packaged against the previous one (tools/release-package.mjs --previous).
int update_versions_stage(const char *directory,const char *installed,const struct update_release *release) {
    char path[UPDATE_PATH];struct update_versions next,old;
    if(update_path(path,"%s/BrickOS/package/%s/versions.json",directory,release->build) || update_versions_load(path,&next))return -1;
    if(strcmp(next.release,release->version))return update_error("GitHub tag does not match the package release version");
    if(!installed)return 0;
    struct stat st;
    if(update_safe_path(installed))return -1;
    if(lstat(installed,&st))return errno==ENOENT?0:-1; // A genuinely absent legacy manifest.
    if(!S_ISREG(st.st_mode))return update_error("Installed component metadata is not a regular file");
    if(update_versions_load(installed,&old))return -1;
    int order;if(brickos_semver_compare(next.release,old.release,&order) || order<0)return update_error("Release downgrade refused");
    for(int i=0;i<UPDATE_COMPONENTS;++i) {
        if(brickos_semver_compare(next.version[i],old.version[i],&order) || order<0)
            return update_error("Component downgrade refused: %s",update_component_ids[i]);
    }
    return 0;
}

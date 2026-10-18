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
int update_versions_load(const char *path,struct update_versions *versions) {
    struct mainui_json_tree tree;char error[160];memset(versions,0,sizeof(*versions));
    if(mainui_json_tree_load(&tree,path,error,sizeof(error)))return update_error("Invalid component versions: %s",error);
    int result=-1;struct brickos_semver parsed;
    const struct mainui_json_value *schema=mainui_json_tree_get(tree.root,"schema"),
        *components=mainui_json_tree_get(tree.root,"components");
    const char *release=mainui_json_tree_string(tree.root,"release");
    if(!schema || schema->type!=MAINUI_JSON_TREE_NUMBER || schema->number!=1 ||
        !components || components->type!=MAINUI_JSON_TREE_OBJECT || components->length!=UPDATE_COMPONENTS ||
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
int update_versions_stage(const char *directory,const char *sd,const struct update_release *release) {
    char path[UPDATE_PATH];struct update_versions next,old;
    if(update_path(path,"%s/BrickOS/package/%s/versions.json",directory,release->build) || update_versions_load(path,&next))return -1;
    if(strcmp(next.release,release->version))return update_error("GitHub tag does not match the package release version");
    if(update_path(path,"%s/BrickOS/versions.json",sd))return -1;
    struct stat st;
    if(update_safe_path(path))return -1;
    if(lstat(path,&st))return errno==ENOENT?0:-1; // A genuinely absent legacy manifest.
    if(!S_ISREG(st.st_mode))return update_error("Installed component metadata is not a regular file");
    if(update_versions_load(path,&old))return -1;
    int order;if(brickos_semver_compare(next.release,old.release,&order) || order<0)return update_error("Release downgrade refused");
    for(int i=0;i<UPDATE_COMPONENTS;++i) {
        if(brickos_semver_compare(next.version[i],old.version[i],&order) || order<0)
            return update_error("Component downgrade refused: %s",update_component_ids[i]);
        if(!order && strcmp(next.fingerprint[i],old.fingerprint[i]))
            return update_error("Changed component has no version bump: %s",update_component_ids[i]);
    }
    return 0;
}

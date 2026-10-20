#include "update.h"
#include "../mainui/json_tree.h"
#include "../common/semver.h"
#include <stdio.h>
#include <string.h>

static int text(const struct mainui_json_value *object, const char *key, char *out, size_t size) {
    const char *value=mainui_json_tree_string(object,key);
    if(!value || strlen(value)>=size)return -1;
    strcpy(out,value);return 0;
}
static int integer(const struct mainui_json_value *object,const char *key,uint64_t *out) {
    const struct mainui_json_value *v=mainui_json_tree_get(object,key);
    if(!v || v->type!=MAINUI_JSON_TREE_NUMBER || v->number<1 || v->number>9007199254740991.0)return -1;
    *out=(uint64_t)v->number;return (double)*out==v->number?0:-1;
}
int update_parse_release(const char *path, struct update_release *release) {
    struct mainui_json_tree tree;char error[160];memset(release,0,sizeof(*release));
    if(mainui_json_tree_load(&tree,path,error,sizeof(error)))return update_error("Invalid release metadata: %s",error);
    const struct mainui_json_value *root=tree.root,*draft=mainui_json_tree_get(root,"draft"),
        *pre=mainui_json_tree_get(root,"prerelease"),*assets=mainui_json_tree_get(root,"assets");
    int result=-1,found=0;
    if(!draft || draft->type!=MAINUI_JSON_TREE_FALSE || !pre || pre->type!=MAINUI_JSON_TREE_FALSE ||
        !assets || assets->type!=MAINUI_JSON_TREE_ARRAY || text(root,"tag_name",release->tag,sizeof(release->tag)) ||
        !release->tag[0] || strspn(release->tag,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-")!=strlen(release->tag) ||
        !strcmp(release->tag,".") || !strcmp(release->tag,".."))goto done;
    const char *version=release->tag+(release->tag[0]=='v');struct brickos_semver parsed;
    if(brickos_semver_parse(version,&parsed) || parsed.pre[0])goto done;
    strcpy(release->version,version);
    for(const struct mainui_json_value *asset=assets->first;asset;asset=asset->next) {
        const char *name=mainui_json_tree_string(asset,"name");
        if(!name || strlen(name)!=24 || strncmp(name,"brickos-",8) || strcmp(name+20,".zip"))continue;
        memcpy(release->build,name+8,12);release->build[12]=0;
        if(!update_hex(release->build,12))continue;
        if(++found!=1)goto done;
        char digest[80];const char *state=mainui_json_tree_string(asset,"state");
        if(!state || strcmp(state,"uploaded") || integer(asset,"id",&release->id) || integer(asset,"size",&release->size) ||
            release->size>UPDATE_MAX_ZIP || text(asset,"digest",digest,sizeof(digest)) || strncmp(digest,"sha256:",7) ||
            !update_hex(digest+7,64) || text(asset,"browser_download_url",release->url,sizeof(release->url)))goto done;
        strcpy(release->digest,digest+7);
        // Exactly this repository's download path for this tag and name, so no dot segment or
        // other path can follow the prefix. GitHub writes a '+' of the tag as %2B.
        char expected[sizeof(release->url)],encoded[3*sizeof(release->tag)];size_t at=0;
        for(const char *c=release->tag;*c;++c){if(*c=='+'){memcpy(encoded+at,"%2B",3);at+=3;}else encoded[at++]=*c;}
        encoded[at]=0;
        int plain=snprintf(expected,sizeof(expected),"%s%s/%s",UPDATE_DOWNLOAD,release->tag,name);
        if(plain<0 || (size_t)plain>=sizeof(expected))goto done;
        if(strcmp(release->url,expected)) {
            int escaped=snprintf(expected,sizeof(expected),"%s%s/%s",UPDATE_DOWNLOAD,encoded,name);
            if(escaped<0 || (size_t)escaped>=sizeof(expected) || strcmp(release->url,expected))goto done;
        }
    }
    if(found==1)result=0;
done:
    mainui_json_tree_free(&tree);
    return result?update_error("No unambiguous stable BrickOS package with a SHA-256 digest was found"):0;
}

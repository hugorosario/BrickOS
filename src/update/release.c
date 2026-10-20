#include "update.h"
#include "../mainui/json_tree.h"
#include "../common/semver.h"
#include <stdio.h>
#include <stdlib.h>
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

char *update_release_body(const char *path) {
    struct mainui_json_tree tree;char error[160];
    if(mainui_json_tree_load(&tree,path,error,sizeof(error)))return NULL;
    const char *body=mainui_json_tree_string(tree.root,"body");
    char *copy=body?malloc(strlen(body)+1):NULL;
    if(copy)strcpy(copy,body);
    mainui_json_tree_free(&tree);
    return copy;
}

// The length of the valid UTF-8 character at s, with n bytes left, or 0 when there is none.
static size_t utf8_length(const unsigned char *s,size_t n) {
    if(s[0]<0x80)return 1;
    size_t length=s[0]>=0xC2 && s[0]<=0xDF?2:s[0]>=0xE0 && s[0]<=0xEF?3:s[0]>=0xF0 && s[0]<=0xF4?4:0;
    if(!length || length>n)return 0;
    for(size_t i=1;i<length;++i)if((s[i]&0xC0)!=0x80)return 0;
    if((s[0]==0xE0 && s[1]<0xA0) || (s[0]==0xF0 && s[1]<0x90))return 0;  // overlong
    if((s[0]==0xED && s[1]>=0xA0) || (s[0]==0xF4 && s[1]>=0x90))return 0; // surrogate, above U+10FFFF
    return length;
}

// The plain text of one changelog bullet, at most size - 1 bytes; *whole is cleared when it did
// not fit. Markdown emphasis and code markers and link targets go. So do braces and
// backslashes, because MainUI applies {{tags}} in script output and turns \n into a new line,
// and control characters. A byte that does not start a valid UTF-8 character becomes '?'.
static size_t plain(const char *in,size_t length,char *out,size_t size,int *whole) {
    const unsigned char *s=(const unsigned char *)in;size_t n=0,i=0;
    *whole=1;
    while(i<length) {
        unsigned char c=s[i];
        if(c=='_' && i+1<length && s[i+1]=='_'){i+=2;continue;}
        if(c=='*' || c=='`' || c=='[' || c=='{' || c=='}' || c=='\\' || c<0x20 || c==0x7f){++i;continue;}
        if(c==']') {
            ++i;
            if(i<length && s[i]=='(') {
                const unsigned char *end=memchr(s+i,')',length-i);
                i=end?(size_t)(end-s)+1:length;
            }
            continue;
        }
        size_t step=utf8_length(s+i,length-i);
        const unsigned char *bytes=step?s+i:(const unsigned char *)"?";
        if(n+(step?step:1)>=size){*whole=0;break;}
        memcpy(out+n,bytes,step?step:1);n+=step?step:1;i+=step?step:1;
    }
    while(n && out[n-1]==' ')--n;
    out[n]=0;
    size_t lead=0;while(out[lead]==' ')++lead;
    if(lead){memmove(out,out+lead,n-lead+1);n-=lead;}
    return n;
}

int update_changelog_lines(const char *body,char *out,size_t size,int max_lines,int *cut) {
    *cut=0;
    if(size)out[0]=0;
    const char *open=body?strstr(body,UPDATE_CHANGELOG_OPEN):NULL;
    if(!open)return -1;
    const char *start=open+strlen(UPDATE_CHANGELOG_OPEN),*close=strstr(start,UPDATE_CHANGELOG_CLOSE);
    if(!close)return -1;
    static char text[4096];
    size_t used=0;int count=0;
    for(const char *line=start;line<close;) {
        const char *end=memchr(line,'\n',(size_t)(close-line));
        if(!end)end=close;
        const char *p=line;
        while(p<end && (*p==' ' || *p=='\t'))++p;
        line=end<close?end+1:close;
        if(end-p<3 || p[0]!='-' || p[1]!=' ')continue;
        int whole;size_t length=plain(p+2,(size_t)(end-p-2),text,sizeof(text),&whole);
        if(!length)continue;
        if(count==max_lines){*cut=1;break;}
        // "- ", the text, a newline and the terminator.
        if(!whole || used+length+4>size) {
            *cut=1;
            if(used+4>=size)break;
            size_t room=size-used-4;
            if(length>room) {
                length=room;
                while(length && ((unsigned char)text[length]&0xC0)==0x80)--length;
            }
            if(!length)break;
        }
        memcpy(out+used,"- ",2);memcpy(out+used+2,text,length);
        used+=length+2;out[used++]='\n';out[used]=0;++count;
        if(*cut)break;
    }
    return count;
}

#ifndef BRICKOS_SEMVER_H
#define BRICKOS_SEMVER_H
#include <stdint.h>
#include <string.h>

// SemVer 2 precedence; storage bound 63 bytes, core fields up to nine decimal digits.
struct brickos_semver { uint32_t core[3]; char pre[64]; };
static inline int brickos_semver_identifiers(const char *text, size_t length, int prerelease) {
    size_t at=0;
    while(at<length) {
        size_t start=at;int numeric=1;
        while(at<length && text[at]!='.') {
            char c=text[at++];
            if(c<'0' || c>'9')numeric=0;
            if(!((c>='0' && c<='9') || (c>='a' && c<='z') || (c>='A' && c<='Z') || c=='-'))return -1;
        }
        if(at==start || (prerelease && numeric && at-start>1 && text[start]=='0'))return -1;
        if(at<length && ++at==length)return -1;
    }
    return length?0:-1;
}
static inline int brickos_semver_parse(const char *text, struct brickos_semver *v) {
    if(!text || !*text || strlen(text)>63)return -1;
    memset(v,0,sizeof(*v));const char *p=text;
    for(int field=0;field<3;++field) {
        const char *start=p;uint32_t number=0;
        while(*p>='0' && *p<='9') {
            if(p-start>=9)return -1;
            number=number*10+(unsigned)(*p++-'0');
        }
        if(p==start || (p-start>1 && *start=='0'))return -1;
        v->core[field]=number;
        if(field<2 && *p++!='.')return -1;
    }
    if(*p=='-') {
        const char *start=++p;while(*p && *p!='+')++p;
        size_t n=(size_t)(p-start);if(brickos_semver_identifiers(start,n,1))return -1;
        memcpy(v->pre,start,n);v->pre[n]=0;
    }
    if(*p=='+'){++p;if(brickos_semver_identifiers(p,strlen(p),0))return -1;return 0;}
    return *p?-1:0;
}
static inline int brickos_semver_compare(const char *left,const char *right,int *order) {
    struct brickos_semver a,b;if(brickos_semver_parse(left,&a) || brickos_semver_parse(right,&b))return -1;
    *order=0;
    for(int i=0;i<3;++i)if(a.core[i]!=b.core[i]){*order=a.core[i]<b.core[i]?-1:1;return 0;}
    if(!a.pre[0] || !b.pre[0]){*order=!a.pre[0] && !b.pre[0]?0:a.pre[0]?-1:1;return 0;}
    const char *x=a.pre,*y=b.pre;
    while(*x && *y) {
        size_t nx=strcspn(x,"."),ny=strcspn(y,".");
        int numeric_x=strspn(x,"0123456789")>=nx,numeric_y=strspn(y,"0123456789")>=ny;
        int result=0;
        if(numeric_x!=numeric_y)result=numeric_x?-1:1;
        else if(numeric_x && nx!=ny)result=nx<ny?-1:1;
        else {result=strncmp(x,y,nx<ny?nx:ny);if(!result && nx!=ny)result=nx<ny?-1:1;}
        if(result){*order=result<0?-1:1;return 0;}
        x+=nx;y+=ny;if(*x)++x;else if(*y){*order=-1;return 0;}
        if(*y)++y;else if(*x){*order=1;return 0;}
    }
    return 0;
}
#endif

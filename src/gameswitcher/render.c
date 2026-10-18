#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const SDL_Color white = {239,241,251,255}, muted = {165,177,195,255};
static const SDL_Color lilac = {188,165,246,255}, panel = {24,29,41,255};
static void rect(SDL_Surface *s, SDL_Rect r, SDL_Color c) {
    SDL_FillRect(s, &r, SDL_MapRGBA(s->format,c.r,c.g,c.b,c.a));
}
// How far row y of a rounded box of height h sits into its top or bottom corner; 0 between.
static int corner_row(int y, int h, int radius) {
    return y<radius ? radius-1-y : y>=h-radius ? y-(h-radius) : 0;
}
static void rounded(SDL_Surface *s, SDL_Rect r, int radius, SDL_Color c) {
    for (int y=0; y<r.h; ++y) {
        int dy=corner_row(y,r.h,radius), inset=0;
        if (!dy) {
            // The straight part between the corners is one rectangle.
            int end=y; while (end+1<r.h && !corner_row(end+1,r.h,radius)) ++end;
            rect(s,(SDL_Rect){r.x,r.y+y,r.w,end-y+1},c); y=end; continue;
        }
        int dx=radius; while (dx*dx+dy*dy>radius*radius) --dx; inset=radius-dx;
        rect(s,(SDL_Rect){r.x+inset,r.y+y,r.w-2*inset,1},c);
    }
}
// Text surfaces are kept by font, colour and text: the cards' texts are drawn on every frame
// while the cards move.
#define TEXT_CACHE 32
static struct { TTF_Font *font; SDL_Color color; char label[352]; SDL_Surface *surface; } texts[TEXT_CACHE];
static int text_next;
static void texts_clear(void) {
    for (int i=0;i<TEXT_CACHE;++i) { SDL_FreeSurface(texts[i].surface); texts[i].surface=NULL; }
    text_next=0;
}
static SDL_Surface *text_surface(TTF_Font *font, const char *label, SDL_Color c, int *owned) {
    *owned=0;
    for (int i=0;i<TEXT_CACHE;++i)
        if (texts[i].surface && texts[i].font==font && !memcmp(&texts[i].color,&c,sizeof(c)) && !strcmp(texts[i].label,label))
            return texts[i].surface;
    SDL_Surface *p=TTF_RenderUTF8_Blended(font,label,c);
    if (!p || strlen(label)>=sizeof(texts[0].label)) { *owned=p!=NULL; return p; }
    int i=text_next; text_next=(text_next+1)%TEXT_CACHE;
    SDL_FreeSurface(texts[i].surface);
    texts[i].font=font; texts[i].color=c; snprintf(texts[i].label,sizeof(texts[i].label),"%s",label); texts[i].surface=p;
    return p;
}
// center: 0 aligns the text left in the box, 1 centres it, 2 aligns it right.
static void text_alpha(SDL_Surface *s, TTF_Font *font, const char *label, SDL_Rect box, SDL_Color c, int center, Uint8 alpha) {
    if (!font || !label || !alpha) return;
    int owned; SDL_Surface *p=text_surface(font,label,c,&owned);
    if (!p) return;
    SDL_Rect old, dst={box.x+(center==2 ? box.w-p->w : center ? (box.w-p->w)/2 : 0),box.y+(box.h-p->h)/2,0,0};
    SDL_SetSurfaceAlphaMod(p,alpha);
    SDL_GetClipRect(s,&old); SDL_SetClipRect(s,&box); SDL_BlitSurface(p,NULL,s,&dst);
    SDL_SetClipRect(s,&old); SDL_SetSurfaceAlphaMod(p,255);
    if (owned) SDL_FreeSurface(p);
}
static void text(SDL_Surface *s, TTF_Font *font, const char *label, SDL_Rect box, SDL_Color c, int center) {
    text_alpha(s,font,label,box,c,center,255);
}
static const SDL_Color background = {13,18,28,255};
// The per-pixel loops of the fades: at -O3, GCC vectorizes them (NEON on the Brick).
#if defined(__GNUC__) && !defined(__clang__)
#define PIXEL_LOOP __attribute__((optimize("O3")))
#else
#define PIXEL_LOOP
#endif
// A transparent layer the size of the screen holding a removed card, drawn once per removal
// (layer_removal names it) and blended at the centre while the card fades out.
static SDL_Surface *fade_layer;
static int layer_removal=-1;
static SDL_Surface *layer_for(const SDL_Surface *s) {
    if (!fade_layer || fade_layer->w!=s->w || fade_layer->h!=s->h) {
        SDL_FreeSurface(fade_layer);
        fade_layer=SDL_CreateRGBSurfaceWithFormat(0,s->w,s->h,32,SDL_PIXELFORMAT_ARGB8888);
        if (!fade_layer) return NULL;
    }
    SDL_FillRect(fade_layer,NULL,0);
    return fade_layer;
}
// The layer's pixels within r over the screen at opacity: a pixel mixes by its own alpha times
// the opacity. On the Brick this costs about a third of SDL's blend of the whole layer.
static PIXEL_LOOP void blend_over(SDL_Surface *s, const SDL_Surface *layer, SDL_Rect r, float opacity) {
    SDL_Rect screen={0,0,s->w,s->h};
    if (opacity<=0 || !SDL_IntersectRect(&r,&screen,&r)) return;
    Uint32 k=(Uint32)lroundf(256*fminf(opacity,1.f));
    for (int y=r.y; y<r.y+r.h; ++y) {
        const Uint32 *from=(const Uint32 *)((const Uint8 *)layer->pixels+(size_t)y*layer->pitch)+r.x;
        Uint32 *to=(Uint32 *)((Uint8 *)s->pixels+(size_t)y*s->pitch)+r.x;
        for (int x=0; x<r.w; ++x) {
            // Without a branch: a transparent pixel mixes in at weight 0 and leaves the screen's.
            Uint32 c=from[x], a=c>>24, w=((a+(a>>7))*k)>>8, d=to[x];
            Uint32 rb=((c&0xff00ff)*w+(d&0xff00ff)*(256-w))>>8, g=((c&0x00ff00)*w+(d&0x00ff00)*(256-w))>>8;
            to[x]=0xff000000u|(rb&0xff00ff)|(g&0x00ff00);
        }
    }
}
// The rows top to bottom faded toward the background at opacity: the cards under a zoom, where
// only the plain background lies beneath them. Pixels within cover, drawn over next, are left.
static PIXEL_LOOP void fade_row(Uint32 *p, int from, int to, Uint32 w, Uint32 brb, Uint32 bg) {
    for (int x=from; x<to; ++x) {
        Uint32 c=p[x], rb=((c&0xff00ff)*w+brb)>>8, g=((c&0x00ff00)*w+bg)>>8;
        p[x]=0xff000000u|(rb&0xff00ff)|(g&0x00ff00);
    }
}
static void fade_band(SDL_Surface *s, int top, int bottom, float opacity, SDL_Rect cover) {
    Uint32 w=(Uint32)lroundf(256*fminf(fmaxf(opacity,0.f),1.f));
    if (w>=256) return;
    Uint32 back=SDL_MapRGBA(s->format,background.r,background.g,background.b,255);
    Uint32 brb=(back&0xff00ff)*(256-w), bg=(back&0x00ff00)*(256-w);
    int left=cover.x<0?0:cover.x, right=cover.x+cover.w>s->w?s->w:cover.x+cover.w;
    for (int y=top; y<bottom && y<s->h; ++y) {
        Uint32 *p=(Uint32 *)((Uint8 *)s->pixels+(size_t)y*s->pitch);
        if (y>=cover.y && y<cover.y+cover.h && left<right) { fade_row(p,0,left,w,brb,bg); fade_row(p,right,s->w,w,brb,bg); }
        else fade_row(p,0,s->w,w,brb,bg);
    }
}
int gs_assets_open(struct gs_assets *a, const char *font) {
    memset(a,0,sizeof(*a));
    a->title=TTF_OpenFont(font,36); a->body=TTF_OpenFont(font,28); a->small=TTF_OpenFont(font,22);
    if (!a->title || !a->body || !a->small) { gs_assets_close(a); return -1; }
    return 0;
}
void gs_assets_close(struct gs_assets *a) {
    for (int i=0;i<GS_PLACES;++i) { SDL_FreeSurface(a->images[i]); a->images[i]=NULL; }
    SDL_FreeSurface(a->removed);
    texts_clear(); SDL_FreeSurface(fade_layer); fade_layer=NULL; layer_removal=-1;
    if (a->title) TTF_CloseFont(a->title);
    if (a->body) TTF_CloseFont(a->body);
    if (a->small) TTF_CloseFont(a->small);
    memset(a,0,sizeof(*a));
}
// The entry k places from the selected one, around the ring.
static size_t position(const struct gs_model *m,int k) {
    long n=(long)m->index.count;
    return (size_t)((((long)m->selected+k)%n+n)%n);
}
static SDL_Surface *load_thumbnail(struct gs_store *s,const struct gs_entry *e) {
    int fd=gs_entry_thumbnail(s,e);
    FILE *f=fd>=0 ? fdopen(fd,"rb") : NULL;
    if (!f) { if (fd>=0) close(fd); return NULL; }
    SDL_RWops *rw=SDL_RWFromFP(f,SDL_TRUE);
    if (!rw) { fclose(f); return NULL; }
    SDL_Surface *loaded=SDL_LoadBMP_RW(rw,1);
    if (!loaded) return NULL;
    // Converted once to the screen's format, opaque: scaling it then copies rows, where SDL
    // converts every pixel of a 24-bit picture on each frame (6 ms a card on the Brick).
    SDL_Surface *image=SDL_ConvertSurfaceFormat(loaded,SDL_PIXELFORMAT_ARGB8888,0);
    SDL_FreeSurface(loaded);
    if (image) SDL_SetSurfaceBlendMode(image,SDL_BLENDMODE_NONE);
    return image;
}
SDL_Surface *(*gs_thumbnail_loader)(struct gs_store *,const struct gs_entry *)=load_thumbnail;
void gs_assets_reload(struct gs_assets *a,struct gs_store *s,const struct gs_model *m) {
    SDL_Surface *old[GS_PLACES]; char old_ids[GS_PLACES][GS_ID];
    memcpy(old,a->images,sizeof(old)); memcpy(old_ids,a->ids,sizeof(old_ids));
    memset(a->images,0,sizeof(a->images)); memset(a->ids,0,sizeof(a->ids)); memset(a->valid,0,sizeof(a->valid));
    for (int k=-2;k<=2 && m->index.count;++k) {
        int place=k+2; const struct gs_entry *e=&m->index.entries[position(m,k)];
        memcpy(a->ids[place],e->id,GS_ID); a->ids[place][GS_ID-1]=0;
        a->valid[place]=gs_entry_resumable(s,e)==0;
        // A picture still shown moves with its entry; a second place of the same entry gets a copy.
        int found=-1;
        for (int j=0;j<GS_PLACES && *e->id && found<0;++j) if (old[j] && !strcmp(old_ids[j],a->ids[place])) found=j;
        if (found>=0) { a->images[place]=old[found]; old[found]=NULL; continue; }
        for (int j=0;j<place && *e->id && found<0;++j) if (a->images[j] && !strcmp(a->ids[j],a->ids[place])) found=j;
        if (found>=0) { a->images[place]=SDL_DuplicateSurface(a->images[found]); continue; }
        a->images[place]=gs_thumbnail_loader(s,e); ++a->loads;
    }
    for (int j=0;j<GS_PLACES;++j) SDL_FreeSurface(old[j]);
    a->selected=m->selected; a->loaded=1;
}
void gs_assets_keep_removed(struct gs_assets *a,const struct gs_model *m) {
    const struct gs_entry *e=gs_model_selected(m);
    SDL_FreeSurface(a->removed);
    a->removed=a->images[2]; a->images[2]=NULL; a->ids[2][0]=0;
    a->removed_valid=a->valid[2]; ++a->removals;
    if (e) a->removed_entry=*e; else memset(&a->removed_entry,0,sizeof(a->removed_entry));
}
static void title(const char *path,char out[256]) {
    const char *base=strrchr(path,'/'); base=base?base+1:path;
    snprintf(out,256,"%s",base);
    char *dot=strrchr(out,'.'); if (dot) *dot=0;
}
static void time_label(char out[80],uint64_t ms,const char *prefix) {
    uint64_t minutes=ms/60000;
    snprintf(out,80,"%s %" PRIu64 "h %02" PRIu64 "m",prefix,minutes/60,minutes%60);
}
static void preview(SDL_Surface *s,SDL_Surface *image,SDL_Rect to) {
    rounded(s,to,16,(SDL_Color){39,46,62,255});
    if (image && image->w>0 && image->h>0) {
        double scale=fmin((double)to.w/image->w,(double)to.h/image->h);
        SDL_Rect dst={to.x,to.y,(int)(image->w*scale),(int)(image->h*scale)};
        dst.x+=(to.w-dst.w)/2; dst.y+=(to.h-dst.h)/2;
        // Pictures are opaque: without blending, a picture in the screen's format scales by rows.
        SDL_SetSurfaceBlendMode(image,SDL_BLENDMODE_NONE);
        SDL_BlitScaled(image,NULL,s,&dst);
    } else {
        // Stable monochrome gamepad placeholder, including unreadable/mismatched BMP.
        SDL_Rect pad={to.x+to.w/2-62,to.y+to.h/2-34,124,68};
        rounded(s,pad,22,muted);
        SDL_Color dark={39,46,62,255};
        rect(s,(SDL_Rect){pad.x+26,pad.y+20,8,28},dark);
        rect(s,(SDL_Rect){pad.x+16,pad.y+30,28,8},dark);
        rounded(s,(SDL_Rect){pad.x+85,pad.y+18,10,10},5,dark);
        rounded(s,(SDL_Rect){pad.x+101,pad.y+34,10,10},5,dark);
    }
}
// A card d places from the centre: the centre card (620x488 at 202, 142, with a 580x324 picture)
// at 0, the side cards (620x420 at 202 -/+ 658, 175, with 580x280 pictures) at -1 and 1, and in
// between while the cards slide. Past one place a card keeps the side size, off the screen.
struct card_box { int x, y, h, image_h; };
static struct card_box card_box(float d) {
    float off=fminf(fabsf(d),1.f);
    return (struct card_box){(int)lroundf(202+658*d),(int)lroundf(142+33*off),(int)lroundf(488-68*off),(int)lroundf(324-44*off)};
}
static void draw_card(SDL_Surface *s,const struct gs_assets *a,const struct gs_model *m,const struct gs_entry *e,
                      SDL_Surface *image,int valid,float d,uint64_t now) {
    struct card_box b=card_box(d);
    int x=b.x, centre=fabsf(d)<0.5f;
    SDL_Rect card={x,b.y,620,b.h};
    if (centre) {
        int pulse=(int)(5+3*sin((double)(now%2400)*6.283185307/2400));
        rounded(s,(SDL_Rect){x-pulse,card.y-pulse,card.w+2*pulse,card.h+2*pulse},32,(SDL_Color){69,56,94,255});
        rounded(s,(SDL_Rect){x-2,card.y-2,card.w+4,card.h+4},27,lilac);
    }
    rounded(s,card,25,panel);
    preview(s,image,(SDL_Rect){x+20,card.y+20,580,b.image_h});
    if (fabsf(d)>=1.5f) return;
    char name[256],core[256],context[340]; title(e->identity.rom,name); title(e->identity.core,core);
    char *suffix=strstr(core,"_libretro"); if (suffix && !suffix[9]) *suffix=0;
    const char *start=e->identity.rom; const char *system=strstr(start,"/Roms/");
    system=system ? system+6 : ""; const char *slash=strchr(system,'/');
    snprintf(context,sizeof(context),"%.*s  ·  %s",slash?(int)(slash-system):0,system,core);
    text(s,a->body,name,(SDL_Rect){x+30,card.y+b.image_h+31,560,44},white,0);
    text(s,a->small,context,(SDL_Rect){x+30,card.y+b.image_h+76,560,36},muted,0);
    if (centre) {
        char detail[80]; time_label(detail,e->play_ms,"Played");
        const char *label=valid ? (m->show_time?detail:"A  Resume game") : "Checkpoint unavailable";
        text(s,a->small,label,(SDL_Rect){x+30,card.y+card.h-44,560,30},valid?lilac:muted,0);
    }
}
// The cards around the selected one, shift places off their places while they slide (0 at rest),
// the far ones first so that the nearer ones overlap them, and the page.
static void carousel(SDL_Surface *s,const struct gs_assets *a,const struct gs_model *m,float shift,uint64_t now) {
    size_t n=m->index.count;
    int reach=shift!=0 && n>=3 ? 2 : 1, places[GS_PLACES], count=0;
    for (int k=-reach;k<=reach;++k) if (n>1 || !k) places[count++]=k;
    for (int i=0;i<count;++i)
        for (int j=i+1;j<count;++j)
            if (fabsf(places[j]+shift)>fabsf(places[i]+shift)) { int t=places[i]; places[i]=places[j]; places[j]=t; }
    for (int i=0;i<count;++i) {
        int k=places[i]; float d=k+shift;
        if (fabsf(d)>2.2f) continue;
        draw_card(s,a,m,&m->index.entries[position(m,k)],a->images[k+2],a->valid[k+2],d,now);
    }
}
// The n / N counter under the cards. It shares the bottom of the screen with the fullscreen title.
static void page_counter(SDL_Surface *s,const struct gs_assets *a,const struct gs_model *m,Uint8 alpha) {
    char page[80]; snprintf(page,sizeof(page),"%zu / %zu",m->selected+1,m->index.count);
    text_alpha(s,a->small,page,(SDL_Rect){414,638,196,42},muted,1,alpha);
}
static void fullscreen_title(SDL_Surface *s,const struct gs_assets *a,const struct gs_model *m,Uint8 alpha) {
    char name[256]; title(gs_model_selected(m)->identity.rom,name);
    text_alpha(s,a->body,name,(SDL_Rect){40,650,944,42},white,1,alpha);
}
void gs_render(SDL_Surface *s,const struct gs_assets *a,const struct gs_model *m,uint64_t now) {
    gs_render_at(s,a,m,NULL,now);
}
void gs_render_at(SDL_Surface *s,const struct gs_assets *a,const struct gs_model *m,const struct gs_motion *motion,uint64_t now) {
    float shift=motion ? gs_motion_shift(motion,now) : 0;
    float zoom=motion ? gs_motion_zoom_value(motion,now) : m->fullscreen ? 1.f : 0.f;
    float removed=motion ? gs_motion_removed(motion,now) : 0;
    rect(s,(SDL_Rect){0,0,1024,768},background);
    text(s,a->title,"GameSwitcher",(SDL_Rect){40,25,550,65},white,0);
    // The battery level, at the top right.
    char status[16];
    if (m->battery>=0) snprintf(status,sizeof(status),"%d%%",m->battery);
    else snprintf(status,sizeof(status),"--%%");
    text(s,a->small,status,(SDL_Rect){636,38,348,40},muted,2);
    if (!m->index.count) {
        preview(s,NULL,(SDL_Rect){332,200,360,210});
        text(s,a->body,"No checkpoints yet",(SDL_Rect){140,435,744,52},white,1);
        text(s,a->small,"Saved games will appear here",(SDL_Rect){140,494,744,44},muted,1);
    } else if (zoom>=1) {
        preview(s,a->images[2],(SDL_Rect){28,104,968,544});
        fullscreen_title(s,a,m,255);
    } else if (zoom<=0) {
        carousel(s,a,m,shift,now);
        page_counter(s,a,m,255);
    } else {
        // The zoom: the cards and their texts fade out toward the background under the centre
        // picture, which grows from the card's picture area to the fullscreen preview. The cards'
        // rows hold only the cards over the plain background. The counter is gone by halfway,
        // where the title, in the same place, starts to fade in.
        SDL_Rect from={222,162,580,324}, to={28,104,968,544};
        SDL_Rect at={(int)lroundf(from.x+(to.x-from.x)*zoom),(int)lroundf(from.y+(to.y-from.y)*zoom),
                     (int)lroundf(from.w+(to.w-from.w)*zoom),(int)lroundf(from.h+(to.h-from.h)*zoom)};
        carousel(s,a,m,shift,now);
        // The picture's rounded box covers all but its corners, which still fade.
        fade_band(s,120,690,1-zoom,(SDL_Rect){at.x,at.y+16,at.w,at.h-32});
        page_counter(s,a,m,(Uint8)lroundf(255*fmaxf(0.f,1-2*zoom)));
        preview(s,a->images[2],at);
        fullscreen_title(s,a,m,(Uint8)lroundf(255*fmaxf(0.f,2*zoom-1)));
    }
    if (removed>0) {
        // A removed card fades out at the centre, over the card coming in. It is drawn once,
        // when its fade starts, and blended within its reach (the card and its pulse outline).
        if (layer_removal!=a->removals || !fade_layer) {
            SDL_Surface *layer=layer_for(s);
            if (layer) { draw_card(layer,a,m,&a->removed_entry,a->removed,a->removed_valid,0,now); layer_removal=a->removals; }
        }
        if (fade_layer && layer_removal==a->removals) blend_over(s,fade_layer,(SDL_Rect){190,130,644,512},removed);
    }
    if (*m->message) text(s,a->small,m->message,(SDL_Rect){40,688,944,35},(SDL_Color){255,154,146,255},1);
    else if (m->show_time) {
        char total[80]; time_label(total,gs_model_total(m),"Total");
        text(s,a->small,total,(SDL_Rect){40,688,944,35},muted,1);
    }
    text(s,a->small,m->index.count?"B  Back     X  Remove     Y  Fullscreen     SELECT  Time":"B  Back",
         (SDL_Rect){40,729,944,32},white,1);
}

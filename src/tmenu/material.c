#include "material.h"
#include <SDL_image.h>
#include <stdio.h>
#include <stdlib.h>

static const SDL_Color white = {239, 241, 251, 255}, muted = {165, 177, 195, 255};
static const SDL_Color lilac = {188, 165, 246, 255}, selected = {31, 23, 49, 255};
static const SDL_Color red = {255, 116, 110, 255};

static void rect(SDL_Surface *s, int x, int y, int w, int h, SDL_Color c) {
    SDL_Rect r = {x,y,w,h};
    SDL_FillRect(s, &r, SDL_MapRGBA(s->format,c.r,c.g,c.b,c.a));
}

// Alpha surfaces keep the paused frame visible beneath the scrim and panel.
static void rounded(SDL_Surface *s, SDL_Rect to, int radius, SDL_Color c) {
    SDL_Surface *p = SDL_CreateRGBSurfaceWithFormat(0,to.w,to.h,32,SDL_PIXELFORMAT_ARGB8888);
    if (!p) return;
    SDL_FillRect(p,NULL,0);
    for (int y=0; y<to.h; ++y) {
        int inset=0, dy=y<radius ? radius-1-y : y>=to.h-radius ? y-(to.h-radius) : 0;
        if (dy) {
            int dx=radius;
            while (dx>0 && dx*dx+dy*dy>radius*radius) --dx;
            inset=radius-dx;
        }
        rect(p,inset,y,to.w-2*inset,1,c);
    }
    SDL_SetSurfaceBlendMode(p,SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(p,NULL,s,&to);
    SDL_FreeSurface(p);
}

static void text(SDL_Surface *s, TTF_Font *font, const char *label, SDL_Rect box, SDL_Color c, int center) {
    if (!font || !label) return;
    SDL_Surface *p=TTF_RenderUTF8_Blended(font,label,c);
    if (!p) return;
    SDL_Rect old, dst={box.x+(center ? (box.w-p->w)/2 : 0),box.y+(box.h-p->h)/2,0,0};
    SDL_GetClipRect(s,&old);
    SDL_SetClipRect(s,&box);
    SDL_BlitSurface(p,NULL,s,&dst);
    SDL_SetClipRect(s,&old);
    SDL_FreeSurface(p);
}

static void disk(SDL_Surface *s,int cx,int cy,int radius,SDL_Color c) {
    for (int y=-radius;y<=radius;++y) {
        int x=radius;
        while (x*x+y*y>radius*radius) --x;
        rect(s,cx-x,cy+y,2*x+1,1,c);
    }
}

static void line(SDL_Surface *s,int x,int y,int tx,int ty,int width,SDL_Color c) {
    int dx=abs(tx-x), sx=x<tx?1:-1, dy=-abs(ty-y), sy=y<ty?1:-1, error=dx+dy;
    for (;;) {
        disk(s,x,y,width/2,c);
        if (x==tx && y==ty) break;
        int e=error*2;
        if (e>=dy) { error+=dy; x+=sx; }
        if (e<=dx) { error+=dx; y+=sy; }
    }
}

// Small vector masks give every action its own monochrome glyph at any theme color.
static void icon(SDL_Surface *s,int item,int x,int y,SDL_Color c) {
    SDL_Surface *p=SDL_CreateRGBSurfaceWithFormat(0,44,44,32,SDL_PIXELFORMAT_ARGB8888);
    if (!p) return;
    SDL_Color clear={0,0,0,0};
    SDL_FillRect(p,NULL,0);
    switch (item) {
    case TMENU_ITEM_CONTINUE:
        for (int i=0;i<29;++i) rect(p,12,8+i,1+(i<15?i:28-i)*3/2,1,c);
        break;
    case TMENU_ITEM_SAVE:
        rect(p,8,6,28,32,c); rect(p,12,10,20,24,clear);
        rect(p,14,6,15,12,c); rect(p,23,9,3,6,clear);
        rect(p,14,24,16,14,c); rect(p,18,28,8,6,clear); break;
    case TMENU_ITEM_LOAD:
        line(p,22,5,22,26,4,c); line(p,13,18,22,27,4,c); line(p,31,18,22,27,4,c);
        line(p,8,29,8,37,4,c); line(p,8,37,36,37,4,c); line(p,36,37,36,29,4,c); break;
    case TMENU_ITEM_ADVANCED:
        disk(p,22,22,14,c);
        rect(p,18,3,8,38,c); rect(p,3,18,38,8,c);
        line(p,9,9,35,35,7,c); line(p,9,35,35,9,7,c);
        disk(p,22,22,7,clear); break;
    case TMENU_ITEM_NETPLAY:
        // Network nodes and links, rather than a second settings glyph.
        line(p,22,10,10,32,3,c); line(p,22,10,34,32,3,c); line(p,10,32,34,32,3,c);
        disk(p,22,10,6,c); disk(p,10,32,6,c); disk(p,34,32,6,c); break;
    case TMENU_ITEM_FULLSCREEN:
        line(p,7,17,7,7,4,c); line(p,7,7,17,7,4,c);
        line(p,27,7,37,7,4,c); line(p,37,7,37,17,4,c);
        line(p,7,27,7,37,4,c); line(p,7,37,17,37,4,c);
        line(p,27,37,37,37,4,c); line(p,37,37,37,27,4,c); break;
    case TMENU_ITEM_EXIT:
        disk(p,22,22,17,c); disk(p,22,22,12,clear);
        rect(p,24,8,20,22,clear);
        line(p,23,22,39,22,4,c); line(p,33,16,39,22,4,c); line(p,33,28,39,22,4,c); break;
    default:
        rect(p,7,17,8,12,c);
        for(int i=0;i<20;++i) rect(p,15,12+i,1+(i<10?i:19-i),1,c);
        line(p,34,14,38,22,3,c); line(p,38,22,34,30,3,c); break;
    }
    SDL_SetSurfaceBlendMode(p,SDL_BLENDMODE_BLEND);
    SDL_Rect dst={x,y,44,44}; SDL_BlitSurface(p,NULL,s,&dst); SDL_FreeSurface(p);
}

static const char *label(const struct tmenu_assets *a,const struct tmenu_menu *m,int item) {
    static const char *en[TMENU_ITEMS]={"Continue","Save state","Load state","Volume","Advanced menu","Netplay","Fullscreen","Exit to menu"};
    static const char *pt[TMENU_ITEMS]={"Continuar","Guardar estado","Carregar estado","Volume","Menu avançado","Netplay","Ecrã completo","Sair para o menu"};
    if(a->portuguese) return pt[item];
    if(a->english) return en[item];
    return m->strings->items[item];
}

static void hint(SDL_Surface *s,TTF_Font *font,TTF_Font *small,int x,const char *key,const char *name) {
    (void)small;
    disk(s,x+18,720,18,muted); disk(s,x+18,720,15,white);
    text(s,font,key,(SDL_Rect){x,700,36,40},selected,1);
    text(s,font,name,(SDL_Rect){x+48,695,205,50},white,0);
}

static void slots(SDL_Surface *s,TTF_Font *font,TTF_Font *small,const struct tmenu_assets *a,
                  struct tmenu_menu *m,tmenu_thumb_path_fn thumb_path) {
    int slot=m->screen==TMENU_SCREEN_SAVE ? m->save_index : m->saved[m->load_index];
    rounded(s,(SDL_Rect){222,180,580,404},20,(SDL_Color){64,54,88,255});
    rounded(s,(SDL_Rect){224,182,576,400},18,(SDL_Color){28,38,51,255});
    SDL_Rect area={238,200,548,310};
    int preview=0;
    if(slot>=0 && slot<m->slot_count && m->slot_saved[slot] && thumb_path) {
        char *path=thumb_path(slot);
        SDL_Surface *image=path ? IMG_Load(path) : NULL;
        free(path);
        if(image && image->w>0 && image->h>0) {
            int w=area.w,h=(int)((long)image->h*w/image->w);
            if(h>area.h) { h=area.h; w=(int)((long)image->w*h/image->h); }
            SDL_Rect dst={area.x+(area.w-w)/2,area.y+(area.h-h)/2,w,h};
            SDL_BlitScaled(image,NULL,s,&dst); preview=1;
        }
        if(image) SDL_FreeSurface(image);
    }
    if(!preview) {
        icon(s,m->screen==TMENU_SCREEN_SAVE ? TMENU_ITEM_SAVE : TMENU_ITEM_LOAD,490,300,muted);
        text(s,font, slot>=0 && slot<m->slot_count && m->slot_saved[slot] ? (a->portuguese?"Sem pré-visualização":"No preview") :
             (a->portuguese?"Slot vazio":"Empty slot"),(SDL_Rect){238,365,548,55},muted,1);
    }
    char number[64]; snprintf(number,sizeof(number),"Slot %d / %d",slot+1,m->slot_count);
    text(s,font,number,(SDL_Rect){238,518,548,54},white,1);
    for(int y=-6;y<=6;++y) {
        int w=7-abs(y);
        rect(s,450-w,608+y,w,1,muted);
        rect(s,476,608+y,w,1,muted);
    }
    text(s,small,a->portuguese?"Escolher slot":"Choose slot",(SDL_Rect){496,590,190,36},muted,0);
}

void tmenu_render_material(SDL_Surface *s,SDL_Surface *background,const struct tmenu_assets *a,
                           const struct tmenu_theme *theme,struct tmenu_menu *m,tmenu_thumb_path_fn thumb_path) {
    (void)theme;
    TTF_Font *font=a->font, *small=a->small_font;
    SDL_FillRect(s,NULL,SDL_MapRGB(s->format,0,0,0));
    if(background) { SDL_Rect dst={(1024-background->w)/2,(768-background->h)/2,0,0}; SDL_BlitSurface(background,NULL,s,&dst); }
    rounded(s,(SDL_Rect){0,0,1024,768},0,(SDL_Color){0,0,0,110});
    rounded(s,(SDL_Rect){190,78,644,604},28,(SDL_Color){0,0,0,75});
    rounded(s,(SDL_Rect){196,84,632,592},24,(SDL_Color){60,70,90,245});
    rounded(s,(SDL_Rect){198,86,628,588},22,(SDL_Color){19,28,39,247});
    const char *heading=m->screen==TMENU_SCREEN_MAIN ? (a->portuguese?"Menu rápido":"Quick menu") :
         label(a,m,m->screen==TMENU_SCREEN_SAVE ? TMENU_ITEM_SAVE : TMENU_ITEM_LOAD);
    text(s,font,heading,(SDL_Rect){228,106,510,50},white,0);
    icon(s,TMENU_ITEM_ADVANCED,758,109,muted);
    int confirm=1, change=0;
    if(m->screen==TMENU_SCREEN_MAIN) {
        int row=0,total=0,preceding=0;
        for(int item=0;item<TMENU_ITEMS;++item) if(m->visible[item]) { ++total; if(item<m->first) ++preceding; }
        for(int item=m->first;item<TMENU_ITEMS && row<6;++item) {
            if(!m->visible[item]) continue;
            int y=174+row*78, focus=m->focus==item;
            SDL_Color c=focus ? selected : item==TMENU_ITEM_EXIT ? red : white;
            if(item==TMENU_ITEM_LOAD && !m->saved_count && !focus) c=muted;
            if(focus) {
                rounded(s,(SDL_Rect){214,y-2,596,74},22,(SDL_Color){220,205,255,255});
                rounded(s,(SDL_Rect){216,y,592,70},20,lilac);
                if(item==TMENU_ITEM_EXIT) c=(SDL_Color){118,27,34,255};
            } else if(row>0) rect(s,238,y-5,548,1,(SDL_Color){45,56,72,255});
            icon(s,item,250,y+13,c);
            text(s,font,label(a,m,item),(SDL_Rect){328,y,item==TMENU_ITEM_FULLSCREEN || item==TMENU_ITEM_VOLUME ? 350 : 450,70},c,0);
            if(item==TMENU_ITEM_FULLSCREEN || item==TMENU_ITEM_VOLUME) {
                const char *value=m->fullscreen_on ? m->strings->on : m->strings->off;
                char volume[32];
                if(item==TMENU_ITEM_VOLUME) { snprintf(volume,sizeof(volume),"%d / 20",m->ops.keyshm_get(m->ops.context,0)); value=volume; }
                text(s,small,value,(SDL_Rect){685,y,100,70},c,1);
            }
            ++row;
        }
        if(total>6) {
            char position[64]; snprintf(position,sizeof(position),"%d-%d / %d",preceding+1,preceding+row,total);
            text(s,small,position,(SDL_Rect){638,115,105,32},muted,1);
        }
        change=m->focus==TMENU_ITEM_FULLSCREEN || m->focus==TMENU_ITEM_VOLUME;
        confirm=!change && !(m->focus==TMENU_ITEM_LOAD && !m->saved_count);
    } else slots(s,font,small,a,m,thumb_path);
    char notice[TMENU_NOTIFY_BYTES+1];
    if(tmenu_menu_notification(m,notice)) {
        rounded(s,(SDL_Rect){228,639,568,31},10,(SDL_Color){57,46,80,255});
        text(s,small,notice,(SDL_Rect){234,639,556,31},white,1);
    }
    int swap=m->ops.keyshm_get(m->ops.context,22)!=0;
    hint(s,font,small,544,swap?"A":"B",a->portuguese?"Voltar":"Back");
    if(confirm) hint(s,font,small,768,swap?"B":"A",m->screen==TMENU_SCREEN_SAVE ? (a->portuguese?"Guardar":"Save") :
                    m->screen==TMENU_SCREEN_LOAD ? (a->portuguese?"Carregar":"Load") : (a->portuguese?"Selecionar":"Select"));
    if(change) text(s,small,a->portuguese?"Esq./Dir.: Alterar":"Left/Right: Change",(SDL_Rect){768,698,224,44},white,1);
}

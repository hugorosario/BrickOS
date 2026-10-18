#ifndef BRICKOS_GAMESWITCHER_RENDER_H
#define BRICKOS_GAMESWITCHER_RENDER_H
#include "model.h"
#include "motion.h"
#include <SDL.h>
#include <SDL_ttf.h>
// The pictures of the entries two places either side of the selected one, by place (index 2 the
// selected entry), each tagged with its entry's id so that a step keeps the pictures it still
// shows and reads only the new ones. loads counts the pictures read from the store.
#define GS_PLACES 5
struct gs_assets {
    TTF_Font *title, *body, *small;
    SDL_Surface *images[GS_PLACES];
    char ids[GS_PLACES][GS_ID];
    size_t selected;
    int valid[GS_PLACES], loaded, loads;
    // The card of an entry just removed, kept for its fade at the centre; removals counts them, so
    // the renderer draws each removed card once.
    SDL_Surface *removed;
    int removed_valid, removals;
    struct gs_entry removed_entry;
};
// Reads an entry's picture from the store; tests replace it to count the reads.
extern SDL_Surface *(*gs_thumbnail_loader)(struct gs_store *store, const struct gs_entry *entry);
int gs_assets_open(struct gs_assets *assets, const char *font);
void gs_assets_close(struct gs_assets *assets);
void gs_assets_reload(struct gs_assets *assets, struct gs_store *store, const struct gs_model *model);
// Keeps the selected entry's card for its removal fade; call before the history reloads.
void gs_assets_keep_removed(struct gs_assets *assets, const struct gs_model *model);
// Software ARGB surface; no SDL window or second GL context in overlay mode. gs_render draws the
// screen at rest; gs_render_at draws it at now_ms with the motion's slide, zoom and removal.
void gs_render(SDL_Surface *screen, const struct gs_assets *assets, const struct gs_model *model, uint64_t now_ms);
void gs_render_at(SDL_Surface *screen, const struct gs_assets *assets, const struct gs_model *model,
                  const struct gs_motion *motion, uint64_t now_ms);
#endif

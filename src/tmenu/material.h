#ifndef BRICKOS_TMENU_MATERIAL_H
#define BRICKOS_TMENU_MATERIAL_H
#include "render.h"
void tmenu_render_material(SDL_Surface *screen, SDL_Surface *background,
                           const struct tmenu_assets *assets, const struct tmenu_theme *theme,
                           struct tmenu_menu *menu, tmenu_thumb_path_fn thumb_path);
#endif

#ifndef BRICKOS_GAMESWITCHER_MOTION_H
#define BRICKOS_GAMESWITCHER_MOTION_H
#include <stddef.h>
#include <stdint.h>
// GameSwitcher's motion (docs/game-switcher.md): the cards' slide, the fullscreen zoom, a removed
// card's fade and the layer's open and close fades. Each eases out (cubic) over duration_ms, the
// motion time BrickOS's Animations choice gives; 0 puts every value at its target at once. Times
// are passed in, so the rules are tested without SDL.
#define GS_MOTION_MS 200
struct gs_tween { float from, to; uint64_t start_ms; };
struct gs_motion {
    int duration_ms;
    // The cards stood slide_from places off their places at slide_ms: 1 while the new centre card
    // still stands where its right neighbour does. step is the last left (-1) or right (1) key.
    float slide_from;
    uint64_t slide_ms;
    int step;
    struct gs_tween zoom;    // 0 the card, 1 the fullscreen preview
    struct gs_tween removal; // the removed card's opacity, 1 to 0
    struct gs_tween layer;   // the display layer's global alpha, 0 to 255
};
// How far a motion that started at start_ms has come at now_ms, 0 to 1; 1 once duration_ms have
// passed, for no motion (start 0) and for a duration of 0.
float gs_motion_progress(uint64_t start_ms, uint64_t now_ms, int duration_ms);
void gs_motion_init(struct gs_motion *motion, int duration_ms);
// 200 ms times the Animations scale in <sd>/BrickOS/mainui/brickos/appearance.json: 200 for
// Normal, 100 for Fast, 0 for Off; Normal when the file is missing or invalid.
int gs_motion_appearance_ms(const char *sd);
// The selection moved from old to selected among count entries. A neighbour slides, the shortest
// way around the ring (with two entries, the key's direction); a step during a slide starts from
// where the cards are, at most two places off. Anything else, or jump, puts the cards in place.
void gs_motion_select(struct gs_motion *motion, size_t old, size_t selected, size_t count, int jump, uint64_t now_ms);
// How many places the cards still stand off at now_ms; 0 at rest.
float gs_motion_shift(const struct gs_motion *motion, uint64_t now_ms);
// The fullscreen toggle: the zoom heads for 1 or 0 from wherever it is.
void gs_motion_zoom(struct gs_motion *motion, int fullscreen, uint64_t now_ms);
float gs_motion_zoom_value(const struct gs_motion *motion, uint64_t now_ms);
// A removal: the removed card fades out, and the card now selected comes in from side (1 from the
// right, the next entry; -1 from the left, the previous one; 0 when none is left).
void gs_motion_remove(struct gs_motion *motion, int side, uint64_t now_ms);
// The removed card's opacity at now_ms, 1 to 0; 0 once its fade is over.
float gs_motion_removed(const struct gs_motion *motion, uint64_t now_ms);
// The layer fades in (open) to 255, or out from wherever it is to 0.
void gs_motion_fade(struct gs_motion *motion, int open, uint64_t now_ms);
int gs_motion_alpha(const struct gs_motion *motion, uint64_t now_ms);
// 1 while the slide, the zoom, a removal or a fade still moves at now_ms.
int gs_motion_active(const struct gs_motion *motion, uint64_t now_ms);
// The loop paints every 16 ms while something moves, and every 32 ms at rest.
int gs_motion_paint_ms(const struct gs_motion *motion, uint64_t now_ms);
// How long the loop may wait for input before its next frame is due, after one painted at
// painted_ms: never below 0.
int gs_motion_wait_ms(const struct gs_motion *motion, uint64_t now_ms, uint64_t painted_ms);
// Where the card that takes a removed entry's place comes from: 1 (the next one, from the right)
// when the selection kept its place, -1 (the previous one) when it moved back, 0 when none is left.
int gs_motion_removal_side(size_t removed_at, size_t selected, size_t count);
#endif

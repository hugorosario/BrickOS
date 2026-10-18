#include "host.h"

#include <stdio.h>
#include <string.h>

static enum tmenu_call_check reject(char *reason, size_t size, const char *text, enum tmenu_call_check value) {
    snprintf(reason, size, "%s", text);
    return value;
}

static int frame_ok(const struct tmenu_frame_info *f) {
    return f->present && f->w == TMENU_FRAME_W && f->h == TMENU_FRAME_H && f->pitch == TMENU_FRAME_PITCH &&
           f->has_pixels && f->format == TMENU_FRAME_FORMAT;
}

enum tmenu_call_check tmenu_call_check(const struct tmenu_host *host, const struct tmenu_frame_info *screen,
                                       const struct tmenu_frame_info *background, const char *name,
                                       tmenu_present_fn present, char *reason, size_t reason_size) {
    // The official library returns 0 for these two before drawing anything.
    if (!host) return reject(reason, reason_size, "no host table", TMENU_CALL_BAD_ARGUMENTS);
    if (host->slot_count < TMENU_SLOTS_MIN || host->slot_count > TMENU_SLOTS_MAX)
        return reject(reason, reason_size, "slot count outside 1-100", TMENU_CALL_BAD_ARGUMENTS);
    if (!frame_ok(screen) || !frame_ok(background) || !present)
        return reject(reason, reason_size, "screen, background, or present differ from the measured 1024x768 ABGR8888",
                      TMENU_CALL_UNKNOWN_HOST);
    if (!host->saved_slots || !host->thumb_path || !host->save_state || !host->load_state || !host->quit)
        return reject(reason, reason_size, "a host callback is missing", TMENU_CALL_UNKNOWN_HOST);
    if (!name || strcmp(name, "RetroArch") != 0)
        return reject(reason, reason_size, "the caller name is not RetroArch", TMENU_CALL_UNKNOWN_HOST);
    return reject(reason, reason_size, "the observed RetroArch call", TMENU_CALL_OK);
}

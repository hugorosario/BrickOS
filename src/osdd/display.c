#define _GNU_SOURCE
#include "display.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DISP_LAYER_GET_CONFIG 0x48
#define DISP_LAYER_SET_CONFIG 0x47

static uint32_t read_u32(const unsigned char *bytes, size_t offset) {
    uint32_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

static uint64_t read_u64(const unsigned char *bytes, size_t offset) {
    uint64_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

static void write_u32(unsigned char *bytes, size_t offset, uint32_t value) {
    memcpy(bytes + offset, &value, sizeof(value));
}

static void write_u64(unsigned char *bytes, size_t offset, uint64_t value) {
    memcpy(bytes + offset, &value, sizeof(value));
}

static int error_message(char *error, size_t size, const char *message) {
    if (error != NULL && size > 0) snprintf(error, size, "%s", message);
    return -1;
}

int osdd_display_validate(const struct fb_var_screeninfo *variable,
                          const struct fb_fix_screeninfo *fixed,
                          const unsigned char layers[3][OSDD_LAYER_BYTES],
                          struct osdd_display_profile *profile,
                          char *error, size_t error_size) {
    if (variable == NULL || fixed == NULL || layers == NULL || profile == NULL)
        return error_message(error, error_size, "missing display profile input");
    if (variable->xres != OSDD_WIDTH || variable->yres != OSDD_HEIGHT ||
        variable->xres_virtual != OSDD_WIDTH || variable->yres_virtual != 16384 ||
        variable->bits_per_pixel != 32 || fixed->line_length != OSDD_STRIDE)
        return error_message(error, error_size, "unsupported framebuffer geometry");
    if (fixed->smem_start == 0 ||
        fixed->smem_len < OSDD_PANEL_SLOT1_OFFSET + OSDD_SLOT_BYTES)
        return error_message(error, error_size, "insufficient framebuffer mapping");
    // Before the first OSD after boot, the driver returns all zeros for channels 2 and 3
    // and channel 1 keeps its boot z of 16 with alpha mode 0.
    int unconfigured[2] = {1, 1};
    for (uint32_t index = 1; index < 3; ++index)
        for (size_t offset = 0; offset < OSDD_LAYER_BYTES; ++offset)
            if (layers[index][offset] != 0) unconfigured[index - 1] = 0;
    int frontend_boot = layers[0][0x04] == 16 && layers[0][0x05] == 0;
    if (unconfigured[0] != unconfigured[1] || unconfigured[0] != frontend_boot)
        return error_message(error, error_size, "unexpected partly initialized layers");
    int first_osd = unconfigured[0];
    for (uint32_t index = 0; index < (first_osd ? 1U : 3U); ++index) {
        const unsigned char *layer = layers[index];
        unsigned z = index == 0 && first_osd ? 16 : (index + 1) * 4;
        if (read_u32(layer, 0xac) != index + 1 || read_u32(layer, 0xb0) != 0 ||
            read_u32(layer, 0x08) != 0 || layer[0x04] != z)
            return error_message(error, error_size, "unsupported layer identity or z order");
        if (read_u64(layer, 0x88) != ((uint64_t)OSDD_WIDTH << 32) ||
            read_u64(layer, 0x90) != ((uint64_t)OSDD_HEIGHT << 32))
            return error_message(error, error_size, "unsupported layer crop");
    }
    uint64_t base = fixed->smem_start;
    uint64_t frontend_address = read_u64(layers[0], 0x20);
    if (frontend_address < base || frontend_address >= base + OSDD_PANEL_SLOT0_OFFSET ||
        layers[0][0xa8] != 1)
        return error_message(error, error_size, "unexpected frontend layer");
    // An OSD that ends normally leaves both channels idle at their first slot. One that was
    // killed leaves them as they were: shown at any alpha, and with BrickOS at either of the
    // channel's two slots. The official daemon starts over that state and sets both channels
    // idle, so it is accepted too; any other address or alpha mode is not.
    int left_shown = 0;
    for (uint32_t index = 1; index < 3 && !first_osd; ++index) {
        uint64_t address = read_u64(layers[index], 0x20);
        uint64_t first = base + (index == 1 ? OSDD_PANEL_SLOT0_OFFSET : OSDD_TOAST_SLOT0_OFFSET);
        uint64_t second = base + (index == 1 ? OSDD_PANEL_SLOT1_OFFSET : OSDD_TOAST_SLOT1_OFFSET);
        if (address != first && address != second)
            return error_message(error, error_size, "OSD layer address is outside reserved slots");
        if (layers[index][0xa8] != 1 || layers[index][0x05] != 2)
            return error_message(error, error_size, "unsupported OSD layer state");
        if (address != first || layers[index][0x06] != 0) left_shown = 1;
    }
    memset(profile, 0, sizeof(*profile));
    profile->physical_base = base;
    profile->map_bytes = fixed->smem_len;
    profile->first_osd = first_osd;
    profile->left_shown = left_shown;
    if (first_osd || left_shown) {
        // The idle layers the official daemon sets up, restored again on exit.
        osdd_display_layer_config_channel(profile->saved[0], 2, base + OSDD_PANEL_SLOT0_OFFSET, 0);
        osdd_display_layer_config_channel(profile->saved[1], 3, base + OSDD_TOAST_SLOT0_OFFSET, 0);
    } else {
        memcpy(profile->saved[0], layers[1], OSDD_LAYER_BYTES);
        memcpy(profile->saved[1], layers[2], OSDD_LAYER_BYTES);
    }
    if (error != NULL && error_size > 0) error[0] = 0;
    return 0;
}

void osdd_display_layer_config_channel(unsigned char config[OSDD_LAYER_BYTES],
                                       uint32_t channel, uint64_t physical_address,
                                       unsigned char alpha) {
    memset(config, 0, OSDD_LAYER_BYTES);
    config[0x04] = (unsigned char)(channel * 4);
    config[0x05] = 2;              // Global alpha mode.
    config[0x06] = alpha;
    write_u32(config, 0x08, 0);    // ARGB8888 in the legacy disp ABI.
    write_u32(config, 0x10, OSDD_WIDTH);
    write_u32(config, 0x14, OSDD_HEIGHT);
    write_u64(config, 0x20, physical_address);
    for (size_t offset = 0x38; offset <= 0x48; offset += 8) {
        write_u32(config, offset, OSDD_WIDTH);
        write_u32(config, offset + 4, OSDD_HEIGHT);
    }
    write_u64(config, 0x88, (uint64_t)OSDD_WIDTH << 32);
    write_u64(config, 0x90, (uint64_t)OSDD_HEIGHT << 32);
    config[0xa8] = 1;
    write_u32(config, 0xac, channel);
    write_u32(config, 0xb0, 0);
}

void osdd_display_frontend_first_osd(const unsigned char boot[OSDD_LAYER_BYTES],
                                     unsigned char config[OSDD_LAYER_BYTES]) {
    memcpy(config, boot, OSDD_LAYER_BYTES);
    config[0x04] = 4;
    config[0x05] = 2;
}

void osdd_display_layer_config(unsigned char config[OSDD_LAYER_BYTES],
                               uint64_t physical_address, unsigned char alpha) {
    osdd_display_layer_config_channel(config, 3, physical_address, alpha);
}

static int layer_ioctl(int fd, unsigned long request, uint32_t channel,
                       unsigned char bytes[OSDD_LAYER_BYTES]) {
    unsigned long args[3] = {0, (unsigned long)(uintptr_t)bytes, 1};
    if (request == DISP_LAYER_GET_CONFIG) {
        memset(bytes, 0, OSDD_LAYER_BYTES);
        write_u32(bytes, 0xac, channel);
        write_u32(bytes, 0xb0, 0);
    }
    return ioctl(fd, request, args);
}

// The legacy driver does not serialize layer GET and SET calls from different
// processes; a concurrent call has returned another channel's configuration. Read
// each SET back and repeat it until the channel reports the requested state.
static int set_layer(struct osdd_display *display, uint32_t channel,
                     unsigned char config[OSDD_LAYER_BYTES]) {
    unsigned char applied[OSDD_LAYER_BYTES];
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0) ++display->set_retries;
        if (layer_ioctl(display->disp_fd, DISP_LAYER_SET_CONFIG, channel, config) < 0) return -1;
        if (layer_ioctl(display->disp_fd, DISP_LAYER_GET_CONFIG, channel, applied) < 0) return -1;
        if (read_u32(applied, 0xac) == channel && applied[0x04] == config[0x04] &&
            applied[0x06] == config[0x06] && read_u64(applied, 0x20) == read_u64(config, 0x20))
            return 0;
    }
    errno = EIO;
    return -1;
}

int osdd_display_open(struct osdd_display *display, char *error, size_t error_size) {
    if (display == NULL) return error_message(error, error_size, "missing display object");
    memset(display, 0, sizeof(*display));
    display->fb_fd = -1;
    display->disp_fd = -1;
    display->front_slot = 1; // First publication uses slot 0.
    display->panel_front_slot = 1;
    display->fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (display->fb_fd < 0) goto io_error;
    struct fb_var_screeninfo variable;
    struct fb_fix_screeninfo fixed;
    if (ioctl(display->fb_fd, FBIOGET_VSCREENINFO, &variable) < 0 ||
        ioctl(display->fb_fd, FBIOGET_FSCREENINFO, &fixed) < 0) goto io_error;
    display->disp_fd = open("/dev/disp", O_RDWR | O_CLOEXEC);
    if (display->disp_fd < 0) goto io_error;
    unsigned char layers[3][OSDD_LAYER_BYTES];
    for (uint32_t channel = 1; channel <= 3; ++channel) {
        if (layer_ioctl(display->disp_fd, DISP_LAYER_GET_CONFIG,
                        channel, layers[channel - 1]) < 0) goto io_error;
    }
    if (osdd_display_validate(&variable, &fixed, layers, &display->profile,
                              error, error_size) < 0) {
        osdd_display_close(display);
        return -1;
    }
    display->mapping = mmap(NULL, display->profile.map_bytes,
                            PROT_READ | PROT_WRITE, MAP_SHARED, display->fb_fd, 0);
    if (display->mapping == MAP_FAILED) {
        display->mapping = NULL;
        goto io_error;
    }
    if (display->profile.first_osd) {
        // As the official daemon does when it is the first OSD after boot: both OSD
        // channels idle at their first slots, then channel 1's z and alpha mode.
        unsigned char config[OSDD_LAYER_BYTES];
        for (uint32_t channel = 2; channel <= 3; ++channel) {
            memcpy(config, display->profile.saved[channel - 2], sizeof(config));
            if (set_layer(display, channel, config) < 0) goto io_error;
        }
        osdd_display_frontend_first_osd(layers[0], config);
        if (set_layer(display, 1, config) < 0) goto io_error;
    } else if (display->profile.left_shown) {
        // An earlier OSD's shown layers are set idle at their first slots. Channel 2 stays as
        // it is while GameSwitcher holds it.
        int shared = osdd_display_gameswitcher_marker(osdd_display_marker_path);
        unsigned char config[OSDD_LAYER_BYTES];
        for (uint32_t channel = shared ? 3 : 2; channel <= 3; ++channel) {
            memcpy(config, display->profile.saved[channel - 2], sizeof(config));
            if (set_layer(display, channel, config) < 0) goto io_error;
        }
    }
    display->initialized = 1;
    return 0;
io_error:
    if (error != NULL && error_size > 0)
        snprintf(error, error_size, "display GET/open: %s", strerror(errno));
    osdd_display_close(display);
    return -1;
}

int osdd_display_stage(struct osdd_display *display, const void *frame,
                       size_t frame_bytes, uint64_t now_ms, size_t *offset,
                       char *error, size_t error_size) {
    if (display == NULL || !display->initialized || frame == NULL ||
        display->mapping == NULL || offset == NULL || frame_bytes != OSDD_FRAME_BYTES)
        return error_message(error, error_size, "invalid complete OSD frame");
    if (display->last_publish_ms != 0 && now_ms < display->last_publish_ms + 50)
        return error_message(error, error_size, "OSD slot retirement interval not elapsed");
    int back = 1 - display->front_slot;
    *offset = back == 0 ? OSDD_TOAST_SLOT0_OFFSET : OSDD_TOAST_SLOT1_OFFSET;
    if (*offset + frame_bytes > display->profile.map_bytes)
        return error_message(error, error_size, "OSD slot exceeds framebuffer mapping");
    memcpy(display->mapping + *offset, frame, frame_bytes);
    return 0;
}

int osdd_display_stage_panel(struct osdd_display *display, const void *frame,
                             size_t frame_bytes, uint64_t now_ms, size_t *offset,
                             char *error, size_t error_size) {
    if (display == NULL || !display->initialized || frame == NULL ||
        display->mapping == NULL || offset == NULL || frame_bytes != OSDD_FRAME_BYTES)
        return error_message(error, error_size, "invalid complete panel frame");
    if (display->panel_last_publish_ms != 0 &&
        now_ms < display->panel_last_publish_ms + 50)
        return error_message(error, error_size, "panel slot retirement interval not elapsed");
    int back = 1 - display->panel_front_slot;
    *offset = back == 0 ? OSDD_PANEL_SLOT0_OFFSET : OSDD_PANEL_SLOT1_OFFSET;
    if (*offset + frame_bytes > display->profile.map_bytes)
        return error_message(error, error_size, "panel slot exceeds framebuffer mapping");
    memcpy(display->mapping + *offset, frame, frame_bytes);
    return 0;
}

int osdd_display_present(struct osdd_display *display, const void *frame,
                         size_t frame_bytes, uint64_t now_ms,
                         char *error, size_t error_size) {
    size_t offset;
    if (osdd_display_stage(display, frame, frame_bytes, now_ms,
                           &offset, error, error_size) < 0) return -1;
    unsigned char config[OSDD_LAYER_BYTES];
    osdd_display_layer_config(config, display->profile.physical_base + offset, 255);
    if (set_layer(display, 3, config) < 0) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "publish OSD layer: %s", strerror(errno));
        return -1;
    }
    display->front_slot = offset == OSDD_TOAST_SLOT0_OFFSET ? 0 : 1;
    display->last_publish_ms = now_ms;
    display->visible = 1;
    return 0;
}

int osdd_display_hide(struct osdd_display *display, char *error, size_t error_size) {
    if (display == NULL || !display->initialized) return 0;
    size_t offset = display->front_slot == 0 ?
        OSDD_TOAST_SLOT0_OFFSET : OSDD_TOAST_SLOT1_OFFSET;
    unsigned char config[OSDD_LAYER_BYTES];
    osdd_display_layer_config(config, display->profile.physical_base + offset, 0);
    if (set_layer(display, 3, config) < 0) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "hide OSD layer: %s", strerror(errno));
        return -1;
    }
    display->visible = 0;
    return 0;
}

int osdd_display_present_panel(struct osdd_display *display, const void *frame,
                               size_t frame_bytes, uint64_t now_ms, unsigned char alpha,
                               char *error, size_t error_size) {
    size_t offset;
    if (osdd_display_stage_panel(display, frame, frame_bytes, now_ms,
                                 &offset, error, error_size) < 0) return -1;
    unsigned char config[OSDD_LAYER_BYTES];
    osdd_display_layer_config_channel(config, 2,
                                      display->profile.physical_base + offset, alpha);
    if (set_layer(display, 2, config) < 0) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "publish panel layer: %s", strerror(errno));
        return -1;
    }
    display->panel_front_slot = offset == OSDD_PANEL_SLOT0_OFFSET ? 0 : 1;
    display->panel_last_publish_ms = now_ms;
    display->panel_visible = alpha != 0;
    return 0;
}

int osdd_display_panel_alpha(struct osdd_display *display, unsigned char alpha,
                             char *error, size_t error_size) {
    if (display == NULL || !display->initialized) return 0;
    size_t offset = display->panel_front_slot == 0 ?
        OSDD_PANEL_SLOT0_OFFSET : OSDD_PANEL_SLOT1_OFFSET;
    unsigned char config[OSDD_LAYER_BYTES];
    osdd_display_layer_config_channel(config, 2,
                                      display->profile.physical_base + offset, alpha);
    if (set_layer(display, 2, config) < 0) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "set panel alpha: %s", strerror(errno));
        return -1;
    }
    display->panel_visible = alpha != 0;
    return 0;
}

int osdd_display_hide_panel(struct osdd_display *display, char *error, size_t error_size) {
    return osdd_display_panel_alpha(display, 0, error, error_size);
}

const char *osdd_display_marker_path = OSDD_GAMESWITCHER_DISPLAY_MARKER;

int osdd_display_gameswitcher_marker(const char *path) {
    char text[32] = {0}, link[64], exe[512];
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    ssize_t count = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (count <= 0) return 0;
    char *end;
    long pid = strtol(text, &end, 10);
    if (pid <= 0 || pid > 0x7fffffffL || (*end != '\n' && *end != 0)) return 0;
    snprintf(link, sizeof(link), "/proc/%ld/exe", pid);
    ssize_t length = readlink(link, exe, sizeof(exe) - 1);
    if (length <= 0) return 0;
    exe[length] = 0;
    const char *name = strrchr(exe, '/');
    name = name != NULL ? name + 1 : exe;
    // A runtime replaced while GameSwitcher runs shows as deleted.
    return strcmp(name, "GameSwitcher") == 0 || strcmp(name, "GameSwitcher (deleted)") == 0;
}

void osdd_display_close(struct osdd_display *display) {
    if (display == NULL) return;
    if (display->initialized && display->disp_fd >= 0) {
        int shared = osdd_display_gameswitcher_marker(osdd_display_marker_path);
        if (display->panel_visible && !shared) osdd_display_hide_panel(display, NULL, 0);
        if (display->visible) osdd_display_hide(display, NULL, 0);
        for (uint32_t channel = shared ? 3 : 2; channel <= 3; ++channel) {
            unsigned char saved[OSDD_LAYER_BYTES];
            memcpy(saved, display->profile.saved[channel - 2], sizeof(saved));
            layer_ioctl(display->disp_fd, DISP_LAYER_SET_CONFIG, channel, saved);
        }
    }
    if (display->mapping != NULL) munmap(display->mapping, display->profile.map_bytes);
    if (display->disp_fd >= 0) close(display->disp_fd);
    if (display->fb_fd >= 0) close(display->fb_fd);
    display->mapping = NULL;
    display->disp_fd = display->fb_fd = -1;
    display->initialized = 0;
}

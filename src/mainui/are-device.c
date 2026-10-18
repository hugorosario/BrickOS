#define _POSIX_C_SOURCE 200809L
#include "are-device.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#define PF_CONFIG (0x90U / 4)
#define PF_DATA (0xa0U / 4)
#define PF_PULL (0xacU / 4)

static int allowed(int pin) { return pin == MAINUI_ARE_SCL || pin == MAINUI_ARE_SDA; }
static int configure(void *context, int pin, int output) {
    if (!allowed(pin) || (output != 0 && output != 1)) return -1;
    volatile uint32_t *pio = context;
    unsigned shift = (unsigned)(pin & 7) * 4;
    pio[PF_CONFIG] = (pio[PF_CONFIG] & ~(15U << shift)) | ((unsigned)output << shift);
    return 0;
}
static int pull(void *context, int pin, int value) {
    if (pin != MAINUI_ARE_SDA || (value != 0 && value != 1)) return -1;
    volatile uint32_t *pio = context;
    unsigned shift = (unsigned)(pin & 15) * 2;
    pio[PF_PULL] = (pio[PF_PULL] & ~(3U << shift)) | ((unsigned)value << shift);
    return 0;
}
static int set(void *context, int pin, int value) {
    if (!allowed(pin) || (value != 0 && value != 1)) return -1;
    volatile uint32_t *pio = context;
    unsigned mask = 1U << (pin & 31);
    pio[PF_DATA] = value ? pio[PF_DATA] | mask : pio[PF_DATA] & ~mask;
    return 0;
}
static int get(void *context, int pin, int *value) {
    if (pin != MAINUI_ARE_SDA || !value) return -1;
    volatile uint32_t *pio = context;
    *value = (int)((pio[PF_DATA] >> (pin & 31)) & 1U);
    return 0;
}
static int tick(void *context) {
    (void)context;
    // Reference firmware's gAreLinkOSBaseDelayUnit defaults to 350. Preserve its
    // volatile countdown shape; actual bus frequency still needs a device check.
    volatile int remaining = 350, discarded = 0;
    while (remaining > 0) { discarded = remaining + 1; remaining = remaining - 1; }
    (void)discarded;
    return 0;
}
int mainui_are_mmio_ops(void *pio, size_t length, struct mainui_are_gpio_ops *ops) {
    if (!ops) return -1;
    memset(ops, 0, sizeof(*ops));
    if (!pio || ((uintptr_t)pio & 3U) || length < 0xb0U) return -1;
    *ops = (struct mainui_are_gpio_ops){pio, configure, pull, set, get, tick};
    return 0;
}
static int delay(void *context, unsigned milliseconds) {
    (void)context;
    struct timespec wait = {(time_t)(milliseconds / 1000U), (long)(milliseconds % 1000U) * 1000000L};
    while (nanosleep(&wait, &wait)) if (errno != EINTR) return -1;
    return 0;
}
static int nonce(void *context, unsigned char *value) {
    (void)context;
    for (int attempt = 0; attempt < 64; ++attempt) {
        unsigned char byte;
        ssize_t length;
        do { length = getrandom(&byte, 1, GRND_NONBLOCK); } while (length < 0 && errno == EINTR);
        if (length != 1) return -1;
        byte %= 255U;
        if (byte) { *value = byte; return 0; }
    }
    return -1;
}
static void release(struct mainui_are_device *d) {
    mainui_are_close(&d->session);
    mainui_are_bus_close(&d->bus);
    if (d->pio) munmap(d->pio, MAINUI_ARE_PIO_MAP);
    d->pio = NULL;
}
void mainui_are_device_close(struct mainui_are_device *d) {
    if (!d) return;
    release(d);
    memset(d, 0, sizeof(*d));
}
static int prepare(struct mainui_are_device *d) {
    struct mainui_are_tables tables;
    // Fail before GPIO access if the installed firmware tables are unavailable.
    if (mainui_are_load_tables(d->stock_elf, &tables)) return -1;
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) return -1;
    void *map = mmap(NULL, MAINUI_ARE_PIO_MAP, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MAINUI_ARE_PIO_BASE);
    close(fd);
    if (map == MAP_FAILED) return -1;
    d->pio = map;
    struct mainui_are_gpio_ops gpio;
    struct mainui_are_transport transport = {&d->bus, mainui_are_bus_read, mainui_are_bus_write, delay, nonce};
    if (mainui_are_mmio_ops(map, MAINUI_ARE_PIO_MAP, &gpio) || mainui_are_bus_open(&d->bus, &gpio) ||
        mainui_are_bus_power_on(&d->bus) || mainui_are_initialize(&d->session, &tables, &transport)) {
        release(d);
        return -1;
    }
    return 0;
}
int mainui_are_device_serial(void *context, char *out, size_t size) {
    struct mainui_are_device *d = context;
    if (out && size) out[0] = 0;
    if (!d || !out || size < 17 || !d->stock_elf[0]) return -1;
    if (!d->session.ready && prepare(d)) return -1;
    const unsigned char *id = d->session.tables.first;
    uint32_t word = (uint32_t)id[0] | (uint32_t)id[1] << 8 | (uint32_t)id[2] << 16 | (uint32_t)id[3] << 24;
    snprintf(out, size, "TG3040%02X%08X", (unsigned)id[4], (unsigned)(word + UINT32_C(0x3c14dc00)));
    return 0;
}
static int block(void *context, const unsigned char input[16], unsigned char output[16], int decrypt) {
    struct mainui_are_device *d = context;
    unsigned char source[16];
    if (input) memcpy(source, input, sizeof(source));
    if (output) memset(output, 0, 16);
    if (!d || !input || !output || (decrypt != 0 && decrypt != 1)) return -1;
    if (!d->session.ready && prepare(d)) return -1;
    int result = mainui_are_block(&d->session, source, output, decrypt);
    if (result) release(d);
    return result;
}
int mainui_are_device_encoder(struct mainui_are_device *d, const char *stock_elf, struct mainui_encoder *encoder) {
    if (encoder) memset(encoder, 0, sizeof(*encoder));
    if (!d || !encoder || !stock_elf || stock_elf[0] != '/' || strlen(stock_elf) >= sizeof(d->stock_elf)) return -1;
    memset(d, 0, sizeof(*d));
    strcpy(d->stock_elf, stock_elf);
    *encoder = (struct mainui_encoder){d, block};
    return 0;
}

#include "are-bus.h"
#include <string.h>

#define CHIP_WRITE 0x38
#define CHIP_READ 0x39

static int cfg(struct mainui_are_bus *b, int pin, int output) { return b->ops.configure(b->ops.context, pin, output); }
static int pull(struct mainui_are_bus *b, int value) { return b->ops.pull(b->ops.context, MAINUI_ARE_SDA, value); }
static int set(struct mainui_are_bus *b, int pin, int value) { return b->ops.set(b->ops.context, pin, value); }
static int tick(struct mainui_are_bus *b) { return b->ops.tick(b->ops.context); }
static int get(struct mainui_are_bus *b, int *value) {
    if (b->ops.get(b->ops.context, MAINUI_ARE_SDA, value)) return -1;
    return *value == 0 || *value == 1 ? 0 : -1;
}
static int output(struct mainui_are_bus *b) {
    return cfg(b, MAINUI_ARE_SDA, 1) || set(b, MAINUI_ARE_SDA, 1) ? -1 : 0;
}
static int input(struct mainui_are_bus *b) {
    return cfg(b, MAINUI_ARE_SDA, 0) || pull(b, 0) || pull(b, 1) ? -1 : 0;
}
static int stop(struct mainui_are_bus *b) {
    // Try every idle transition even when one callback fails during recovery.
    int failed = cfg(b, MAINUI_ARE_SDA, 1) != 0;
    failed |= set(b, MAINUI_ARE_SDA, 1) != 0;
    failed |= set(b, MAINUI_ARE_SCL, 0) != 0;
    failed |= set(b, MAINUI_ARE_SDA, 0) != 0;
    failed |= tick(b) != 0;
    failed |= set(b, MAINUI_ARE_SCL, 1) != 0;
    failed |= tick(b) != 0;
    failed |= set(b, MAINUI_ARE_SDA, 1) != 0;
    return failed ? -1 : 0;
}
static int start(struct mainui_are_bus *b) {
    return output(b) || set(b, MAINUI_ARE_SDA, 1) || set(b, MAINUI_ARE_SCL, 1) || tick(b) ||
           set(b, MAINUI_ARE_SDA, 0) || tick(b) || set(b, MAINUI_ARE_SCL, 0) ? -1 : 0;
}
static int write_byte(struct mainui_are_bus *b, unsigned char byte) {
    for (unsigned bit = 0x80; bit; bit >>= 1)
        if (set(b, MAINUI_ARE_SDA, !!(byte & bit)) || set(b, MAINUI_ARE_SCL, 1) || tick(b) ||
            set(b, MAINUI_ARE_SCL, 0) || tick(b)) return -1;
    if (set(b, MAINUI_ARE_SDA, 1) || input(b) || set(b, MAINUI_ARE_SCL, 1) || tick(b) ||
        cfg(b, MAINUI_ARE_SDA, 0) || pull(b, 0)) return -1;
    int ack;
    if (get(b, &ack) || set(b, MAINUI_ARE_SCL, 0) || output(b)) return -1;
    return ack;
}
static int read_byte(struct mainui_are_bus *b, int acknowledge, unsigned char *byte) {
    unsigned char result = 0;
    if (input(b)) return -1;
    for (unsigned bit = 0x80; bit; bit >>= 1) {
        int value;
        if (set(b, MAINUI_ARE_SCL, 1) || cfg(b, MAINUI_ARE_SDA, 0) || pull(b, 0) || get(b, &value)) return -1;
        if (value) result |= (unsigned char)bit;
        if (set(b, MAINUI_ARE_SCL, 0) || tick(b)) return -1;
    }
    if (output(b) || set(b, MAINUI_ARE_SDA, 1) || (acknowledge && set(b, MAINUI_ARE_SDA, 0)) ||
        set(b, MAINUI_ARE_SCL, 1) || tick(b) || set(b, MAINUI_ARE_SCL, 0)) return -1;
    *byte = result;
    return 0;
}
static int send_address(struct mainui_are_bus *b) {
    int value;
    if (cfg(b, MAINUI_ARE_SDA, 0) || pull(b, 0) || get(b, &value) || (!value && stop(b))) return -1;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (start(b)) return -1;
        int ack = write_byte(b, CHIP_WRITE);
        if (ack <= 0) return ack;
    }
    return -1;
}
static int write_transfer(struct mainui_are_bus *b, const unsigned char *address, size_t addresses,
                           const unsigned char *bytes, size_t length) {
    int ok = send_address(b) == 0;
    for (size_t i = 0; ok && i < addresses; ++i) ok = write_byte(b, address[i]) == 0;
    for (size_t i = 0; ok && i < length; ++i) ok = write_byte(b, bytes[i]) == 0;
    if (stop(b)) ok = 0;
    return ok ? 0 : -1;
}
int mainui_are_bus_open(struct mainui_are_bus *bus, const struct mainui_are_gpio_ops *ops) {
    if (!bus) return -1;
    memset(bus, 0, sizeof(*bus));
    if (!ops || !ops->configure || !ops->pull || !ops->set || !ops->get || !ops->tick) return -1;
    bus->ops = *ops;
    int ok = cfg(bus, MAINUI_ARE_SDA, 1) == 0 && cfg(bus, MAINUI_ARE_SCL, 1) == 0;
    if (stop(bus)) ok = 0;
    if (!ok) { memset(bus, 0, sizeof(*bus)); return -1; }
    bus->ready = 1;
    return 0;
}
void mainui_are_bus_close(struct mainui_are_bus *bus) {
    if (!bus) return;
    if (bus->ready) (void)stop(bus);
    memset(bus, 0, sizeof(*bus));
}
int mainui_are_bus_power_on(struct mainui_are_bus *bus) {
    const unsigned char command = 0xf0;
    return bus && bus->ready ? write_transfer(bus, NULL, 0, &command, 1) : -1;
}
int mainui_are_bus_write(void *context, uint16_t address, const unsigned char *bytes, size_t length) {
    struct mainui_are_bus *bus = context;
    if (!bus || !bus->ready || !bytes || !length || length > 255) return -1;
    unsigned char prefix[2] = {(unsigned char)(address >> 8), (unsigned char)address};
    return write_transfer(bus, prefix, sizeof(prefix), bytes, length);
}
int mainui_are_bus_read(void *context, uint16_t address, unsigned char *bytes, size_t length) {
    struct mainui_are_bus *bus = context;
    if (!bytes || !length || length > 255) return -1;
    memset(bytes, 0, length);
    if (!bus || !bus->ready) return -1;
    unsigned char prefix[2] = {(unsigned char)(address >> 8), (unsigned char)address};
    int ok = send_address(bus) == 0;
    for (size_t i = 0; ok && i < sizeof(prefix); ++i) ok = write_byte(bus, prefix[i]) == 0;
    if (ok) ok = set(bus, MAINUI_ARE_SDA, 1) == 0 && start(bus) == 0 && write_byte(bus, CHIP_READ) == 0;
    for (size_t i = 0; ok && i < length; ++i) ok = read_byte(bus, i + 1 < length, &bytes[i]) == 0;
    if (stop(bus)) ok = 0;
    if (!ok) memset(bytes, 0, length);
    return ok ? 0 : -1;
}

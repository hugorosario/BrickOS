#ifndef BRICKOS_MAINUI_ARE_BUS_H
#define BRICKOS_MAINUI_ARE_BUS_H

#include <stddef.h>
#include <stdint.h>

// Stock bit-banged bus: PF1 is clock, PF2 is data. The device adapter supplies
// register access; fixtures record these same operations without touching pins.
#define MAINUI_ARE_SCL 129
#define MAINUI_ARE_SDA 130
struct mainui_are_gpio_ops {
    void *context;
    int (*configure)(void *context, int pin, int output);
    int (*pull)(void *context, int pin, int value);
    int (*set)(void *context, int pin, int value);
    int (*get)(void *context, int pin, int *value);
    int (*tick)(void *context);
};
struct mainui_are_bus {
    struct mainui_are_gpio_ops ops;
    int ready;
};
int mainui_are_bus_open(struct mainui_are_bus *bus, const struct mainui_are_gpio_ops *ops);
void mainui_are_bus_close(struct mainui_are_bus *bus);
// The chip power-on command has no register prefix, matching arelink_power_on.
int mainui_are_bus_power_on(struct mainui_are_bus *bus);
// Compatible with mainui_are_transport.read/write; address bytes are big endian.
int mainui_are_bus_read(void *context, uint16_t address, unsigned char *bytes, size_t length);
int mainui_are_bus_write(void *context, uint16_t address, const unsigned char *bytes, size_t length);

#endif

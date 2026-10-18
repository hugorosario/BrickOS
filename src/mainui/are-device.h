#ifndef BRICKOS_MAINUI_ARE_DEVICE_H
#define BRICKOS_MAINUI_ARE_DEVICE_H

#include "are.h"
#include "are-bus.h"
#include "launch.h"

#define MAINUI_ARE_PIO_BASE 0x0300b000U
#define MAINUI_ARE_PIO_MAP 0x2000U
struct mainui_are_device {
    struct mainui_are session;
    struct mainui_are_bus bus;
    void *pio;
    char stock_elf[1024];
};
// Binds only PF1/PF2 fields in an existing map. Also usable with an aligned host
// register fixture; does not open /dev/mem or initiate bus traffic.
int mainui_are_mmio_ops(void *pio, size_t length, struct mainui_are_gpio_ops *ops);
// Installs a lazy encoder: loading tables and opening the GPIO bus happen on the
// first block, not while the frontend starts. Call only for a real, non-dry root.
// Configure a fresh object once; close it before configuring it again.
int mainui_are_device_encoder(struct mainui_are_device *device, const char *stock_elf,
                              struct mainui_encoder *encoder);
void mainui_are_device_close(struct mainui_are_device *device);
// Lazily reads the ARE identity and formats the stock Device Info serial.
int mainui_are_device_serial(void *context, char *out, size_t size);

#endif

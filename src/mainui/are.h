#ifndef BRICKOS_MAINUI_ARE_H
#define BRICKOS_MAINUI_ARE_H

#include <stddef.h>
#include <stdint.h>

// Protocol tables belong to the installed firmware; no firmware bytes or chip
// credentials are compiled into BrickOS. Loading does not access hardware.
struct mainui_are_tables {
    unsigned char first[32], second[16];
};
int mainui_are_load_tables(const char *stock_elf, struct mainui_are_tables *tables);

// The caller prepares/powers the stock GPIO I2C bus before initialization.
// Transfers use the stock chip's 16-bit register addresses. All callbacks return
// zero on success. nonce supplies a nonzero byte, as stock arelink_random does.
struct mainui_are_transport {
    void *context;
    int (*read)(void *context, uint16_t address, unsigned char *bytes, size_t length);
    int (*write)(void *context, uint16_t address, const unsigned char *bytes, size_t length);
    int (*delay)(void *context, unsigned milliseconds);
    int (*nonce)(void *context, unsigned char *value);
};
struct mainui_are {
    struct mainui_are_tables tables;
    struct mainui_are_transport transport;
    int ready;
};
// Reads the chip handshake and updates the session's table, as arelink_init does.
// Hardware/transport failures leave the session unavailable; no EEPROM writes.
int mainui_are_initialize(struct mainui_are *are, const struct mainui_are_tables *tables,
                          const struct mainui_are_transport *transport);
void mainui_are_close(struct mainui_are *are);
// Same callback signature as mainui_encoder.block. No substitute software cipher:
// the transport must deliver the actual chip's 16-byte encrypt/decrypt response.
int mainui_are_block(void *context, const unsigned char input[16],
                     unsigned char output[16], int decrypt);

#endif

#ifndef BRICKOS_INPUTD_GPIO_H
#define BRICKOS_INPUTD_GPIO_H

#include <stdint.h>

// Allwinner PIO access as the stock daemon does it: 8 KiB of /dev/mem mapped at the PIO
// bank base 0x0300b000 and at the LPIO base 0x07022000. A GPIO number splits into bank
// gpio >> 5 (stride 0x24) and pin gpio & 31; bank 11 (PL) lives in the LPIO block.
#define INPUTD_PIO_BASE 0x0300b000u
#define INPUTD_LPIO_BASE 0x07022000u
#define INPUTD_PIO_MAP 0x2000u
#define INPUTD_LPIO_BANK 11

struct inputd_gpio_reg {
    int lpio;          // 1 for the LPIO block
    uint32_t cfg;      // byte offset of the pin's CFG register in its block
    int cfg_shift;     // 4-bit field
    uint32_t dat;      // byte offset of the bank's DAT register
    int dat_bit;
};

// Pure register arithmetic, for host tests. Returns -1 for a negative or out-of-range GPIO.
int inputd_gpio_reg(int gpio, struct inputd_gpio_reg *reg);

struct inputd_gpio {
    volatile uint8_t *pio;
    volatile uint8_t *lpio;
    int readonly;
};

// Maps both blocks; the descriptor is closed afterwards. A read-only mapping cannot change
// the mux, for passive checks while another producer runs. Returns 0 or -1.
int inputd_gpio_open(struct inputd_gpio *g, int readonly);
void inputd_gpio_close(struct inputd_gpio *g);
// Sets the pin's mux to 0 (input), leaving the pull configuration alone, as the stock
// daemon does. Returns 0 or -1.
int inputd_gpio_input(struct inputd_gpio *g, int gpio);
// The pin's mux field, or -1.
int inputd_gpio_mux(struct inputd_gpio *g, int gpio);
// The pin level: 0 or 1, or -1.
int inputd_gpio_level(struct inputd_gpio *g, int gpio);

#endif

#define _GNU_SOURCE
#include "gpio.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define BANK_STRIDE 0x24u
#define DAT_OFFSET 0x10u
#define BANKS 12

int inputd_gpio_reg(int gpio, struct inputd_gpio_reg *reg) {
    if (gpio < 0 || gpio >= BANKS * 32) return -1;
    int bank = gpio >> 5;
    int pin = gpio & 31;
    uint32_t base = bank == INPUTD_LPIO_BANK ? 0 : (uint32_t)bank * BANK_STRIDE;
    reg->lpio = bank == INPUTD_LPIO_BANK;
    reg->cfg = base + (uint32_t)((pin >> 3) & 3) * 4;
    reg->cfg_shift = (pin & 7) * 4;
    reg->dat = base + DAT_OFFSET;
    reg->dat_bit = pin;
    return 0;
}

static volatile uint32_t *reg32(struct inputd_gpio *g, int lpio, uint32_t offset) {
    volatile uint8_t *block = lpio ? g->lpio : g->pio;
    if (!block || offset + 4 > INPUTD_PIO_MAP) return NULL;
    return (volatile uint32_t *)(block + offset);
}

int inputd_gpio_open(struct inputd_gpio *g, int readonly) {
    g->pio = g->lpio = NULL;
    g->readonly = readonly;
    int fd = open("/dev/mem", (readonly ? O_RDONLY : O_RDWR) | O_SYNC | O_CLOEXEC);
    if (fd < 0) return -1;
    int prot = readonly ? PROT_READ : PROT_READ | PROT_WRITE;
    void *pio = mmap(NULL, INPUTD_PIO_MAP, prot, MAP_SHARED, fd, INPUTD_PIO_BASE);
    void *lpio = mmap(NULL, INPUTD_PIO_MAP, prot, MAP_SHARED, fd, INPUTD_LPIO_BASE);
    close(fd);
    if (pio == MAP_FAILED || lpio == MAP_FAILED) {
        if (pio != MAP_FAILED) munmap(pio, INPUTD_PIO_MAP);
        if (lpio != MAP_FAILED) munmap(lpio, INPUTD_PIO_MAP);
        return -1;
    }
    g->pio = pio;
    g->lpio = lpio;
    return 0;
}

void inputd_gpio_close(struct inputd_gpio *g) {
    if (g->pio) munmap((void *)g->pio, INPUTD_PIO_MAP);
    if (g->lpio) munmap((void *)g->lpio, INPUTD_PIO_MAP);
    g->pio = g->lpio = NULL;
}

int inputd_gpio_input(struct inputd_gpio *g, int gpio) {
    struct inputd_gpio_reg reg;
    if (g->readonly || inputd_gpio_reg(gpio, &reg) < 0) return -1;
    volatile uint32_t *cfg = reg32(g, reg.lpio, reg.cfg);
    if (!cfg) return -1;
    *cfg = *cfg & ~(0xfu << reg.cfg_shift);
    return 0;
}

int inputd_gpio_mux(struct inputd_gpio *g, int gpio) {
    struct inputd_gpio_reg reg;
    if (inputd_gpio_reg(gpio, &reg) < 0) return -1;
    volatile uint32_t *cfg = reg32(g, reg.lpio, reg.cfg);
    return cfg ? (int)((*cfg >> reg.cfg_shift) & 0xf) : -1;
}

int inputd_gpio_level(struct inputd_gpio *g, int gpio) {
    struct inputd_gpio_reg reg;
    if (inputd_gpio_reg(gpio, &reg) < 0) return -1;
    volatile uint32_t *dat = reg32(g, reg.lpio, reg.dat);
    return dat ? (int)((*dat >> reg.dat_bit) & 1) : -1;
}

#define _POSIX_C_SOURCE 200809L
#include "are.h"

#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int range(uint64_t offset, uint64_t length, size_t size) {
    return offset <= size && length <= size - offset;
}

// Read through the ELF symbol's defining section; never assume a firmware build
// address or execute code from this file. Reject absent, ambiguous or invalid data.
static int table_symbol(const unsigned char *file, size_t size, const Elf64_Ehdr *header,
                        const char *name, unsigned char *out, size_t length) {
    int found = 0;
    for (unsigned i = 0; i < header->e_shnum; ++i) {
        Elf64_Shdr section;
        memcpy(&section, file + header->e_shoff + i * sizeof(section), sizeof(section));
        if (section.sh_type != SHT_SYMTAB) continue;
        if (section.sh_entsize != sizeof(Elf64_Sym) || section.sh_size % sizeof(Elf64_Sym) ||
            !range(section.sh_offset, section.sh_size, size) || section.sh_link >= header->e_shnum) return -1;
        Elf64_Shdr strings;
        memcpy(&strings, file + header->e_shoff + section.sh_link * sizeof(strings), sizeof(strings));
        if (strings.sh_type != SHT_STRTAB || !range(strings.sh_offset, strings.sh_size, size)) return -1;
        for (uint64_t pos = 0; pos < section.sh_size; pos += sizeof(Elf64_Sym)) {
            Elf64_Sym symbol;
            memcpy(&symbol, file + section.sh_offset + pos, sizeof(symbol));
            if (symbol.st_name >= strings.sh_size) return -1;
            const char *label = (const char *)file + strings.sh_offset + symbol.st_name;
            if (!memchr(label, 0, strings.sh_size - symbol.st_name)) return -1;
            if (strcmp(label, name)) continue;
            if (found || ELF64_ST_TYPE(symbol.st_info) != STT_OBJECT || symbol.st_size != length ||
                symbol.st_shndx == SHN_UNDEF || symbol.st_shndx >= header->e_shnum) return -1;
            Elf64_Shdr data;
            memcpy(&data, file + header->e_shoff + symbol.st_shndx * sizeof(data), sizeof(data));
            if (data.sh_type != SHT_PROGBITS || !(data.sh_flags & SHF_ALLOC) ||
                (data.sh_flags & (SHF_WRITE | SHF_EXECINSTR)) || symbol.st_value < data.sh_addr ||
                !range(data.sh_offset, data.sh_size, size)) return -1;
            uint64_t relative = symbol.st_value - data.sh_addr;
            if (!range(relative, length, data.sh_size)) return -1;
            memcpy(out, file + data.sh_offset + relative, length);
            found = 1;
        }
    }
    return found ? 0 : -1;
}

static void decode_table(unsigned char *bytes, size_t length) {
    // DNS_0002_Func's rolling 16-bit transform; it is not the hardware cipher.
    uint16_t state = 0xab36;
    for (size_t i = 0; i < length; ++i) {
        unsigned char source = bytes[i];
        bytes[i] ^= (unsigned char)(state >> 8);
        state = (uint16_t)((uint16_t)(source + state) * 0xce6dU + 0x58bfU);
    }
}

int mainui_are_load_tables(const char *stock_elf, struct mainui_are_tables *tables) {
    if (!tables) return -1;
    memset(tables, 0, sizeof(*tables));
    if (!stock_elf) return -1;
    FILE *stream = fopen(stock_elf, "rb");
    if (!stream) return -1;
    int ok = fseek(stream, 0, SEEK_END) == 0;
    long length = ok ? ftell(stream) : -1;
    ok = length >= (long)sizeof(Elf64_Ehdr) && length <= 64 * 1024 * 1024 && fseek(stream, 0, SEEK_SET) == 0;
    unsigned char *file = ok ? malloc((size_t)length) : NULL;
    if (!file) ok = 0;
    if (ok && fread(file, 1, (size_t)length, stream) != (size_t)length) ok = 0;
    if (fclose(stream)) ok = 0;
    Elf64_Ehdr header;
    if (ok) {
        memcpy(&header, file, sizeof(header));
        ok = !memcmp(header.e_ident, ELFMAG, SELFMAG) && header.e_ident[EI_CLASS] == ELFCLASS64 &&
             header.e_ident[EI_DATA] == ELFDATA2LSB && header.e_ident[EI_VERSION] == EV_CURRENT &&
             header.e_machine == EM_AARCH64 && header.e_type == ET_EXEC && header.e_version == EV_CURRENT &&
             header.e_ehsize == sizeof(header) && header.e_shentsize == sizeof(Elf64_Shdr) && header.e_shnum &&
             range(header.e_shoff, (uint64_t)header.e_shnum * sizeof(Elf64_Shdr), (size_t)length);
    }
    struct mainui_are_tables result;
    if (ok) ok = !table_symbol(file, (size_t)length, &header, "DNS_0013_Var", result.first, sizeof(result.first)) &&
                 !table_symbol(file, (size_t)length, &header, "DNS_0001_Var", result.second, sizeof(result.second));
    free(file);
    if (!ok) return -1;
    decode_table(result.first, sizeof(result.first));
    decode_table(result.second, sizeof(result.second));
    *tables = result;
    return 0;
}

static unsigned char compress_bits(unsigned char bits, unsigned char mask) {
    unsigned char result = 0;
    unsigned target = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (mask & (1U << i)) {
            if (bits & (1U << i)) result |= (unsigned char)(1U << target);
            ++target;
        }
    }
    return result;
}

static unsigned char packet_mask(const struct mainui_are_tables *tables, unsigned char nonce) {
    unsigned char mixed = (unsigned char)(tables->first[nonce & 15] + (nonce >> 4));
    unsigned char high = compress_bits(mixed, tables->second[mixed >> 4]);
    unsigned char low = compress_bits(nonce, tables->second[mixed & 15]);
    return (unsigned char)((high << 4) | low);
}

static void decode_packet(const struct mainui_are_tables *tables, const unsigned char *packet,
                          unsigned char *out, size_t length) {
    unsigned char mask = packet[0] ? packet_mask(tables, packet[0]) : 0;
    for (size_t i = 0; i < length; ++i) out[i] = packet[i + 1] ^ mask;
}

void mainui_are_close(struct mainui_are *are) {
    if (are) memset(are, 0, sizeof(*are));
}

int mainui_are_initialize(struct mainui_are *are, const struct mainui_are_tables *tables,
                          const struct mainui_are_transport *transport) {
    if (!are) return -1;
    mainui_are_close(are);
    if (!tables || !transport || !transport->read || !transport->write || !transport->delay || !transport->nonce)
        return -1;
    struct mainui_are candidate = {*tables, *transport, 0};
    unsigned char handshake[6] = {0}, identity[5];
    for (int attempt = 0; attempt < 24; ++attempt) {
        if (transport->delay(transport->context, 1) ||
            transport->read(transport->context, 0x5000, handshake, sizeof(handshake))) return -1;
        if ((handshake[0] & 15) > 4) {
            decode_packet(tables, handshake, identity, sizeof(identity));
            memcpy(candidate.tables.first, identity, sizeof(identity));
            candidate.ready = 1;
            *are = candidate;
            return 0;
        }
    }
    return -1;
}

int mainui_are_block(void *context, const unsigned char input[16], unsigned char output[16], int decrypt) {
    struct mainui_are *are = context;
    unsigned char source[16];
    if (input) memcpy(source, input, sizeof(source));
    if (output) memset(output, 0, 16);
    if (!are || !are->ready || !input || !output || (decrypt != 0 && decrypt != 1)) return -1;
    unsigned char packet[18], response[17] = {0}, decoded[16];
    const struct mainui_are_transport *transport = &are->transport;
    if (transport->nonce(transport->context, packet) || !packet[0]) return -1;
    for (int i = 0; i < 16; ++i) packet[i + 1] = source[15 - i];
    packet[17] = (unsigned char)decrypt;
    unsigned char mask = packet_mask(&are->tables, packet[0]);
    for (int i = 1; i < 18; ++i) packet[i] ^= mask;
    if (transport->write(transport->context, 0x2000, packet, sizeof(packet))) return -1;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (transport->delay(transport->context, 1) ||
            transport->read(transport->context, 0x2000, response, sizeof(response))) return -1;
        if (response[0]) {
            decode_packet(&are->tables, response, decoded, sizeof(decoded));
            for (int i = 0; i < 16; ++i) output[i] = decoded[15 - i];
            return 0;
        }
    }
    return -1;
}

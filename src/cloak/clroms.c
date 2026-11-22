// clroms.c -- see clroms.h. The sizes and CRC-32s are MAME 0.288's
// (cloak.cpp's cloak).
#include "clroms.h"

#include <stddef.h>
#include <string.h>

cl_rom_store_t cl_rom_store;
uint8_t *cl_rom_gfx;

#define G CLROMS_GFX
const cl_rom_file_t cl_rom_files[CL_ROM_FILES] = {
    {"136023-501.bin", 0x2000, 0xc2dbef1bu, 0x0000},
    {"136023-502.bin", 0x2000, 0x316d0c7bu, 0x2000},
    {"136023-503.bin", 0x4000, 0xb9c291a6u, 0x4000},
    {"136023-504.bin", 0x4000, 0xd014a1c0u, 0x8000},
    {"136023-509.bin", 0x2000, 0x46c021a4u, 0xc000},
    {"136023-510.bin", 0x2000, 0x8c9cf017u, 0xe000},
    {"136023-511.bin", 0x2000, 0x66fd8a34u, 0x10000},
    {"136023-512.bin", 0x2000, 0x48c8079eu, 0x12000},
    {"136023-513.bin", 0x2000, 0x13f1cbabu, 0x14000},
    {"136023-514.bin", 0x2000, 0x6f8c7991u, 0x16000},
    {"136023-515.bin", 0x2000, 0x835438a0u, 0x18000},
    {"136023-105.bin", 0x1000, 0xee443909u, G},
    {"136023-106.bin", 0x1000, 0xd708b132u, G + 0x1000},
    {"136023-107.bin", 0x1000, 0xc42c84a4u, G + 0x2000},
    {"136023-108.bin", 0x1000, 0x4fe13d58u, G + 0x3000},
};
#undef G
_Static_assert(offsetof(cl_rom_store_t, slave) == 0xc000 && sizeof(cl_rom_store_t) == 0x1a000, "the store's layout");

uint32_t cl_crc32(uint32_t crc, const uint8_t *p, uint32_t n) {
    static const uint32_t t[16] = {0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
                                   0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
                                   0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ t[crc & 15];
        crc = (crc >> 4) ^ t[crc & 15];
    }
    return ~crc;
}

int clroms_wanted(uint32_t size, uint32_t crc, uint32_t found) {
    for (int i = 0; i < CL_ROM_FILES; i++)
        if (cl_rom_files[i].size == size && cl_rom_files[i].crc == crc && !(found & 1u << i)) return i;
    return -1;
}

bool clroms_size_ok(uint32_t size) {
    for (int i = 0; i < CL_ROM_FILES; i++)
        if (cl_rom_files[i].size == size) return true;
    return false;
}

uint8_t *clroms_place(int i) {
    uint32_t at = cl_rom_files[i].at;
    return at < CLROMS_GFX ? (uint8_t *)&cl_rom_store + at : cl_rom_gfx + (at - CLROMS_GFX);
}

void clroms_put(int i, const uint8_t *data) { memcpy(clroms_place(i), data, cl_rom_files[i].size); }

void clroms_roms(cl_roms_t *r) {
    r->main = cl_rom_store.main;
    r->slave = cl_rom_store.slave;
    r->chars = cl_rom_gfx;
    r->sprites = cl_rom_gfx + 0x2000;
}

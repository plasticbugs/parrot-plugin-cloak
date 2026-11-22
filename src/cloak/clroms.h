// clroms.h -- where the ROMs go: MAME's "cloak" set, each file known by its
// size and CRC-32, never by its name. Portable: the plugin and the Mac tests
// share it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cl.h"

typedef struct {
    const char *name;                   // MAME's name, for messages
    uint32_t size, crc;
    uint32_t at;                        // where its bytes go: in cl_rom_store, or from CLROMS_GFX in cl_rom_gfx
} cl_rom_file_t;

#define CL_ROM_FILES 15                 // (not 136023-116.3n, the vertical timing PROM, which MAME doesn't use)
#define CLROMS_ALL   ((1u << CL_ROM_FILES) - 1)
extern const cl_rom_file_t cl_rom_files[CL_ROM_FILES];

// The CPUs' ROMs stay; the characters and the sprites are read once, at
// cl_init, so they go where the plugin says (cl_rom_gfx: 16K, the chars
// then the sprites), which can be room cl_init clears after it reads them
typedef struct {
    uint8_t main[0xc000], slave[0xe000];
} cl_rom_store_t;
extern cl_rom_store_t cl_rom_store;
#define CLROMS_GFX  ((uint32_t)sizeof(cl_rom_store_t))
#define CLROMS_GFX_SIZE 0x4000
extern uint8_t *cl_rom_gfx;
uint8_t *clroms_place(int i);           // where file i's bytes go

uint32_t cl_crc32(uint32_t crc, const uint8_t *p, uint32_t n);
// which of the files still to find (not in found) has this size and CRC-32 (-1: none)
int clroms_wanted(uint32_t size, uint32_t crc, uint32_t found);
bool clroms_size_ok(uint32_t size);     // one of the files' sizes
// a file's bytes (size of them, its CRC-32 right) to its place
void clroms_put(int i, const uint8_t *data);
// the board's ROMs from the store
void clroms_roms(cl_roms_t *r);

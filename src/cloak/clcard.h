// clcard.h -- the plugin's files on the card, through fsro: the ROMs,
// loose or zipped in /CLOAK (MAME's cloak.zip, as it is) or /CLOAK.ZIP,
// and the high scores in /CLOAK/CLOAK.NV. Portable: the plugin and the Mac
// test (test/card.sh, on disk images) share it. (Pac-Man's pmcard.c and
// Q*bert's qbcard.c.)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../sdplayer/fsro.h"
#include "clroms.h"

#define CLCARD_HOME "/CLOAK"
#define CLCARD_ZIP  "/CLOAK.ZIP"
#define CLCARD_NV   "CLOAK.NV"

// Every file in CLCARD_HOME, then CLCARD_ZIP, until all are found: each
// ROM to its place in cl_rom_store. A zip is never read whole: its
// directory, then each wanted entry's packed bytes into buf and unpacked
// after them. buf: max bytes (the directory, and a packed ROM with the ROM
// after it, must fit); tick: called between sectors (the plugin serves the
// console), or NULL. Returns clroms' found bits.
uint32_t clcard_load_roms(fs_t *fs, uint8_t *buf, uint32_t max, void (*tick)(void));

// The NVRAM (cl_nvram, 512 bytes: the high scores and the operator's
// settings) from its file, before cl_init: false, and cl_nvram untouched,
// if there is no file of that size. The file is MAME's nvram/cloak/nvram,
// byte for byte, so it can be copied either way.
bool clcard_nv_load(fs_t *fs);
// cl_nvram to the file, made if it isn't there (fs needs its writer, and
// the card the /CLOAK folder; a file there of another size is left alone)
bool clcard_nv_save(fs_t *fs);

// license:BSD-3-Clause
// copyright-holders:Dan Boris, Mirko Buffoni, Brad Oliver, Eric Smith, Juergen Buchmueller, Aaron Giles, AJR, Couriersud, Nicola Salmoria
//
// cl.h -- Atari's Cloak & Dagger board (1983) as MAME 0.288 emulates it:
// the master 6502 (the game, the inputs, two POKEYs), the slave 6502 (the
// bitmap), the playfield, the sprites and the palette. cl.c has the details.
#ifndef CL_CL_H
#define CL_CL_H

#include <stdbool.h>
#include <stdint.h>

#define CL_MAIN_CLOCK 1000000
#define CL_SLAVE_CLOCK 1250000
#define CL_POKEY_CLOCK 1250000          // XTAL(10'000'000) / 8
#define CL_W 256
#define CL_H 232                        // the visible rows, 24-255 of the 256
#define CL_Y0 24

typedef struct {
	const uint8_t *main;                // 0x4000-0xffff (48K)
	const uint8_t *slave;               // 0x2000-0xffff (56K)
	const uint8_t *chars;               // 8K: 136023-105, -106
	const uint8_t *sprites;             // 8K: 136023-107, -108 (both read at cl_init only: may be in cl_bm)
} cl_roms_t;

// the inputs as MAME's ports read them (P1 and SYSTEM active low, START active high)
typedef struct { uint8_t p1, system, start, dsw; } cl_inputs_t;
#define CL_P1 0xff                      // bits 0-3 right stick D U R L (fire), 4-7 left stick (move)
#define CL_SYSTEM 0xff                  // bit 0 VBLANK (the board's), 1 service, 2 coin 2, 3 coin 1, 7 igniter
#define CL_START 0x0f                   // 0x80 start 1, 0x40 start 2
#define CL_DSW 0x02                     // 1 credit 1 game
extern cl_inputs_t cl_in;

extern uint8_t cl_ram[0x400], cl_vram[0x400], cl_shared[0x800], cl_nvram[0x200], cl_spr[0x100];
extern uint8_t cl_sram[0x800];          // the slave's RAM (0x0008-0x000f is the graph processor)
extern uint16_t cl_pal[64];             // the palette RAM, 9 bits, active low
extern uint8_t cl_bm[2][0x8000];        // the two bitmaps, 4 bits a pixel: (y << 7) | (x >> 1), even x low
extern uint8_t cl_bm_sel, cl_bm_x, cl_bm_y;
// the shown bitmap changes only when the other is shown (the slave draws in
// the one not shown): a count of those, and of cl_init's
extern uint32_t cl_bm_shows;
extern uint8_t cl_latch;                // the LS259 at 10B: bit 3 flips the screen
extern uint32_t cl_frames, cl_resets;
extern uint32_t cl_unsupported;        // bits: what the game did that this doesn't model (0: none)
extern uint32_t cl_rgb[64];             // the palette as MAME's pens (0xRRGGBB)

void cl_init(const cl_roms_t *r);       // power on; cl_nvram is kept as it is (the card's file)
bool cl_frame(void);                    // run to the next VBLANK; false if the watchdog reset the board
uint32_t cl_run_to(uint64_t until);     // run to the master's cycle until: the VBLANKs passed
uint64_t cl_time(void);                 // the board's time, in master cycles
uint64_t cl_main_cycles(void), cl_slave_cycles(void);

// the picture: pens 0-63 (cl_rgb) a pixel, the screen's 256 rows (24-255 shown)
typedef uint8_t cl_fb_t[256][CL_W];
void cl_render(cl_fb_t fb, int y0, int y1);     // rows y0-y1 as the board would draw them now
// the characters' and sprites' rows as decoded from their ROMs: 8 pens a
// row, x 0 in the low nibble (cl_render's, clcv's)
extern uint32_t cl_char_rows[256][8];
extern uint32_t cl_sprite_rows[128][16];
// MAME draws the rows down to where the beam is when the slave flips its
// bitmaps (update_partial), and the rest at VBLANK: with cl_frame_fb set
// (the tests), it gets the picture as MAME's screen has it at each VBLANK
extern cl_fb_t *cl_frame_fb;

// the cart's picture (clcv.c): called at each VBLANK, after MAME's screen
// has its rows
extern void (*cl_on_vblank)(void);
uint32_t cl_pal_rgb(uint16_t pal);      // a palette RAM word's color (0xRRGGBB)

// counts for the note and the tests
typedef struct { uint32_t slices, flips; } cl_stats_t;
extern cl_stats_t cl_stats;

// the CPUs' state, for the tests
typedef struct { uint16_t pc, sp; uint8_t a, x, y, p; } cl_regs_t;
void cl_regs(int cpu, cl_regs_t *r);    // 0 master, 1 slave
// a test's per-instruction hook: the CPU, its registers and total cycles (MAME's trace)
extern void (*cl_trace)(int cpu, const cl_regs_t *r, uint64_t cycles);

// the POKEYs' sound: each one's output level (MAME's OPAMP_LOW_PASS V0, the
// sum of its channels' conductances) summed into bins of its 1.25 MHz
// samples, PS_RATE_Q8 bins a second
#define CL_SND_ONE 65536                // the level of 1.0 (MAME's V0), in sums
void cl_audio_rate_q8(uint32_t hz_q8);
int cl_audio_take(int32_t *sum1, int32_t *sum2, uint16_t *count, int max);
// per POKEY the stats the note shows, and the register writes for tests
typedef struct { uint8_t audf[4], audc[4], audctl, skctl; uint32_t out_raw; } cl_pokey_regs_t;
void cl_pokey_regs(int n, cl_pokey_regs_t *r);
extern void (*cl_pokey_hook)(int n, uint64_t clock, uint32_t out_raw);  // each output change (tests)
extern bool cl_pokey_exact;             // step every clock (the tests' reference)

#endif

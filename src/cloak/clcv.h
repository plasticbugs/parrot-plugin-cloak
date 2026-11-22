// clcv: the cart's side of Cloak & Dagger on the ColecoVision. Portable C
// (no pico-sdk), used by the plugin and by the Mac tests.
//
// The console runs gbkernel (coleco/gbcv/kernel/gbkernel.asm): each TV
// frame it takes a command list from the cart and runs it. This makes those
// lists from the board (cl.h):
//
//   once:                       clcv_init(e), then cl_on_vblank pointed at
//                               a hook that calls clcv_vblank (clrun.c does)
//   after a VBLANK:             clcv_picture(e)  (or clcv_canvas for a message)
//   each list:                  clcv_sound(e, samples, CLCV_PCM_N)
//                               len = clcv_tv_frame(e, list)
//
// The picture. MAME shows rows 24-255 of the board's 256 x 256: 232 rows,
// 40 more than the TMS9918's 192. Each 8-row band of the board's (a row of
// its characters, the playfield's grid) gives 7 rows, or 6 for 11 bands of
// the 29: two rows go into one at the band's middle (rows 3 and 4; or 1 and
// 2, 5 and 6), keeping a pixel of either that stands out from the rows
// around it, so lines and the text's strokes one pixel high stay. The
// board's 64 colors (its palette RAM, changed by the game) go to the
// TMS9918's nearest in OKLab; Graphics II shows two colors a run of 8 x 1.
// No TMS9918 sprites: the board's sprites are drawn into the tiles.
//
// A picture is the playfield, the sprites and the colors as they were at
// the last VBLANK, and the bitmap shown as it is when the picture is made
// (the slave flips to a whole new scene, so it is the newest one, where
// MAME may show the rows above a flip from the scene before). It is made
// only where the board changed since the last one: the bitmap's 8-pixel
// runs (known by a hash of each), the playfield's characters (and those
// with a recolored pen), the sprites where they were and are, and
// everything when the bitmap's colors or the screen's flip change. At most
// cells_max of the 6,144 runs a picture (clrun's credit, at most
// CLCV_CELLS), from the row after the last one made; the rest go to the next.
//
// A list carries the bytes that differ from what the console holds, as
// many as the frame has time for (about 1,800).
//
// The sound. The two POKEYs (cl.c's bins), each through its OPAMP_LOW_PASS
// as MAME mixes them, go out as CLCV_PCM_N samples a list through two of
// the SN76489's tones parked at a period of 1, their two attenuators summed
// as a DAC (Q*bert's and Pac-Man's way): the POKEYs' noise and their
// pitches both come through, which mapping their channels onto the PSG's
// three tones and its noise could not do. The console runs the lists back
// to back, so a list's length is the sample clock: CLCV_LIST_T T, 100
// samples, 5,962 a second. Every list sets the whole PSG.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cl.h"

#define CLCV_W      256
#define CLCV_H      192
#define CLCV_BYTES  6144            // pattern bytes, and as many color bytes
#define CLCV_LIST_MAX 3000
#define CLCV_PCM_N  100             // samples a list
#define CLCV_LIST_T 60036           // a list's length in the console's T (3,579,545 a second)
#define CLCV_SRC_H  CL_H            // the board's rows shown (232)
// the most 8 x 1 runs a picture makes again; a whole new screen takes
// four pictures, while its bytes take seven lists to send anyway
#define CLCV_CELLS  1536
// the board's master cycles (1 MHz) in a list, << 16
#define CLCV_LIST_CYCLES_Q16 ((uint64_t)CLCV_LIST_T * CL_MAIN_CLOCK * 65536u / 3579545u)
// the samples' rate, << 8: CLCV_PCM_N a list
#define CLCV_RATE_Q8 ((uint32_t)((uint64_t)CLCV_PCM_N * 3579545u * 256u / CLCV_LIST_T))

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define CLCV_HOT __attribute__((section(".time_critical.clcv")))
#define CLCV_DATA __attribute__((section(".time_critical.clcv_data")))    // a hot table (m6502.c's M65_DATA)
#else
#define CLCV_HOT
#define CLCV_DATA
#endif

typedef struct {
    uint32_t tv_frames, pictures, pat_bytes, col_bytes, runs, refreshed, pcm_frames;
    uint32_t settled;               // lists that ended with nothing left to send
    uint32_t t_max;                 // the longest list, in T
    uint32_t cells;                 // 8 x 1 runs made again
    uint32_t deferred;              // pictures that left runs to the next
    uint32_t clipped;               // samples the limiter took in
    uint32_t recolored;             // pictures made whole again for a color or the flip
} clcv_stats_t;

// the board's state at a VBLANK, but for the bitmap
typedef struct {
    uint8_t vram[0x400], spr[0x100];
    uint32_t rgb[64];               // the pens (cl_rgb)
    uint8_t latch;
} clcv_board_t;

typedef struct {
    // the console's VRAM as the lists so far leave it, and the newest picture
    uint8_t vram_pat[CLCV_BYTES], vram_col[CLCV_BYTES];
    uint8_t want_pat[CLCV_BYTES], want_col[CLCV_BYTES];
    bool regs_done;
    bool names_done;
    uint16_t names_at;
    uint16_t cursor;                // where the next list's scan starts
    uint16_t refresh_at;
    bool dirty;                     // want differs from vram somewhere (or may)
    uint32_t bands;                 // the same, for each band of 8 lines (256 bytes)

    // the picture: the board at the last VBLANK, and as the last picture had it
    clcv_board_t now, last;
    bool fresh;                     // a VBLANK since the last picture
    bool pic_valid;                 // last (and the TMS9918's colors) as made
    uint8_t tms[64];                // the pens' TMS9918 colors, as the last picture made them
    uint32_t src_dirty[CLCV_SRC_H]; // the board's 8-pixel runs changed (a bit a column)
    uint32_t pending[CLCV_H];       // the console's runs left for the next picture
    uint8_t row_next;               // the row the next picture starts at
    uint16_t cells_max;             // the most runs the next picture makes (clcv_init: CLCV_CELLS)

    // the sound
    bool pcm;                       // the lists carry samples (the game is on)
    uint8_t pcm_gain;               // 0-32: the DAC comes up over the first lists
    int32_t hp_x, hp_y;             // the high-pass's last input and output (<< 8)
    uint8_t pcm_bytes[CLCV_PCM_N][3];  // each sample's three attenuators, in the order written
    uint8_t dac_att[3];             // the attenuators as the last sample left them
    uint8_t tone_bytes[12];
    uint8_t tone_n;
    uint8_t resync;
    clcv_stats_t stats;
} clcv_t;

void clcv_init(clcv_t *e);
// at VBLANK: the playfield, the sprites, the palette and the flip
void clcv_vblank(clcv_t *e);
// a new picture of the board since the last VBLANK; false: it left runs for the next
bool clcv_picture(clcv_t *e);
// a new picture: 256 x 192 pixels, a TMS9918 color each (1-15); for messages
void clcv_canvas(clcv_t *e, const uint8_t *px);
// room the plugin may use before cl_init (a message's canvas, the card's
// reads): the board's bitmaps, which cl_init clears
uint8_t *clcv_scratch(size_t *n);

// The next list's sound: n samples at CLCV_RATE_Q8, MAME's +-1.0 as
// +-32767; n = 0: none (a message: the console waits for the vertical
// blank again, and the DAC is let go)
void clcv_sound(clcv_t *e, const int16_t *s, int n);
// the POKEYs' bins (cl_audio_take's) as MAME mixes them: their filters, half each
typedef struct { int32_t f1, f2; } clcv_mix_t;     // the filters' outputs, 1.0 = 65536
int16_t clcv_mix(clcv_mix_t *m, int32_t sum1, int32_t sum2, uint16_t count);

// the next list into out (CLCV_LIST_MAX bytes); returns its length
size_t clcv_tv_frame(clcv_t *e, uint8_t *out);

// tests and previews
// the picture the console shows once the lists so far have run, a TMS9918 color a pixel
void clcv_screen(const clcv_t *e, uint8_t *px);
// the picture wanted, the same way
void clcv_wanted(const clcv_t *e, uint8_t *px);
// the picture before the TMS9918's two colors a run: every pixel its own (256 x 192)
void clcv_ideal(const clcv_t *e, uint8_t *px);
// the board's picture as clcv has it (232 x 256 pens, MAME's rows 24-255)
void clcv_source(const clcv_t *e, uint8_t *pens);
// the rows of the board's a row of the console's comes from (two: merged)
void clcv_rows_of(int y, int *s0, int *s1);
// bytes the console still needs
uint32_t clcv_backlog(const clcv_t *e);
float clcv_dac_level(int att0, int att1, int att2);
extern const uint32_t clcv_tms_rgb[16];

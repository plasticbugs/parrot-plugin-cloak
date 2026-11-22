// clcv.c -- see clcv.h. The lists and the sound are pmcv.c's (../pacman,
// from qbcv.c), the picture is new.
#include "clcv.h"

#include <math.h>
#include <string.h>

#include "cldac3.h"

// ---- lists (qbcv.c's, from gbkernel.asm's costs) ---------------------------------
#define FRAME_T    (262 * 228)
#define VBLANK_T   (70 * 228)
#define C_START    480
#define C_RUN_VB   106
#define C_BYTE_VB  23
#define C_RUN_ACT  106
#define C_BYTE_ACT 31
#define C_PSG      75
#define C_PSG_BYTE 52
#define C_REG      114
#define C_END_VB   54
#define C_END      52
#define VB_LIMIT   (VBLANK_T - 600)
#define LIST_LIMIT (FRAME_T - 1200)

// The console goes from one list straight into the next, so a list's
// length is the sample clock (qbcv.c's)
#define C_FRAME    357
#define PCM_L      CLCV_LIST_T
#define PCM_FIRST  (C_FRAME + C_END_VB)
#define C_SAMPLE   (C_PSG + 3 * C_PSG_BYTE)
#define PCM_STEP   (((PCM_L - PCM_FIRST - C_SAMPLE - C_END) << 8) / (CLCV_PCM_N - 1))

#define PATTERNS 0x0000
#define COLORS   0x2000
#define NAMES    0x1800
#define BLACK    1

typedef struct {
    uint8_t *b;
    size_t n;
    uint32_t t;
    bool vblank;
} list_t;

static CLCV_HOT void l_run_head(list_t *l, uint16_t addr, int n) {
    l->t += l->vblank ? C_RUN_VB + n * C_BYTE_VB : C_RUN_ACT + n * C_BYTE_ACT;
    l->b[l->n++] = 1;
    l->b[l->n++] = (uint8_t)addr;
    l->b[l->n++] = (uint8_t)((addr >> 8) & 0x3F) | 0x40;
    l->b[l->n++] = (uint8_t)n;
}

static CLCV_HOT void l_psg(list_t *l, const uint8_t *d, int n) {
    l->t += C_PSG + C_PSG_BYTE * n;
    l->b[l->n++] = 2;
    l->b[l->n++] = (uint8_t)n;
    memcpy(l->b + l->n, d, n);
    l->n += n;
}

static CLCV_HOT void l_reg(list_t *l, uint8_t r, uint8_t v) {
    l->t += C_REG;
    l->b[l->n++] = 3;
    l->b[l->n++] = v;
    l->b[l->n++] = 0x80 | r;
}

static CLCV_HOT void l_end_vblank(list_t *l) {
    if (!l->vblank) return;
    l->vblank = false;
    l->t += C_END_VB;
    l->b[l->n++] = 0;
}

static CLCV_HOT int room(const list_t *l) {
    int t = l->vblank ? ((int)VB_LIMIT - (int)l->t - C_END_VB - C_RUN_VB) / C_BYTE_VB
                      : ((int)LIST_LIMIT - (int)l->t - C_END - C_RUN_ACT) / C_BYTE_ACT;
    return t > 255 ? 255 : t;
}

// ---- colors -----------------------------------------------------------------------
const uint32_t clcv_tms_rgb[16] = {0x000000, 0x000000, 0x21C842, 0x5EDC78, 0x5455ED, 0x7D76FC, 0xD4524D, 0x42EBF5,
                                   0xFC5554, 0xFF7978, 0xD4C154, 0xE6CE80, 0x21B03B, 0xC95BBA, 0xCCCCCC, 0xFFFFFF};

// The board's colors are 3 bits a primary through its resistor weights
// (cl_pal_rgb): 512 of them, each to the TMS9918's nearest in OKLab
static float tms_lab[16][3];
static uint16_t nearer[16][16];         // [c][a] bit b: c is nearer a than b
static uint8_t tms_of9[512];            // a 9-bit color's TMS9918 color
static uint8_t level_of[256];           // a primary's 0-255 back to its 3 bits
static bool colors_ready;

static void oklab(uint32_t rgb, float *out) {
    float c[3];
    for (int i = 0; i < 3; i++) {
        float v = (float)((rgb >> (16 - 8 * i)) & 255) / 255.0f;
        c[i] = v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
    }
    float l = cbrtf(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    float m = cbrtf(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    float s = cbrtf(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    out[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    out[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    out[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

static float d2(const float *a, const float *b) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

static void colors_init(void) {
    for (int i = 1; i < 16; i++) oklab(clcv_tms_rgb[i], tms_lab[i]);
    for (int c = 1; c < 16; c++)
        for (int a = 1; a < 16; a++)
            for (int b = 1; b < 16; b++)
                if (d2(tms_lab[c], tms_lab[a]) <= d2(tms_lab[c], tms_lab[b])) nearer[c][a] |= (uint16_t)(1u << b);
    for (int v = 0; v < 512; v++) {
        uint32_t rgb = cl_pal_rgb((uint16_t)~v);
        float lab[3];
        oklab(rgb, lab);
        int best = BLACK;
        float bd = 1e9f;
        for (int t = 1; t < 16; t++) {
            float d = d2(lab, tms_lab[t]);
            if (d < bd) { bd = d; best = t; }
        }
        tms_of9[v] = (uint8_t)best;
        level_of[(rgb >> 16) & 255] = (uint8_t)((v >> 6) & 7);
        level_of[(rgb >> 8) & 255] = (uint8_t)((v >> 3) & 7);
        level_of[rgb & 255] = (uint8_t)(v & 7);
    }
    colors_ready = true;
}

static inline CLCV_HOT uint8_t tms_of_rgb(uint32_t rgb) {
    return tms_of9[level_of[(rgb >> 16) & 255] << 6 | level_of[(rgb >> 8) & 255] << 3 | level_of[rgb & 255]];
}

// ---- the rows: 232 of the board's into 192 ------------------------------------------
// Each 8-row band of the board's gives 7 rows (rows 3 and 4 as one), or 6
// (1 and 2, 5 and 6) for 11 bands of the 29, spread evenly
static uint8_t out_s0[CLCV_H], out_s1[CLCV_H];
static uint8_t src_out[CLCV_SRC_H];     // the console's row a board's row is in
static bool rows_ready;

static void rows_init(void) {
    int y = 0;
    for (int band = 0; band < CLCV_SRC_H / 8; band++) {
        bool two = (band + 1) * 11 / 29 > band * 11 / 29;
        for (int k = 0; k < 8; k++) {
            int s = band * 8 + k;
            bool second = two ? (k == 2 || k == 6) : k == 4;    // merged into the row before
            if (second) {
                out_s1[y - 1] = (uint8_t)s;
                src_out[s] = (uint8_t)(y - 1);
                continue;
            }
            out_s0[y] = out_s1[y] = (uint8_t)s;
            src_out[s] = (uint8_t)y;
            y++;
        }
    }
    rows_ready = true;
}

void clcv_rows_of(int y, int *s0, int *s1) {
    if (!rows_ready) rows_init();
    *s0 = out_s0[y];
    *s1 = out_s1[y];
}

// ---- the bitmap ------------------------------------------------------------------
// The picture takes the bitmap shown as it is when the picture is made (the
// front one, cl_bm[!cl_bm_sel]: the slave flips to a whole new scene), and
// knows the runs it changed by a hash of each one's 8 pixels (4 bytes) as
// the last picture had them: 15K, where a copy would take 30K. Two contents
// with one hash (1 in 65,536) would leave a run as it was, so each picture
// also makes one row's runs again, the whole screen in 232 pictures. A
// row's runs are looked at only when the row's own hash (FNV-1a of its
// words, which any one word's change changes) did change.
static uint16_t run_hash[CLCV_SRC_H][32];
static uint32_t row_hash[CLCV_SRC_H];
static uint8_t heal_row;
static uint32_t shows_seen;             // cl_bm_shows at the last look (the bitmap can't change without one)

uint8_t *clcv_scratch(size_t *n) {
    *n = sizeof cl_bm;                  // (the board clears it at cl_init)
    return &cl_bm[0][0];
}

// a screen run's 8 pixels: the screen's pixel X is the bitmap's X + 6 (even
// X: a byte's low nibble) or X + 5 (odd: the high), so run cx is bytes
// 4cx + 3 to 4cx + 6 of the row (the last run wraps to the row's start)
static inline CLCV_HOT uint32_t run_bytes(const uint8_t *row, int cx) {
    if (cx < 31) {
        uint32_t w;
        memcpy(&w, row + 4 * cx + 3, 4);
        return w;
    }
    return row[127] | (uint32_t)row[0] << 8 | (uint32_t)row[1] << 16 | (uint32_t)row[2] << 24;
}

// the bitmap's runs changed since the last picture, into src_dirty
static CLCV_HOT void bitmap_changes(clcv_t *e) {
    const uint8_t *bm = cl_bm[!cl_bm_sel] + (CL_Y0 << 7);
    for (int s = 0; s < CLCV_SRC_H && shows_seen != cl_bm_shows; s++, bm += 128) {
        const uint32_t *w = (const uint32_t *)(const void *)bm;
        uint32_t rh = 2166136261u;
        for (int k = 0; k < 32; k += 4)
            rh = ((((rh ^ w[k]) * 16777619u ^ w[k + 1]) * 16777619u ^ w[k + 2]) * 16777619u ^ w[k + 3]) * 16777619u;
        if (rh == row_hash[s]) continue;
        row_hash[s] = rh;
        uint16_t *h = run_hash[s];
        uint32_t changed = 0;
        for (int cx = 0; cx < 32; cx++) {
            uint16_t v = (uint16_t)((run_bytes(bm, cx) * 0x9E3779B1u) >> 16);
            if (v != h[cx]) {
                h[cx] = v;
                changed |= 1u << cx;
            }
        }
        e->src_dirty[s] |= changed;
    }
    shows_seen = cl_bm_shows;
    e->src_dirty[heal_row] = ~0u;
    heal_row = (uint8_t)(heal_row + 1 == CLCV_SRC_H ? 0 : heal_row + 1);
}

CLCV_HOT void clcv_vblank(clcv_t *e) {
    memcpy(e->now.vram, cl_vram, sizeof e->now.vram);
    memcpy(e->now.spr, cl_spr, sizeof e->now.spr);
    memcpy(e->now.rgb, cl_rgb, sizeof e->now.rgb);
    e->now.latch = cl_latch;
    e->fresh = true;
}

// ---- the board's picture, 8 pixels at a time -------------------------------------
// The sprites on each of the board's rows (a bit each, sprite k at bit k),
// as cl_render draws them: 63 first, 0 last (in front)
// and on each run's 8 columns (the sprites parked off the game's screen sit
// on its last 16 rows, so a row can have most of the 64 on it)
static uint64_t row_sprites[256], run_sprites[32];

static CLCV_HOT void sprite_rows(const clcv_board_t *b) {
    memset(row_sprites, 0, sizeof row_sprites);
    memset(run_sprites, 0, sizeof run_sprites);
    bool flip = (b->latch >> 3) & 1;
    for (int k = 0; k < 64; k++) {
        int sy = 240 - b->spr[k], sx = b->spr[k + 192];
        if (flip) { sy = 240 - sy; sx -= 9; }
        for (int j = 0; j < 16; j++) {
            int y = sy + j;
            if (y >= 0 && y < 256) row_sprites[y] |= 1ull << k;
        }
        for (int x = sx; x < sx + 8; x += 7)    // its first column's run and its last's
            if (x >= 0 && x <= 255) run_sprites[x >> 3] |= 1ull << k;
    }
}

// pens of the board's row s (0 = MAME's row 24), pixels 8cx to 8cx + 7, as cl_render draws them
CLCV_HOT static void src_pens(const clcv_board_t *b, int s, int cx, uint8_t *p) {
    int y = s + CL_Y0;
    bool flip = (b->latch >> 3) & 1;
    int ty = flip ? 255 - y : y;
    if (!flip) {
        uint32_t r = cl_char_rows[b->vram[(ty >> 3) * 32 + cx]][ty & 7];
        for (int x = 0; x < 8; x++) p[x] = (uint8_t)((r >> (4 * x)) & 15);
    } else {
        uint32_t r = cl_char_rows[b->vram[(ty >> 3) * 32 + (31 - cx)]][ty & 7];
        for (int x = 0; x < 8; x++) p[x] = (uint8_t)((r >> (4 * (7 - x))) & 15);
    }
    // the bitmap (run_bytes), x 128 and up (the bitmap's) its second 8 colors
    uint32_t w = run_bytes(cl_bm[!cl_bm_sel] + (y << 7), cx);
    for (int i = 0; i < 8; i++, w >>= 4) {
        uint8_t v = (uint8_t)(w & 7);
        int X = cx * 8 + i, x = (X + 6 - (X & 1)) & 0xff;
        if (v) p[i] = (uint8_t)(0x10 | ((x & 0x80) >> 4) | v);
    }
    uint64_t m = row_sprites[y] & run_sprites[cx];
    while (m) {
        int k = 63 - __builtin_clzll(m);
        m &= ~(1ull << k);
        int code = b->spr[k + 64] & 0x7f;
        bool fx = b->spr[k + 64] & 0x80, fy = false;
        int sx = b->spr[k + 192], sy = 240 - b->spr[k];
        if (flip) { sx -= 9; sy = 240 - sy; fx = !fx; fy = true; }
        int i0 = sx - cx * 8;
        if (i0 <= -8 || i0 >= 8) continue;
        int j = y - sy;
        uint32_t r = cl_sprite_rows[code][fy ? 15 - j : j];
        for (int i = 0; i < 8; i++) {
            int px = sx + i, at = px - cx * 8;
            if (at < 0 || at > 7 || px > 255) continue;
            int pen = (r >> (4 * (fx ? 7 - i : i))) & 15;
            if (pen) p[at] = (uint8_t)(32 + pen);
        }
    }
}

// the TMS9918 colors of those pixels: src_pens's pens through e->tms, the
// playfield and the bitmap in one pass where no sprite is on the run
static CLCV_DATA const uint8_t bank_hi[32] = {   // the pixels with the bitmap's x 128 and up
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xfc,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03};

static inline CLCV_HOT void src_tms(const clcv_t *e, const clcv_board_t *b, int s, int cx, uint8_t *c) {
    int y = s + CL_Y0;
    if (row_sprites[y] & run_sprites[cx]) {
        uint8_t p[8];
        src_pens(b, s, cx, p);
        for (int i = 0; i < 8; i++) c[i] = e->tms[p[i]];
        return;
    }
    bool flip = (b->latch >> 3) & 1;
    int ty = flip ? 255 - y : y;
    uint32_t r = cl_char_rows[b->vram[(ty >> 3) * 32 + (flip ? 31 - cx : cx)]][ty & 7];
    if (flip) {                         // its 8 pens the other way round
        r = (r >> 16) | (r << 16);
        r = ((r >> 8) & 0x00ff00ffu) | ((r & 0x00ff00ffu) << 8);
        r = ((r >> 4) & 0x0f0f0f0fu) | ((r & 0x0f0f0f0fu) << 4);
    }
    uint32_t w = run_bytes(cl_bm[!cl_bm_sel] + (y << 7), cx);
    unsigned hi = bank_hi[cx];
    const uint8_t *t = e->tms;
    for (int i = 0; i < 8; i++, w >>= 4, r >>= 4, hi >>= 1) {
        unsigned v = w & 7;
        c[i] = v ? t[0x10 | (hi & 1) << 3 | v] : t[r & 15];
    }
}

// the console's row y, run cx: the board's row, or two of its rows as one
// (a pixel of the second where it stands out from the rows about it and
// the first's doesn't)
CLCV_HOT static void out_run(const clcv_t *e, const clcv_board_t *b, int y, int cx, uint8_t *o) {
    int s0 = out_s0[y], s1 = out_s1[y];
    src_tms(e, b, s0, cx, o);
    if (s1 == s0) return;
    uint8_t a[8], c[8], d[8];
    src_tms(e, b, s1, cx, c);
    if (s0 > 0) src_tms(e, b, s0 - 1, cx, a);
    else memcpy(a, o, 8);
    if (s1 + 1 < CLCV_SRC_H) src_tms(e, b, s1 + 1, cx, d);
    else memcpy(d, c, 8);
    for (int i = 0; i < 8; i++) {
        bool b_thin = o[i] != a[i] && o[i] != c[i], c_thin = c[i] != o[i] && c[i] != d[i];
        if (c_thin && !b_thin) o[i] = c[i];
    }
}

// ---- 8 TMS9918 colors as a pattern byte and a color byte (pmcv.c's span) -------------
// kept as the console has it where that will do
static CLCV_HOT inline __attribute__((always_inline)) void span(clcv_t *e, unsigned a, const uint8_t *c) {
    uint8_t vc = e->vram_col[a], vp = e->vram_pat[a];
    uint8_t c0 = c[0];
    if (c[1] == c0 && c[2] == c0 && c[3] == c0 && c[4] == c0 && c[5] == c0 && c[6] == c0 && c[7] == c0) {
        uint8_t vf = vc >> 4, vb = vc & 15;
        if (vf == c0 || vb == c0) {
            e->want_col[a] = vc;
            e->want_pat[a] = vf == vb ? vp : vf == c0 ? 0xFF : 0x00;
        } else {
            e->want_col[a] = (uint8_t)(c0 << 4 | c0);
            e->want_pat[a] = vp;
        }
        return;
    }
    {                                   // two colors (the general case's answer)
        uint8_t d = 0;
        unsigned m0 = 0;
        int i = 0;
        for (; i < 8; i++) {
            uint8_t x = c[i];
            if (x == c0) m0 |= 0x80u >> i;
            else if (d && x != d) break;
            else d = x;
        }
        if (i == 8) {
            static CLCV_DATA const uint8_t pop4[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
            int n0 = pop4[m0 & 15] + pop4[m0 >> 4];
            uint8_t a1 = n0 > 4 ? c0 : n0 < 4 ? d : (c0 < d ? c0 : d), a2 = a1 == c0 ? d : c0;
            uint8_t vf = vc >> 4, vb = vc & 15, fg, bg;
            if (vf == a1 && vb == a2) { fg = a1; bg = a2; }
            else { fg = a2; bg = a1; }
            e->want_pat[a] = (uint8_t)(fg == c0 ? m0 : ~m0 & 0xFF);
            e->want_col[a] = (uint8_t)(fg << 4 | bg);
            return;
        }
    }
    uint8_t cnt[16] = {0};
    for (int i = 0; i < 8; i++) cnt[c[i]]++;
    // the two most used (ties: the lower color)
    uint8_t a1 = 0, a2 = 0;
    for (int k = 1; k < 16; k++)
        if (cnt[k] > cnt[a1] || (!a1 && cnt[k])) a1 = (uint8_t)k;
    for (int k = 1; k < 16; k++)
        if (k != a1 && cnt[k] && (!a2 || cnt[k] > cnt[a2])) a2 = (uint8_t)k;
    uint8_t fg, bg;
    uint8_t vf = vc >> 4, vb = vc & 15;
    if (vf == a1 && vb == a2) { fg = a1; bg = a2; }
    else if (vf == a2 && vb == a1) { fg = a2; bg = a1; }
    else { fg = a2; bg = a1; }
    uint8_t pat = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t x = c[i];
        bool on = x == fg || (x != bg && ((nearer[x][fg] >> bg) & 1));
        pat = (uint8_t)(pat << 1 | on);
    }
    e->want_pat[a] = pat;
    e->want_col[a] = (uint8_t)(fg << 4 | bg);
}

static inline CLCV_HOT unsigned run_at(int y, int cx) { return (unsigned)((y >> 3) * 256 + cx * 8 + (y & 7)); }

// ---- the picture -------------------------------------------------------------------
// the pens each character has (a bit each, 0 not counted: every one has it)
static uint16_t char_pens[256];

static void char_pens_init(void) {
    for (int c = 0; c < 256; c++) {
        uint16_t m = 0;
        for (int r = 0; r < 8; r++)
            for (int x = 0; x < 8; x++) m |= (uint16_t)(1u << ((cl_char_rows[c][r] >> (4 * x)) & 15));
        char_pens[c] = m & 0xfffe;
    }
}

// a sprite's 8 x 16 as the board's rows it covers (s0..s1) and its runs (a bit each)
static CLCV_HOT void sprite_mark(clcv_t *e, const clcv_board_t *b, int k) {
    bool flip = (b->latch >> 3) & 1;
    int sx = b->spr[k + 192], sy = 240 - b->spr[k];
    if (flip) { sx -= 9; sy = 240 - sy; }
    uint32_t m = 0;
    for (int x = sx; x < sx + 8; x += 7)
        if (x >= 0 && x <= 255) m |= 1u << (x >> 3);
    if (!m) return;
    for (int y = sy; y < sy + 16; y++) {
        int s = y - CL_Y0;
        if (s >= 0 && s < CLCV_SRC_H) e->src_dirty[s] |= m;
    }
}

CLCV_HOT bool clcv_picture(clcv_t *e) {
    if (!colors_ready) colors_init();
    if (!rows_ready) rows_init();
    if (!e->fresh) return true;
    e->fresh = false;
    const clcv_board_t *b = &e->now;
    // the colors (of the pens drawn: not 16, 24 and 32, transparent, nor
    // 48-63) and the flip. A playfield pen's new color: the characters that
    // have it; a sprite pen's: the sprites; the rest (the background, the
    // bitmap's, the flip): everything
    uint8_t tms[64];
    uint64_t changed = 0;
    for (int p = 0; p < 64; p++) {
        tms[p] = tms_of_rgb(b->rgb[p]);
        if (tms[p] != e->tms[p]) changed |= 1ull << p;
    }
    changed &= ~(1ull << 16 | 1ull << 24 | 1ull << 32 | 0xffffull << 48);
    memcpy(e->tms, tms, sizeof tms);
    if (!e->pic_valid) char_pens_init();
    bool flip = (b->latch >> 3) & 1;
    if (!e->pic_valid || ((b->latch ^ e->last.latch) & 8) || (changed & (1ull | 0xffffull << 16))) {
        e->stats.recolored++;
        for (int s = 0; s < CLCV_SRC_H; s++) e->src_dirty[s] = ~0u;
    } else {
        // the playfield's characters: those changed, and those with a pen recolored
        uint16_t pf = (uint16_t)changed;
        for (int i = 0; i < 0x400; i += 4) {
            uint32_t w, v;
            memcpy(&w, b->vram + i, 4);
            memcpy(&v, e->last.vram + i, 4);
            if (w == v && !(pf & (char_pens[b->vram[i]] | char_pens[b->vram[i + 1]] | char_pens[b->vram[i + 2]] |
                                  char_pens[b->vram[i + 3]])))
                continue;
            for (int k = i; k < i + 4; k++) {
                if (b->vram[k] == e->last.vram[k] && !(pf & char_pens[b->vram[k]])) continue;
                int r = k >> 5, col = k & 31;
                int cx = flip ? 31 - col : col;
                for (int t = r * 8; t < r * 8 + 8; t++) {
                    int y = flip ? 255 - t : t, s = y - CL_Y0;
                    if (s >= 0 && s < CLCV_SRC_H) e->src_dirty[s] |= 1u << cx;
                }
            }
        }
        // the sprites that moved or changed (all, for a sprite pen's new
        // color): where they were and where they are
        bool spr = (changed >> 32) != 0;
        for (int k = 0; k < 64; k++)
            if (spr || b->spr[k] != e->last.spr[k] || b->spr[k + 64] != e->last.spr[k + 64] ||
                b->spr[k + 192] != e->last.spr[k + 192]) {
                sprite_mark(e, &e->last, k);
                sprite_mark(e, b, k);
            }
    }
    bitmap_changes(e);
    e->last = *b;
    e->pic_valid = true;
    sprite_rows(b);
    // the console's runs: its rows' own and those of the rows a merge looks at
    uint32_t want[CLCV_H];
    for (int y = 0; y < CLCV_H; y++) {
        int s0 = out_s0[y], s1 = out_s1[y];
        uint32_t m = e->src_dirty[s0] | e->src_dirty[s1];
        if (s1 != s0) {
            if (s0 > 0) m |= e->src_dirty[s0 - 1];
            if (s1 + 1 < CLCV_SRC_H) m |= e->src_dirty[s1 + 1];
        }
        want[y] = m | e->pending[y];
        e->pending[y] = 0;
    }
    memset(e->src_dirty, 0, sizeof e->src_dirty);
    // from row_next on, up to CLCV_CELLS runs; the rest wait
    int done = 0;
    bool full = false;
    uint8_t c[8];
    int start = e->row_next;
    for (int k = 0; k < CLCV_H; k++) {
        int y = start + k;
        if (y >= CLCV_H) y -= CLCV_H;
        uint32_t m = want[y];
        if (!m) continue;
        int n = __builtin_popcount(m);
        if (!full && done && done + n > e->cells_max) {
            full = true;
            e->row_next = (uint8_t)y;
            e->stats.deferred++;
        }
        if (full) {
            e->pending[y] = m;
            continue;
        }
        done += n;
        for (; m; m &= m - 1) {
            int cx = __builtin_ctz(m);
            out_run(e, b, y, cx, c);
            span(e, run_at(y, cx), c);
        }
        e->bands |= 1u << (y >> 3);
    }
    e->stats.cells += (uint32_t)done;
    e->stats.pictures++;
    e->dirty = true;
    return !full;
}

void clcv_canvas(clcv_t *e, const uint8_t *px) {
    if (!colors_ready) colors_init();
    e->pic_valid = false;
    uint8_t c[8];
    for (int y = 0; y < CLCV_H; y++)
        for (int cx = 0; cx < 32; cx++) {
            for (int i = 0; i < 8; i++) {
                uint8_t v = px[y * CLCV_W + cx * 8 + i];
                c[i] = v ? v : BLACK;
            }
            span(e, run_at(y, cx), c);
        }
    e->bands = ~0u;
    e->stats.pictures++;
    e->dirty = true;
}

// ---- tests -------------------------------------------------------------------------
static void screen_of(const uint8_t *pat, const uint8_t *colt, uint8_t *px) {
    for (int y = 0; y < CLCV_H; y++)
        for (int x = 0; x < CLCV_W; x++) {
            unsigned a = run_at(y, x >> 3);
            uint8_t c = colt[a];
            c = (pat[a] & (0x80 >> (x & 7))) ? c >> 4 : c & 15;
            px[y * CLCV_W + x] = c ? c : BLACK;
        }
}

void clcv_screen(const clcv_t *e, uint8_t *px) { screen_of(e->vram_pat, e->vram_col, px); }
void clcv_wanted(const clcv_t *e, uint8_t *px) { screen_of(e->want_pat, e->want_col, px); }

void clcv_ideal(const clcv_t *e, uint8_t *px) {
    if (!rows_ready) rows_init();
    sprite_rows(&e->last);
    for (int y = 0; y < CLCV_H; y++)
        for (int cx = 0; cx < 32; cx++) out_run(e, &e->last, y, cx, px + y * CLCV_W + cx * 8);
}

void clcv_source(const clcv_t *e, uint8_t *pens) {
    sprite_rows(&e->last);
    for (int s = 0; s < CLCV_SRC_H; s++)
        for (int cx = 0; cx < 32; cx++) src_pens(&e->last, s, cx, pens + s * CLCV_W + cx * 8);
}

// ---- what the console needs (pmcv.c's) ------------------------------------------------
static inline CLCV_HOT bool need_pat(const clcv_t *e, unsigned j) { return e->want_pat[j] != e->vram_pat[j]; }
static inline CLCV_HOT bool need_col(const clcv_t *e, unsigned j) { return e->want_col[j] != e->vram_col[j]; }

// The next bytes to send, from the cursor on (qbcv.c's next_run)
CLCV_HOT static bool next_run(clcv_t *e, bool *color, unsigned *s, int *m, unsigned most) {
    if (!e->dirty) return false;
    unsigned i = e->cursor;
    int band = -1;
    for (unsigned k = 0; k < CLCV_BYTES;) {
        if (!(i & 255)) {
            if (band >= 0) e->bands &= ~(1u << band);
            band = -1;
            if (k + 256 <= CLCV_BYTES && !(e->bands >> (i >> 8) & 1)) {
                k += 256;
                i = i + 256 == CLCV_BYTES ? 0 : i + 256;
                continue;
            }
            band = (int)(i >> 8);
        }
        if (!(i & 3) && k + 4 <= CLCV_BYTES) {
            uint32_t w, v, wc, vc;
            memcpy(&w, e->want_pat + i, 4);
            memcpy(&v, e->vram_pat + i, 4);
            memcpy(&wc, e->want_col + i, 4);
            memcpy(&vc, e->vram_col + i, 4);
            if (w == v && wc == vc) {
                k += 4;
                i = i + 4 == CLCV_BYTES ? 0 : i + 4;
                continue;
            }
        }
        bool nc = need_col(e, i);
        if (!nc && !need_pat(e, i)) {
            k++;
            i = i + 1 == CLCV_BYTES ? 0 : i + 1;
            continue;
        }
        e->cursor = (uint16_t)i;
        *color = nc;
        *s = i;
        unsigned end = i + 1, gap = 0;
        for (unsigned j = i + 1; j < CLCV_BYTES && j - i < most; j++) {
            if (nc ? need_col(e, j) : need_pat(e, j)) { end = j + 1; gap = 0; }
            else if (++gap > 3) break;
        }
        *m = (int)(end - i);
        return true;
    }
    e->dirty = false;
    return false;
}

static CLCV_HOT void put_run(clcv_t *e, list_t *l, bool color, unsigned s, int n) {
    l_run_head(l, (uint16_t)((color ? COLORS : PATTERNS) + s), n);
    uint8_t *d = l->b + l->n;
    if (color) {
        memcpy(d, e->want_col + s, n);
        memcpy(e->vram_col + s, d, n);
        e->stats.col_bytes += (unsigned)n;
    } else {
        memcpy(d, e->want_pat + s, n);
        memcpy(e->vram_pat + s, d, n);
        e->stats.pat_bytes += (unsigned)n;
    }
    l->n += n;
    e->stats.runs++;
}

// what's on screen written again as it is (qbcv.c's)
static CLCV_HOT void refresh_run(clcv_t *e, list_t *l, int n) {
    unsigned at = e->refresh_at;
    bool color = at >= CLCV_BYTES;
    unsigned s = color ? at - CLCV_BYTES : at;
    if (s + n > CLCV_BYTES) n = (int)(CLCV_BYTES - s);
    l_run_head(l, (uint16_t)((color ? COLORS : PATTERNS) + s), n);
    memcpy(l->b + l->n, (color ? e->vram_col : e->vram_pat) + s, n);
    l->n += n;
    e->refresh_at = (uint16_t)((at + n) % (2 * CLCV_BYTES));
    e->stats.refreshed += (unsigned)n;
}

uint32_t clcv_backlog(const clcv_t *e) {
    uint32_t n = 0;
    if (e->dirty)
        for (unsigned j = 0; j < CLCV_BYTES; j++) n += need_pat(e, j) + need_col(e, j);
    return n;
}

// the name table: name i at place i in each third (it changes never)
static void names_run(clcv_t *e, list_t *l, int n) {
    int left = 768 - e->names_at;
    if (n > left) n = left;
    l_run_head(l, (uint16_t)(NAMES + e->names_at), n);
    for (int k = 0; k < n; k++) l->b[l->n++] = (uint8_t)(e->names_at + k);
    e->names_at = (uint16_t)(e->names_at + n);
    if (e->names_at == 768) e->names_done = true;
}

// ---- a list, the console waiting for the vertical blank (messages) -------------------
static void plain_list(clcv_t *e, list_t *l) {
    if (e->resync) {
        l_end_vblank(l);
        if (e->tone_n) l_psg(l, e->tone_bytes, e->tone_n);
        if (e->resync-- == 2) return;
    } else {
        // no sprites: the kernel's table has its end mark first; R1 as gbkernel sets it
        l_reg(l, 1, 0xE2);
        e->regs_done = true;
        if (e->tone_n) l_psg(l, e->tone_bytes, e->tone_n);
    }
    while (!e->names_done) {
        int n = room(l);
        if (n < 16 && l->vblank) { l_end_vblank(l); continue; }
        if (n < 1) break;
        names_run(e, l, n);
    }
    bool color;
    unsigned s;
    int m;
    while (e->names_done && next_run(e, &color, &s, &m, 255)) {
        int n = room(l);
        if (n < m && l->vblank) {
            if (n >= 8) put_run(e, l, color, s, n);
            l_end_vblank(l);
            continue;
        }
        if (n < 1) break;
        put_run(e, l, color, s, n < m ? n : m);
    }
    for (int k = 0; k < 2 && e->names_done; k++) {
        int n = room(l);
        if (n < 32) break;
        refresh_run(e, l, n);
    }
    l_end_vblank(l);
}

// ---- a list with samples in it (qbcv.c's pcm_list) -------------------------------------
CLCV_HOT static void fill(clcv_t *e, list_t *l, uint32_t until) {
    int avail = (int)until - (int)l->t;
    if (avail < C_RUN_ACT + C_BYTE_ACT) return;
    int n = (avail - C_RUN_ACT) / C_BYTE_ACT;
    if (n > 255) n = 255;
    if (!e->names_done) { names_run(e, l, n); return; }
    bool color;
    unsigned s;
    int m;
    if (!next_run(e, &color, &s, &m, 1)) {
        if (e->refresh_at % CLCV_BYTES + (unsigned)n > CLCV_BYTES) e->refresh_at = (uint16_t)(e->refresh_at / CLCV_BYTES * CLCV_BYTES);
        refresh_run(e, l, n);
        return;
    }
    if (s + (unsigned)n > CLCV_BYTES) s = CLCV_BYTES - (unsigned)n;
    put_run(e, l, color, s, n);
}

CLCV_HOT static void pcm_list(clcv_t *e, list_t *l) {
    l->t = C_FRAME;
    l_end_vblank(l);
    unsigned tone_at = 0;
    for (int k = 0; k < CLCV_PCM_N; k++) {
        uint8_t b[10];
        int n = 0;
        b[n++] = e->pcm_bytes[k][0];
        b[n++] = e->pcm_bytes[k][1];
        b[n++] = e->pcm_bytes[k][2];
        while (tone_at < e->tone_n && n < 5) {
            unsigned g = 1;
            while (tone_at + g < e->tone_n && !(e->tone_bytes[tone_at + g] & 0x80)) g++;
            if (n + (int)g > 6) break;
            memcpy(b + n, e->tone_bytes + tone_at, g);
            n += (int)g;
            tone_at += g;
        }
        if (k + 1 == CLCV_PCM_N) {
            l_psg(l, b, n);
            break;
        }
        uint32_t until = PCM_FIRST + (((uint32_t)(k + 1) * PCM_STEP) >> 8);
        int avail = (int)until - (int)l->t - (C_PSG + C_PSG_BYTE * n);
        // a short run of the picture's as one of two (qbcv.c's)
        bool color;
        unsigned s;
        int m = 0;
        bool two = e->names_done && next_run(e, &color, &s, &m, 7) && m <= 2 &&
                   avail >= 2 * C_RUN_ACT + C_BYTE_ACT * (m + 2);
        int need = two ? m + 1 : 1;
        int base = avail - (two ? 2 : 1) * C_RUN_ACT, best = 0, best_left = 99;
        for (int r = 0; r < 3 && base - C_PSG_BYTE * r >= C_BYTE_ACT * need; r++) {
            int left = (base - C_PSG_BYTE * r) % C_BYTE_ACT;
            if (left < best_left) { best_left = left; best = r; }
        }
        while (best--) b[n++] = e->pcm_bytes[k][2];     // (the last attenuator again: no change)
        l_psg(l, b, n);
        if (two) put_run(e, l, color, s, m);
        fill(e, l, until);
    }
    e->stats.pcm_frames++;
    e->resync = 2;
}

CLCV_HOT size_t clcv_tv_frame(clcv_t *e, uint8_t *out) {
    list_t l = {out, 0, C_START, true};
    if (e->pcm && e->regs_done) pcm_list(e, &l);
    else plain_list(e, &l);
    l.t += C_END;
    l.b[l.n++] = 0;
    e->tone_n = 0;
    e->stats.tv_frames++;
    if (l.t > e->stats.t_max) e->stats.t_max = l.t;
    if (!e->dirty) e->stats.settled++;
    return l.n;
}

// ---- the sound (pmcv.c's) --------------------------------------------------------------
// The DAC: the three tones at a period of 1, their attenuators as measured
// on the user's console (coleco/ladder.json), summed (Donkey Kong 0.1.2's,
// ../dkong/dkcv.c). cldac3.h's 512 levels go from 0 to 2.40, each a triple
// of attenuators chosen as Movies' table is (tools/mkdac3.py: within 1.5
// levels of its level, neighbouring levels' triples alike, so a small change
// makes small partial sums between a sample's three writes). The sound rests
// at the middle (DAC_MID, 1.20). Up to 0.1.1 two tones made the DAC (128
// levels, 0 to 1.40): the third gives 1.7 times the room either side of
// the rest. The steps stay about as fine: neighbouring levels often share a
// triple (214 different ones in the 512), which keeps the partial sums small.
#define DAC_LEVELS CLDAC3_LEVELS
#define DAC_MID    255
#define DAC_HALF   255.0f           // the levels either side of DAC_MID
// SCALE_LEVELS levels for SCALE_FULL of the mix (MAME's +-1.0 as +-32767);
// past LOUD_KNEE levels a soft knee that rounds off toward 255, never
// clipping (Dragon's Lair 0.1.3's). The DAC can't carry the POKEYs' DC (their
// output is all above 0, and moves with each channel's volume), so a
// high-pass takes it off: at 1.9 Hz, where Pac-Man's is at 15, because
// the POKEYs' DC moves a lot and a 15 Hz pole's slow swing after each move
// is a difference from MAME's sound (test/soundcmp.sh: 15.5 dB at 15 Hz,
// 20.9 here). Both can be set for the tests. 0.1.2: 380 instead of 42 of
// the two-tone DAC's 128 levels (99 of these), 3.8 times as loud (+11.7
// dB): the game played some 13 dB under an ordinary ColecoVision cartridge
// (a game's square tones swing twice as far as a parked tone's level can).
// 5.3% of the scripted game's samples go into the knee (0.2% before):
// sound to difference against MAME's 16.9 dB overall (20.9), the median
// second 23.7 (20.6), log-spectral distance 3.2 dB (4.5).
#ifndef SCALE_LEVELS
#define SCALE_LEVELS 380
#endif
#define SCALE_FULL   8192
#define LOUD_KNEE    191.0f         // the soft knee: 3/4 of the 255 levels either side
#ifndef HP_SHIFT
#define HP_SHIFT     9              // a pole at 1.9 Hz
#endif
static const float ladder[3][16] = {
    {1.0f, 0.7914f, 0.6320f, 0.5084f, 0.4019f, 0.3161f, 0.2459f, 0.1912f, 0.1396f, 0.1080f, 0.0844f, 0.0672f, 0.0524f, 0.0421f, 0.0310f, 0.0f},
    {1.0f, 0.7816f, 0.6125f, 0.4850f, 0.3925f, 0.3109f, 0.2416f, 0.1872f, 0.1373f, 0.1058f, 0.0828f, 0.0653f, 0.0533f, 0.0412f, 0.0306f, 0.0f},
    {1.0f, 0.7979f, 0.6229f, 0.4921f, 0.3942f, 0.3004f, 0.2348f, 0.1874f, 0.1419f, 0.1106f, 0.0846f, 0.0677f, 0.0534f, 0.0420f, 0.0328f, 0.0f}};
static uint8_t limit[512];                  // |level| (0-511) to 0-255: straight to LOUD_KNEE, then soft
static uint16_t dac3[CLDAC3_LEVELS];        // cl_dac3 copied to SRAM (the plugin runs from flash)
static int16_t lq[3][16];                   // ladder in 1/16384ths, for put_sample's integer sums
static bool dac_ready;

float clcv_dac_level(int a0, int a1, int a2) { return ladder[0][a0 & 15] + ladder[1][a1 & 15] + ladder[2][a2 & 15]; }

static void dac_init(void) {
    for (int i = 0; i < 512; i++) {
        float v = (float)i;
        if (v > LOUD_KNEE) v = LOUD_KNEE + (DAC_HALF - LOUD_KNEE) * (1.0f - expf(-(v - LOUD_KNEE) / (DAC_HALF - LOUD_KNEE)));
        limit[i] = (uint8_t)(v + 0.5f);
    }
    memcpy(dac3, cl_dac3, sizeof dac3);
    for (int c = 0; c < 3; c++)
        for (int a = 0; a < 16; a++) lq[c][a] = (int16_t)lrintf(ladder[c][a] * 16384.0f);
    dac_ready = true;
}

// MAME's mix (cloak.cpp): each POKEY's output through its OPAMP_LOW_PASS
// (1k with 0.047 uF and 0.022 uF), half each, here at the bins' rate, in
// 16.16 (the M33 has no double)
static int32_t mix_k1, mix_k2;          // each filter's step, << 16

CLCV_HOT int16_t clcv_mix(clcv_mix_t *m, int32_t sum1, int32_t sum2, uint16_t count) {
    if (!mix_k1) {
        float rate = (float)CLCV_RATE_Q8 / 256.0f;
        mix_k1 = (int32_t)(65536.0f * (1.0f - expf(-1.0f / (0.047e-6f * 1000.0f * rate))));
        mix_k2 = (int32_t)(65536.0f * (1.0f - expf(-1.0f / (0.022e-6f * 1000.0f * rate))));
    }
    if (!count) return 0;
    int32_t v1 = sum1 / count, v2 = sum2 / count;          // CL_SND_ONE (65536) is 1.0
    m->f1 += (int32_t)(((int64_t)(v1 - m->f1) * mix_k1) >> 16);
    m->f2 += (int32_t)(((int64_t)(v2 - m->f2) * mix_k2) >> 16);
    int32_t v = (int32_t)(((int64_t)(m->f1 + m->f2) * 32767) >> 17);
    return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

// a sample's three writes, in the order whose two partial sums (the chip
// between the writes, 52 T each) are nearest the new level. Written a,
// then b, then c: off the new level by |d_b + d_c|, then by |d_c|. In
// integers, the orders tried in turn and the first of equals kept: the
// noise scene has no room for the 400 cycles a sample the floats took.
static CLCV_DATA const uint8_t order[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
static inline CLCV_HOT int32_t iabs(int32_t x) { return x < 0 ? -x : x; }
static inline CLCV_HOT void put_sample(uint8_t *out, int *a0, int *a1, int *a2, uint16_t t) {
    const int w0 = t >> 8, w1 = t >> 4 & 15, w2 = t & 15;
    const int32_t d0 = lq[0][w0] - lq[0][*a0], d1 = lq[1][w1] - lq[1][*a1], d2 = lq[2][w2] - lq[2][*a2];
    const int32_t sum = d0 + d1 + d2;
    const int32_t f0 = iabs(sum - d0), f1 = iabs(sum - d1), f2 = iabs(sum - d2);
    const int32_t l0 = iabs(d0), l1 = iabs(d1), l2 = iabs(d2);
    int32_t best = f0 + l2, v;
    int pick = 0;
    if ((v = f0 + l1) < best) { best = v; pick = 1; }
    if ((v = f1 + l2) < best) { best = v; pick = 2; }
    if ((v = f1 + l0) < best) { best = v; pick = 3; }
    if ((v = f2 + l1) < best) { best = v; pick = 4; }
    if ((v = f2 + l0) < best) pick = 5;
    const uint32_t b = (uint32_t)(0x90 | w0) | (uint32_t)(0xB0 | w1) << 8 | (uint32_t)(0xD0 | w2) << 16;
    out[0] = (uint8_t)(b >> 8 * order[pick][0]);
    out[1] = (uint8_t)(b >> 8 * order[pick][1]);
    out[2] = (uint8_t)(b >> 8 * order[pick][2]);
    *a0 = w0; *a1 = w1; *a2 = w2;
}

CLCV_HOT void clcv_sound(clcv_t *e, const int16_t *s, int n) {
    if (!dac_ready) dac_init();
    e->tone_n = 0;
    if (n <= 0) {
        if (e->pcm) {
            static const uint8_t off[] = {0x9F, 0xBF, 0xDF};
            memcpy(e->tone_bytes, off, sizeof off);
            e->tone_n = sizeof off;
            e->pcm = false;
        }
        return;
    }
    if (!e->pcm) {
        e->pcm = true;
        e->pcm_gain = 0;
        e->hp_x = s[0];
        e->hp_y = 0;
        e->dac_att[0] = e->dac_att[1] = e->dac_att[2] = 15;     // (as the lists before left them)
    }
    // every list: tones 1 and 2 parked at a period of 1, tone 0 and the
    // noise silent, and (0.1.2) tone 0's period and the noise's control, so
    // every register is written (pmcv.c: one misread byte lasts a list at most)
    static const uint8_t park[] = {0x81, 0x00, 0xA1, 0x00, 0xC1, 0x00, 0xE4, 0xFF};   // (the attenuators: every sample's)
    memcpy(e->tone_bytes, park, sizeof park);
    e->tone_n = sizeof park;
    // (the state in locals: the byte stores below could alias e's fields)
    int32_t hx = e->hp_x, hy = e->hp_y, gain = e->pcm_gain;
    int a0 = e->dac_att[0], a1 = e->dac_att[1], a2 = e->dac_att[2];
    uint32_t clipped = 0;
    for (int k = 0; k < CLCV_PCM_N; k++) {
        int32_t x = s[k < n ? k : n - 1];
        hy += ((x - hx) << 8) - (hy >> HP_SHIFT);
        hx = x;
        int32_t v = (hy >> 8) * SCALE_LEVELS / SCALE_FULL;
        int32_t a = v < 0 ? -v : v;
        if (a > LOUD_KNEE) clipped++;
        a = limit[a > 511 ? 511 : a];
        int32_t lvl = DAC_MID + (v < 0 ? -a : a);
        // (the DAC's rest is 1.7 times the two tones': up to it over 43 ms, as steep as before)
        if (gain < 32 && !(k & 7)) gain++;
        lvl = lvl * gain / 32;
        put_sample(e->pcm_bytes[k], &a0, &a1, &a2, dac3[lvl < 0 ? 0 : lvl > DAC_LEVELS - 1 ? DAC_LEVELS - 1 : lvl]);
    }
    e->hp_x = hx;
    e->hp_y = hy;
    e->pcm_gain = (uint8_t)gain;
    e->dac_att[0] = (uint8_t)a0;
    e->dac_att[1] = (uint8_t)a1;
    e->dac_att[2] = (uint8_t)a2;
    e->stats.clipped += clipped;
}

void clcv_init(clcv_t *e) {
    memset(e, 0, sizeof *e);
    e->dirty = true;
    e->bands = (1u << (CLCV_BYTES / 256)) - 1;
    e->cells_max = CLCV_CELLS;
    if (!colors_ready) colors_init();
    if (!dac_ready) dac_init();
    if (!rows_ready) rows_init();
    memset(run_hash, 0, sizeof run_hash);
    memset(row_hash, 0, sizeof row_hash);
    shows_seen = cl_bm_shows - 1;
}

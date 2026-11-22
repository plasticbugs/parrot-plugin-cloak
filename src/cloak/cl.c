// license:BSD-3-Clause
// copyright-holders:Dan Boris, Mirko Buffoni, Brad Oliver, Eric Smith, Juergen Buchmueller, Aaron Giles, AJR, Couriersud, Nicola Salmoria
//
// cl.c -- Atari's Cloak & Dagger board (1983) as MAME 0.288 emulates it, in
// C for the cart:
//
//   cloak.cpp (Dan Boris, Mirko Buffoni): the two 6502s' memory maps, the
//     graph processor and its two bitmaps, the playfield, the sprites, the
//     palette and the inputs
//   pokey.cpp (Brad Oliver, Eric Smith, Juergen Buchmueller): the POKEYs,
//     clock by clock, and their OPAMP_LOW_PASS output
//   74259.cpp (AJR), watchdog.cpp (Aaron Giles), resnet.cpp (Couriersud):
//     the output latch, the watchdog and the palette's resistor weights
//   drawgfx.cpp, tilemap.cpp (Nicola Salmoria, Aaron Giles): the chars'
//     and sprites' decoding and drawing
//   schedule.cpp, diexec.cpp, screen.cpp, sound.cpp (Aaron Giles and
//     others): MAME's timeslices, timers, input line events, VBLANK and
//     the screen's partial updates
//
// MAME runs four devices in turn each timeslice, in its config's order: the
// master 6502, the slave 6502, POKEY 1 and POKEY 2, up to the next timer or
// the 1 ms quantum. Each POKEY read or write is synchronize()d, which ends
// the master's timeslice, and a POKEY reads as far as its own clock has
// got, so what the master reads (RANDOM above all) comes out the same only
// if every timeslice ends where MAME's does: this runs MAME's scheduler as
// it is. The master's main loop reads RANDOM from both POKEYs over and
// over, so there are tens of thousands of timeslices a second, and three
// things keep them cheap:
//
//  - The POKEYs' clocks are counted as MAME runs them, but their chips
//    step only when something looks: a register read or write, or the
//    sound taken at the end of a list (RANDOM needs only the polynomials'
//    places, which follow from the clock). In this game the only thing a
//    POKEY does to the scheduler is the sync when its pots' scan ends, and
//    its clock is known ahead (pot_due), so that timeslice ends there as
//    MAME's does. A chip steps clock by clock (cl_pokey_exact) or from one
//    event to the next with the same result (a counter wrapping, a borrow
//    ending, a new output level).
//  - The synchronize()s wait in a short list of their own, apart from the
//    regular timers.
//  - Times are 64-bit attoseconds from an epoch moved on a second at a
//    time, and every device's cycle is a whole number of 2e11 units, so a
//    timeslice's cycles come from one division by a constant.
//  - A POKEY read that isn't of the pots, and a write of a register's
//    own value, don't step the chip at all.
//
// MAME's license (BSD-3-Clause) is in README.md.
#include "cl.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "m6502.h"

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define CL_HOT __attribute__((section(".time_critical.cl")))
#define CL_DATA __attribute__((section(".time_critical.cl_data")))    // a hot table (m6502.c's M65_DATA)
#else
#define CL_HOT
#define CL_DATA
#endif

// ---------------------------------------------------------------------------
// the machine's constants: times in attoseconds

typedef int64_t at_t;
#define AS_PER_SEC 1000000000000000000LL
#define AT_NEVER INT64_MAX
#define FRAME_AS (AS_PER_SEC / 60)      // set_refresh_hz(60)
#define SCAN_AS (FRAME_AS / 256)        // 256 x 256
#define PIX_AS (FRAME_AS / (256 * 256))
#define VBLANK_AS 2500000000000000LL    // set_vblank_time(ATTOSECONDS_IN_USEC(2500))
#define MAIN_IRQ_AS (AS_PER_SEC / 240)  // irq0_line_hold at 4*60 Hz
#define SLAVE_IRQ_AS (AS_PER_SEC / 120) // at 2*60 Hz
#define QUANTUM_AS (AS_PER_SEC / 1000)  // set_maximum_quantum(attotime::from_hz(1000))
#define SND50_AS (AS_PER_SEC / 50)      // the sound manager's streams update

static const cl_roms_t *roms;
cl_inputs_t cl_in = { CL_P1, CL_SYSTEM, CL_START, CL_DSW };
uint8_t cl_ram[0x400], cl_vram[0x400], cl_shared[0x800], cl_nvram[0x200], cl_spr[0x100];
uint8_t cl_sram[0x800];
uint16_t cl_pal[64];
uint32_t cl_rgb[64];
uint8_t cl_bm[2][0x8000];
uint32_t cl_bm_shows;
uint8_t cl_bm_sel, cl_bm_x, cl_bm_y;
uint8_t cl_latch;
uint32_t cl_frames, cl_resets, cl_unsupported;
cl_fb_t *cl_frame_fb;
void (*cl_on_vblank)(void);
void (*cl_trace)(int cpu, const cl_regs_t *r, uint64_t cycles);
void (*cl_pokey_hook)(int n, uint64_t clock, uint32_t out_raw);
bool cl_pokey_exact;
cl_stats_t cl_stats;

static uint8_t zeros[0x100];            // unmapped reads: 0 (MAME's unmap value)
static uint8_t sink[0x100];             // writes that go nowhere
static const uint8_t *page_rd[256];      // a CPU's pages as cpu_setup maps them (map_pages)
static uint8_t *page_wr[256];

// ---------------------------------------------------------------------------
// the scheduler (src/emu/schedule.cpp, src/emu/diexec.cpp)

typedef struct {
	int64_t lc;                         // m_localtime, in cycles from the epoch (it is always whole cycles)
	int64_t aspc;                       // m_attoseconds_per_cycle
	int64_t cps;                        // m_cycles_per_second
	uint32_t k;                         // aspc in units (UNIT_AS)
	uint64_t total;                     // m_totalcycles (the POKEYs': never reset, their chips' clock)
} exec_t;

enum { EX_MAIN, EX_SLAVE, EX_POKEY1, EX_POKEY2, EX_COUNT };    // MAME's execute list, in config order
static exec_t ex[EX_COUNT];
static int executing = -1;              // only the CPUs execute: the POKEYs are counted
static int32_t epoch_s;                 // the seconds before the epoch
static at_t basetime;

static m65_t cpus[2];

static inline CL_HOT void cpu_select(int n) { m65p = &cpus[n]; }

// Each device's cycle is a whole number of units of 2e11 attoseconds (the
// master's 5, the others' 4), so floor(t / aspc) is floor(t / UNIT_AS) / k,
// and a device's time is a whole number of units.
#define UNIT_AS 200000000000LL
_Static_assert(AS_PER_SEC / CL_MAIN_CLOCK == 5 * UNIT_AS && AS_PER_SEC / CL_SLAVE_CLOCK == 4 * UNIT_AS &&
               AS_PER_SEC / CL_POKEY_CLOCK == 4 * UNIT_AS, "timeslice() takes these");
// floor(t / UNIT_AS), 0 <= t < 2^60: an estimate from t's top 32 bits (it
// comes out low by at most 3), put right; *part: t isn't a whole number of units
static inline CL_HOT uint32_t at_units(at_t t, bool *part) {
	uint32_t q = (uint32_t)(((uint64_t)(uint32_t)((uint64_t)t >> 28) * 5764607u) >> 32);    // 2^60 / UNIT_AS
	int64_t r = t - (int64_t)q * UNIT_AS;
	while (r >= UNIT_AS) { q++; r -= UNIT_AS; }
	*part = r != 0;
	return q;
}

static void exec_init(exec_t *e, uint32_t clock) {
	e->lc = 0;
	e->cps = clock;
	e->aspc = AS_PER_SEC / clock;
	e->k = (uint32_t)(e->aspc / UNIT_AS);
	e->total = 0;
}

// the regular timers (the pending synchronize()s are below)
enum { TM_VBSTART, TM_VBEND, TM_SCAN0, TM_SND50, TM_MAINIRQ, TM_SLAVEIRQ, TM_WATCHDOG, TM_SEROC1, TM_SEROC2, TM_COUNT };
typedef struct { at_t expire, period; uint32_t seq; } tmr_t;   // expire AT_NEVER: off; period 0: one shot
static tmr_t tm[TM_COUNT];
static int tm_head = -2;                // the regular timers' head, or -2: not known
static uint32_t tm_seq;

enum { SY_NONE, SY_IRQ, SY_PWRITE, SY_POT };
typedef struct { at_t t; uint32_t seq; uint8_t type; int param; } pend_t;
#define NPEND 24
static pend_t pend[NPEND];              // the first to go off last (PEND0)
static int npend;
#define PEND0 pend[npend - 1]

static bool in_callback;
static int cb_timer;                    // the regular timer whose callback runs, or -1
static at_t cb_expire;
static bool cb_modified;

static inline CL_HOT int reg_head(void) {
	if (tm_head != -2) return tm_head;
	int h = -1;
	for (int i = 0; i < TM_COUNT; i++) {
		if (tm[i].expire == AT_NEVER) continue;
		if (h < 0 || tm[i].expire < tm[h].expire || (tm[i].expire == tm[h].expire && tm[i].seq < tm[h].seq)) h = i;
	}
	return tm_head = h;
}

static inline CL_HOT at_t head_expire(void) {
	int h = reg_head();
	at_t e = h < 0 ? AT_NEVER : tm[h].expire;
	if (npend && PEND0.t < e) e = PEND0.t;
	return e;
}

static inline CL_HOT void exec_abort(void) { if (executing >= 0) m65_abort(); }

// machine().time(): the timer's time in a callback, the CPU's while it runs, else the base
static inline CL_HOT at_t now(void) {
	if (in_callback) return cb_expire;
	if (executing >= 0) return (ex[executing].lc + m65_done()) * ex[executing].aspc;
	return basetime;
}

static CL_HOT void tm_adjust(int i, at_t delay, at_t period) {
	tm[i].expire = delay == AT_NEVER ? AT_NEVER : now() + delay;
	tm[i].period = period;
	tm[i].seq = ++tm_seq;
	tm_head = -2;
	if (in_callback && i == cb_timer) cb_modified = true;
	if (executing >= 0 && reg_head() == i && !(npend && PEND0.t <= tm[i].expire)) exec_abort();
}

static CL_HOT void pend_add(at_t t, int type, int param) {
	if (npend == NPEND) {
		cl_unsupported |= 1;            // (more than the list: never seen)
		return;
	}
	uint32_t seq = ++tm_seq;
	int i = npend++;                    // after those due before it or with it (a new one goes after)
	while (i > 0 && pend[i - 1].t <= t) { pend[i] = pend[i - 1]; i--; }
	pend[i] = (pend_t){ t, seq, (uint8_t)type, param };
}

// MAME's synchronize(): a timer for now; if it's the first to go off, the
// running CPU's timeslice ends there. One with nothing to call (SY_NONE, a
// POKEY read's) still holds the timeslices that follow to its time.
static inline CL_HOT void synchronize(int type, int param) {
	at_t t = now();
	bool first = t < head_expire();
	pend_add(t, type, param);
	if (first) exec_abort();
}

// the CPUs' IRQ lines (device_input): set_state_synced's event queue, applied by a timer for now
enum { CLEAR_LINE, ASSERT_LINE, HOLD_LINE };
static struct { uint8_t queue[32]; int qn; int curstate; } line[2];

static CL_HOT void irq_events(int n) {                 // empty_event_queue
	for (int i = 0; i < line[n].qn; i++) {
		line[n].curstate = line[n].queue[i];
		cpus[n].irq_state = line[n].curstate != CLEAR_LINE;
	}
	line[n].qn = 0;
}

static CL_HOT void set_input_line(int n, int state) {
	if (line[n].qn == (int)sizeof line[n].queue) irq_events(n);     // full: applied now (MAME logs it)
	line[n].queue[line[n].qn++] = (uint8_t)state;
	if (line[n].qn == 1) synchronize(SY_IRQ, n);
}

// standard_irq_callback(IRQ_LINE): a HOLD_LINE interrupt is dropped as it's taken
static CL_HOT void irq_ack_main(void) {
	if (line[0].curstate == HOLD_LINE) { cpus[0].irq_state = false; line[0].curstate = CLEAR_LINE; }
}
static CL_HOT void irq_ack_slave(void) {
	if (line[1].curstate == HOLD_LINE) { cpus[1].irq_state = false; line[1].curstate = CLEAR_LINE; }
}

// ---------------------------------------------------------------------------
// the screen

static at_t vblank_start_time, vblank_end_time, last_partial_reset;
static int last_partial_scan;
static bool frame_done, watchdog_fired;

static inline CL_HOT bool in_vblank(void) { return now() < vblank_end_time; }

static CL_HOT int vpos(void) {
	int64_t delta = now() - vblank_start_time;
	delta += PIX_AS / 2;
	return (int)((256 + delta / SCAN_AS) % 256);
}

static CL_HOT at_t time_until_pos(int v) {
	v %= 256;                           // (VBLANK starts at the visible area's bottom + 1 = 256)
	int64_t target = (int64_t)v * SCAN_AS;
	int64_t cur = now() - vblank_start_time;
	if (target <= cur + PIX_AS / 2) target += FRAME_AS;
	while (target <= cur) target += FRAME_AS;
	return target - cur;
}

static CL_HOT void update_partial(int scanline) {      // screen_device::update_partial
	if (!cl_frame_fb) return;
	if (scanline < last_partial_scan) return;
	if (last_partial_scan == 0 && last_partial_reset > now()) return;
	int top = last_partial_scan > CL_Y0 ? last_partial_scan : CL_Y0;
	int bot = scanline < 255 ? scanline : 255;
	if (top > bot) return;
	cl_render(*cl_frame_fb, top, bot);
	last_partial_scan = scanline + 1;
}

static CL_HOT void reset_partial_updates(void) {
	last_partial_reset = now();
	last_partial_scan = 0;
	tm_adjust(TM_SCAN0, time_until_pos(0), 0);
}

static CL_HOT void cb_vbstart(void) {
	vblank_start_time = now();
	vblank_end_time = vblank_start_time + VBLANK_AS;
	update_partial(255);                // video().frame_update(): finish_screen_updates
	cl_frames++;
	if (cl_on_vblank) cl_on_vblank();
	frame_done = true;
	tm_adjust(TM_VBSTART, time_until_pos(0), 0);
	tm_adjust(TM_VBEND, vblank_end_time - now(), 0);
}

// ---------------------------------------------------------------------------
// the POKEYs (pokey.cpp)

enum { CHAN1, CHAN2, CHAN3, CHAN4 };
#define NOTPOLY5 0x80
#define POLY4 0x40
#define PURE 0x20
#define VOLUME_ONLY 0x10
#define VOLUME_MASK 0x0f
#define POLY9 0x80
#define CH1_HICLK 0x40
#define CH3_HICLK 0x20
#define CH12_JOINED 0x10
#define CH34_JOINED 0x08
#define CH1_FILTER 0x04
#define CH2_FILTER 0x02
#define CLK_15KHZ 0x01
#define IRQ_SEROC 0x08
#define SK_FRAME 0x80
#define SK_KBERR 0x40
#define SK_OVERRUN 0x20
#define SK_SERIN 0x10
#define SK_KEYBD 0x04
#define SK_BUSY 0x02
#define SK_BREAK 0x80
#define SK_OCLK 0x60
#define SK_ICLK 0x30
#define SK_ASYNC 0x10
#define SK_TWOTONE 0x08
#define SK_PADDLE 0x04
#define SK_RESET 0x03
#define SK_KEYSCAN 0x02
#define DIV_64 28
#define DIV_15 114
enum { CLK_1, CLK_28, CLK_114 };
#define NEVER_CLK UINT64_MAX

typedef struct {
	uint8_t AUDF[4], AUDC[4];
	int32_t borrow[4], counter[4];
	uint8_t output[4], filter[4];
	uint32_t out_raw;
	bool inval;                         // m_old_raw_inval
	int32_t clk[3];
	uint32_t p4, p5, p9, p17;
	uint8_t POTx[8], AUDCTL, ALLPOT, KBCODE, SERIN, SEROUT, IRQST, IRQEN, SKSTAT, SKCTL;
	uint8_t pot_counter;
	int id;
	uint64_t sc;                        // the chip's clock: the steps done (ex[].total is MAME's)
	// the closed form (ff, pk_sync): from clock sc0, where the polynomials,
	// clk[] and pot_counter are as kept, each channel's counter cv as of
	// clock rc and its borrow's next end C; quiet: the channels put off
	bool ff;
	uint8_t quiet;
	uint64_t sc0, rc[4], C[4], W[4];   // W: the wrap that starts C's borrow (NEVER_C: before rc)
	int32_t cv[4];
	uint64_t pot_due;                   // the step at which the pots' scan ends (its sync), or NEVER_CLK
	int32_t level;                      // the output's level now (sound)
	uint64_t snd_pos;                   // the next sample not yet in a bin
} pokey_t;

static pokey_t pk[2];

// the polynomials: poly4 and poly5 and poly9 as MAME's tables, poly17 as
// its bit 0 in a bit table (16K, with its first 32 bits again at the end:
// cl_poly17.h, made by tools/poly17gen.py, a constant so that the cart
// keeps it in flash): its state shifts right a bit a clock, so entry k's
// bits 8-15 are bit 0 of entries k-9 .. k-2
static uint8_t poly4[15], poly5[31];
static uint16_t poly9[511];
#define P17N 131071
#include "cl_poly17.h"
_Static_assert(sizeof poly17_bit == (P17N + 32) / 8 + 2, "cl_poly17.h is the table");

static inline CL_HOT int p17_bit(int32_t k) {
	if (k < 0) k += P17N;
	return (poly17_bit[k >> 3] >> (k & 7)) & 1;
}
static inline CL_HOT uint8_t p17_random(uint32_t k) {  // (poly17[k] >> 8) & 0xff, k < P17N
	uint32_t j = k >= 9 ? k - 9 : k + P17N - 9;
	return (uint8_t)((poly17_bit[j >> 3] | (uint32_t)poly17_bit[(j >> 3) + 1] << 8) >> (j & 7));
}

static void poly_init(void) {
	uint32_t lfsr = 0;
	for (int i = 0; i < 15; i++) {      // poly_init_4_5
		lfsr = (lfsr << 1) | (~((lfsr >> 2) ^ (lfsr >> 3)) & 1);
		poly4[i] = lfsr & 15;
	}
	lfsr = 0;
	for (int i = 0; i < 31; i++) {
		lfsr = (lfsr << 1) | (~((lfsr >> 2) ^ (lfsr >> 4)) & 1);
		poly5[i] = lfsr & 31;
	}
	lfsr = 0x1ff;                       // poly_init_9_17
	for (int i = 0; i < 511; i++) {
		uint32_t in = (lfsr & 1) ^ ((lfsr >> 5) & 1);
		lfsr = (lfsr >> 1) | (in << 8);
		poly9[i] = (uint16_t)lfsr;
	}
#ifndef NDEBUG
	lfsr = 0x1ffff;                     // the table, against the polynomial (the tests' builds)
	for (int i = 0; i < P17N + 32; i++) {
		if (i < P17N) {
			uint32_t in8 = ((lfsr >> 8) & 1) ^ ((lfsr >> 13) & 1);
			uint32_t in = lfsr & 1;
			lfsr = lfsr >> 1;
			lfsr = (lfsr & 0xff7f) | (in8 << 7);
			lfsr = (in << 16) | lfsr;
		}
		if ((uint32_t)((poly17_bit[i >> 3] >> (i & 7)) & 1) != (i < P17N ? (lfsr & 1) : (uint32_t)p17_bit(i - P17N))) abort();
	}
#endif
}

// the output's level: V0 = r_pullup / rTot = 1k times the channels'
// conductances (vol_init), each channel's from its volume
static int32_t chan_g[16];

static void vol_init(void) {
	static const double res[4] = { 90000, 26500, 8050, 3400 };
	for (int j = 0; j < 16; j++) {
		double g = 1.0 / 1e12;
		for (int i = 0; i < 4; i++) g += (j & (1 << i)) ? 1.0 / res[i] : 1.0 / 8e6;
		chan_g[j] = (int32_t)(1000.0 * g * CL_SND_ONE + 0.5);
	}
}

static inline CL_HOT int32_t level_of(uint32_t out_raw) {
	return chan_g[out_raw & 15] + chan_g[(out_raw >> 4) & 15] + chan_g[(out_raw >> 8) & 15] + chan_g[(out_raw >> 12) & 15];
}

static void snd_advance(pokey_t *p, uint64_t upto);

static inline CL_HOT void chan_reset(pokey_t *p, int c) { p->counter[c] = p->AUDF[c] ^ 0xff; p->borrow[c] = 0; }

static inline CL_HOT void inc_chan(pokey_t *p, int c, int cycles) {
	p->counter[c] = (p->counter[c] + 1) & 0xff;
	if (p->counter[c] == 0 && p->borrow[c] == 0) p->borrow[c] = cycles;
	// (the timers' IRQs: IRQEN stays 0 in this game, cl_unsupported says if not)
}

static inline CL_HOT int check_borrow(pokey_t *p, int c) {
	if (p->borrow[c] > 0) {
		p->borrow[c]--;
		return p->borrow[c] == 0;
	}
	return 0;
}

static inline CL_HOT void process_channel(pokey_t *p, int c) {
	if ((p->AUDC[c] & NOTPOLY5) || (poly5[p->p5] & 1)) {
		if (p->AUDC[c] & PURE) p->output[c] ^= 1;
		else if (p->AUDC[c] & POLY4) p->output[c] = poly4[p->p4] & 1;
		else if (p->AUDCTL & POLY9) p->output[c] = poly9[p->p9] & 1;
		else p->output[c] = (uint8_t)p17_bit((int32_t)p->p17);
		p->inval = true;
	}
}

// step_one_clock, for the game's settings: no serial clocks and no two-tone
// (cl_unsupported says if the game sets them); the pots' sync at 228 is the
// scheduler's (pot_due)
static CL_HOT void step_one_clock(pokey_t *p) {
	if (p->SKCTL & SK_RESET) {
		if (++p->p4 == 0x0f) p->p4 = 0;
		if (++p->p5 == 0x1f) p->p5 = 0;
		if (++p->p9 == 0x1ff) p->p9 = 0;
		if (++p->p17 == 0x1ffff) p->p17 = 0;
		int trig[3] = { 1, 0, 0 };
		if (++p->clk[CLK_28] >= DIV_64) { p->clk[CLK_28] = 0; trig[CLK_28] = 1; }
		if (++p->clk[CLK_114] >= DIV_15) { p->clk[CLK_114] = 0; trig[CLK_114] = 1; }
		if (p->AUDCTL & CH1_HICLK) inc_chan(p, CHAN1, (p->AUDCTL & CH12_JOINED) ? 7 : 4);
		int base = (p->AUDCTL & CLK_15KHZ) ? CLK_114 : CLK_28;
		if (!(p->AUDCTL & CH1_HICLK) && trig[base]) inc_chan(p, CHAN1, 1);
		if (p->AUDCTL & CH3_HICLK) inc_chan(p, CHAN3, (p->AUDCTL & CH34_JOINED) ? 7 : 4);
		if (!(p->AUDCTL & CH3_HICLK) && trig[base]) inc_chan(p, CHAN3, 1);
		if (trig[base]) {
			if (!(p->AUDCTL & CH12_JOINED)) inc_chan(p, CHAN2, 1);
			if (!(p->AUDCTL & CH34_JOINED)) inc_chan(p, CHAN4, 1);
		}
		if ((trig[CLK_114] || (p->SKCTL & SK_PADDLE)) && p->pot_counter < 228) p->pot_counter++;
		// (the keyboard scan: no keyboard, so nothing to see)
	}

	if (check_borrow(p, CHAN3)) {
		if (p->AUDCTL & CH34_JOINED) inc_chan(p, CHAN4, 1);
		else chan_reset(p, CHAN3);
		process_channel(p, CHAN3);
		if (p->AUDCTL & CH1_FILTER) p->filter[CHAN1] = p->output[CHAN1];
		else p->filter[CHAN1] = 1;
		p->inval = true;
	}
	if (check_borrow(p, CHAN4)) {
		if (p->AUDCTL & CH34_JOINED) chan_reset(p, CHAN3);
		chan_reset(p, CHAN4);
		process_channel(p, CHAN4);
		if (p->AUDCTL & CH2_FILTER) p->filter[CHAN2] = p->output[CHAN2];
		else p->filter[CHAN2] = 1;
		p->inval = true;
	}
	if (check_borrow(p, CHAN1)) {
		if (p->AUDCTL & CH12_JOINED) inc_chan(p, CHAN2, 1);
		else chan_reset(p, CHAN1);
		process_channel(p, CHAN1);
	}
	if (check_borrow(p, CHAN2)) {
		if (p->AUDCTL & CH12_JOINED) chan_reset(p, CHAN1);
		chan_reset(p, CHAN2);
		process_channel(p, CHAN2);
	}
	if (p->inval) {
		uint32_t sum = 0;
		for (int c = 0; c < 4; c++)
			if ((p->output[c] ^ p->filter[c]) || (p->AUDC[c] & VOLUME_ONLY))
				sum |= (uint32_t)(p->AUDC[c] & VOLUME_MASK) << (c * 4);
		if (p->out_raw != sum) {        // m_stream->update(): the samples to now are the old level
			snd_advance(p, p->sc + 1);
			p->level = level_of(sum);
			if (cl_pokey_hook) cl_pokey_hook(p->id, p->sc, sum);
		}
		p->inval = false;
		p->out_raw = sum;
	}
}

// The chip from one clock to a later one, as MAME's step_one_clock() takes
// it clock by clock (cl_pokey_exact does that), in closed form: a channel's
// counter counts every clock or every base tick (64 or 15 kHz), and the
// only clocks that do more are those where a channel's borrow ends (its
// counter wrapped one or a few clocks before): the channel's output moves
// and its counter reloads, or for a joined pair's low channel the high one
// counts. Those are each channel's next completion (C[]), worked out from
// its counter (cv as of clock rc) and taken in time order, in the order a
// clock takes the channels (3, 4, 1, 2); the counters, the polynomials'
// places, the dividers and the pots' counter follow from the clock when
// they're wanted (to_generic, before a register write or a read that needs
// them). A channel no one can hear (no volume, or volume only, and no
// filter or joined channel looks at it) is put off altogether: after the
// first, its completions come every so many clocks, so where its output
// stands when it's wanted is a sum over at most 31 of them (poly5's
// length). The
// clocks stay those of the clock-by-clock way, output change for output
// change (test/compare.sh checks).
#define NEVER_C UINT64_MAX
#ifdef CL_COUNT_EVENTS
uint32_t cl_ev_count[2][4], cl_rd_count[2][16], cl_wr_count[2][16][2], cl_pair_runs, cl_pair_steps;
#endif

static inline CL_HOT int ff_div(const pokey_t *p) { return (p->AUDCTL & CLK_15KHZ) ? DIV_15 : DIV_64; }
static inline CL_HOT uint32_t ff_clk0(const pokey_t *p) { return (uint32_t)p->clk[(p->AUDCTL & CLK_15KHZ) ? CLK_114 : CLK_28]; }
static inline CL_HOT bool ff_fast(const pokey_t *p, int c) {
	return (c == CHAN1 && (p->AUDCTL & CH1_HICLK)) || (c == CHAN3 && (p->AUDCTL & CH3_HICLK));
}
static inline CL_HOT bool ff_high(const pokey_t *p, int c) {       // a joined pair's high channel (counts the low's borrows)
	return (c == CHAN2 && (p->AUDCTL & CH12_JOINED)) || (c == CHAN4 && (p->AUDCTL & CH34_JOINED));
}
static inline CL_HOT bool ff_low(const pokey_t *p, int c) {
	return (c == CHAN1 && (p->AUDCTL & CH12_JOINED)) || (c == CHAN3 && (p->AUDCTL & CH34_JOINED));
}
static inline CL_HOT int ff_len(const pokey_t *p, int c) {         // a borrow's length (inc_chan's cycles)
	return ff_fast(p, c) ? (ff_low(p, c) ? 7 : 4) : 1;
}

// the step of the n-th tick (n >= 1) of channel c's clock at or after step a
static inline CL_HOT uint64_t ff_tick_n(const pokey_t *p, int c, uint64_t a, uint32_t n) {
	if (ff_fast(p, c)) return a + n - 1;
	uint32_t div = (uint32_t)ff_div(p);
	uint32_t m = (ff_clk0(p) + (uint32_t)(a - p->sc0) + 1) % div;
	return a + (m ? div - m : 0) + (uint64_t)(n - 1) * div;
}
// channel c's ticks in steps a .. b-1
static inline CL_HOT uint32_t ff_ticks(const pokey_t *p, int c, uint64_t a, uint64_t b) {
	if (ff_fast(p, c)) return (uint32_t)(b - a);
	uint32_t div = (uint32_t)ff_div(p), k = ff_clk0(p);
	return (k + (uint32_t)(b - p->sc0)) / div - (k + (uint32_t)(a - p->sc0)) / div;
}

// process_channel() at step k
static inline CL_HOT void ff_process(pokey_t *p, int c, uint64_t k) {
	uint32_t o = (uint32_t)(k - p->sc0) + 1;
	if ((p->AUDC[c] & NOTPOLY5) || (poly5[(p->p5 + o) % 31] & 1)) {
		if (p->AUDC[c] & PURE) p->output[c] ^= 1;
		else if (p->AUDC[c] & POLY4) p->output[c] = poly4[(p->p4 + o) % 15] & 1;
		else if (p->AUDCTL & POLY9) p->output[c] = poly9[(p->p9 + o) % 511] & 1;
		else p->output[c] = (uint8_t)p17_bit((int32_t)((p->p17 + o) % P17N));
	}
}

// the next completion of channel c (not a high one) from its counter
static inline CL_HOT void ff_plan(pokey_t *p, int c) {
	uint32_t n = ((255u - (uint32_t)p->cv[c]) & 0xff) + 1;
	p->W[c] = ff_tick_n(p, c, p->rc[c], n);
	p->C[c] = p->W[c] + (uint64_t)(ff_len(p, c) - 1);
}

// no one hears channel c, nor does it matter to another one's sound: no
// volume (or volume only), and no filter looks at it, nor does it move one
static inline CL_HOT bool ff_unheard(const pokey_t *p, int c) {
	if ((p->AUDC[c] & VOLUME_MASK) && !(p->AUDC[c] & VOLUME_ONLY)) return false;
	switch (c) {
	case CHAN1: return !(p->AUDCTL & CH1_FILTER);
	case CHAN2: return !(p->AUDCTL & CH2_FILTER);
	case CHAN3: return !(p->AUDCTL & CH1_FILTER) && p->filter[CHAN1] == 1;
	default: return !(p->AUDCTL & CH2_FILTER) && p->filter[CHAN2] == 1;
	}
}

static CL_HOT void to_fast(pokey_t *p) {
	p->sc0 = p->sc;
	p->quiet = 0;
	for (int c = 0; c < 4; c++) {
		p->cv[c] = p->counter[c];
		p->rc[c] = p->sc;
		p->C[c] = p->W[c] = NEVER_C;
		if (ff_high(p, c)) continue;
		if (p->borrow[c] > 0) p->C[c] = p->sc + (uint64_t)p->borrow[c] - 1;    // (cv counts from its wrap)
		else ff_plan(p, c);
		bool q = ff_unheard(p, c);
		if (ff_low(p, c)) q = q && ff_unheard(p, c + 1) && p->AUDF[c + 1] == 0;
		if (q) p->quiet |= (uint8_t)(1u << c);
	}
	p->ff = true;
}

// a channel's completion at step k (a low one's with its high one's, if
// that wraps): whether its counter reloaded
static inline CL_HOT bool ff_complete(pokey_t *p, int c, uint64_t k) {
	bool reload = true;
	if (ff_low(p, c)) {
		int h = c + 1;
		p->cv[h] = (p->cv[h] + 1) & 0xff;
		ff_process(p, c, k);
		if (c == CHAN3) p->filter[CHAN1] = (p->AUDCTL & CH1_FILTER) ? p->output[CHAN1] : 1;
		if (p->cv[h] == 0) {            // the high one's borrow: at once
			p->cv[c] = p->AUDF[c] ^ 0xff;
			p->rc[c] = k + 1;
			p->cv[h] = p->AUDF[h] ^ 0xff;
			ff_process(p, h, k);
			if (h == CHAN4) p->filter[CHAN2] = (p->AUDCTL & CH2_FILTER) ? p->output[CHAN2] : 1;
		} else {
			reload = false;
			if (p->W[c] != NEVER_C) {       // the low one counts on from its wrap
				p->cv[c] = 0;
				p->rc[c] = p->W[c] + 1;
			}
		}
	} else {
		p->cv[c] = p->AUDF[c] ^ 0xff;
		p->rc[c] = k + 1;
		ff_process(p, c, k);
		if (c == CHAN3) p->filter[CHAN1] = (p->AUDCTL & CH1_FILTER) ? p->output[CHAN1] : 1;
		if (c == CHAN4) p->filter[CHAN2] = (p->AUDCTL & CH2_FILTER) ? p->output[CHAN2] : 1;
	}
	ff_plan(p, c);
	return reload;
}

static CL_HOT void ff_run(pokey_t *p, uint64_t upto) {
	const uint8_t act = (uint8_t)~p->quiet;
	for (;;) {
		uint64_t e = NEVER_C;
		for (int c = 0; c < 4; c++)
			if ((act & (1u << c)) && p->C[c] < e) e = p->C[c];
		if (p->inval && p->sc < e) e = p->sc;           // a register's new level, at the first clock
		if (e >= upto) break;
		static CL_DATA const uint8_t order[4] = { CHAN3, CHAN4, CHAN1, CHAN2 };    // (CL_DATA: in RAM on the cart, as the code)
		for (int i = 0; i < 4; i++) {
			int c = order[i];
			if ((act & (1u << c)) && p->C[c] == e) {
#ifdef CL_COUNT_EVENTS
				cl_ev_count[p->id][c]++;
#endif
				ff_complete(p, c, e);
			}
		}
		uint32_t sum = 0;
		for (int c = 0; c < 4; c++)
			if ((p->output[c] ^ p->filter[c]) || (p->AUDC[c] & VOLUME_ONLY))
				sum |= (uint32_t)(p->AUDC[c] & VOLUME_MASK) << (c * 4);
		if (p->out_raw != sum) {
			snd_advance(p, e + 1);
			p->level = level_of(sum);
			if (cl_pokey_hook) cl_pokey_hook(p->id, e, sum);
			p->out_raw = sum;
		}
		p->inval = false;
		p->sc = e + 1;
	}
	p->sc = upto;
}

// process_channel() for channel c at n steps first, first + P, ...: where its output ends up
static CL_HOT void ff_run_output(pokey_t *p, int c, uint64_t first, uint32_t n, uint32_t P) {
	uint8_t a = p->AUDC[c];
	if (a & PURE) {
		uint32_t cnt = n;
		if (!(a & NOTPOLY5)) {          // the gate's openings: 31 steps on, the same again
			uint32_t per = 0, rem = 0, full = n / 31, r = n % 31, i = (p->p5 + (uint32_t)(first - p->sc0) + 1) % 31, d = P % 31;
			for (uint32_t j = 0; j < 31; j++) {
				uint32_t g = poly5[i] & 1;
				per += g;
				if (j < r) rem += g;
				i += d;
				if (i >= 31) i -= 31;
			}
			cnt = full * per + rem;
		}
		p->output[c] ^= (uint8_t)(cnt & 1);
		return;
	}
	uint64_t last = first + (uint64_t)(n - 1) * P, k = last;
	if (!(a & NOTPOLY5)) {              // the last whose gate was open
		bool found = false;
		uint32_t back = n < 31 ? n : 31;
		for (uint32_t j = 0; j < back; j++) {
			k = last - (uint64_t)j * P;
			if (poly5[(p->p5 + (uint32_t)(k - p->sc0) + 1) % 31] & 1) { found = true; break; }
		}
		if (!found) return;
	}
	uint32_t o = (uint32_t)(k - p->sc0) + 1;
	if (a & POLY4) p->output[c] = poly4[(p->p4 + o) % 15] & 1;
	else if (p->AUDCTL & POLY9) p->output[c] = poly9[(p->p9 + o) % 511] & 1;
	else p->output[c] = (uint8_t)p17_bit((int32_t)((p->p17 + o) % P17N));
}

// a put-off channel's completions before step t: its output (and its high
// one's), counter and next completion. Once it has reloaded they come every
// P clocks (a pair's high one wrapping at each), and where the outputs
// stand is a sum over at most 31 of them.
static CL_HOT void ff_catch_up(pokey_t *p, int c, uint64_t t) {
	for (;;) {                          // to a reload: the first may be off the beat, and a pair's
		if (p->C[c] >= t) return;       // high one may take some to wrap
		if (ff_complete(p, c, p->C[c])) break;
	}
	if (p->C[c] >= t) return;
	uint32_t P = ff_fast(p, c) ? (uint32_t)p->AUDF[c] + (uint32_t)ff_len(p, c) : ((uint32_t)p->AUDF[c] + 1) * (uint32_t)ff_div(p);
	uint32_t n = (uint32_t)(t - 1 - p->C[c]) / P + 1;
	uint64_t first = p->C[c], last = first + (uint64_t)(n - 1) * P;
	ff_run_output(p, c, first, n, P);
	if (ff_low(p, c)) {
#ifdef CL_COUNT_EVENTS
		cl_pair_runs++;
		cl_pair_steps += n;
#endif
		ff_run_output(p, c + 1, first, n, P);
		p->cv[c + 1] = p->AUDF[c + 1] ^ 0xff;
	}
	p->cv[c] = p->AUDF[c] ^ 0xff;
	p->rc[c] = last + 1;
	p->C[c] = last + P;
	p->W[c] = p->C[c] - (uint64_t)ff_len(p, c) + 1;
}

// the chip's state at its clock as step_one_clock() keeps it
static CL_HOT void to_generic(pokey_t *p) {
	if (!p->ff) return;
	uint64_t t = p->sc;
	for (int c = 0; c < 4; c++) {
		if (p->quiet & (1u << c)) ff_catch_up(p, c, t);
		if (ff_high(p, c)) {
			p->counter[c] = p->cv[c];
			p->borrow[c] = 0;
			continue;
		}
		p->counter[c] = (int32_t)(((uint32_t)p->cv[c] + ff_ticks(p, c, p->rc[c], t)) & 0xff);
		bool wrapped = p->W[c] == NEVER_C || p->W[c] < t;
		p->borrow[c] = (p->C[c] != NEVER_C && wrapped && t <= p->C[c]) ? (int32_t)(p->C[c] - t + 1) : 0;
	}
	uint32_t o = (uint32_t)(t - p->sc0);
	if (p->pot_counter < 228) {
		uint32_t inc = (p->SKCTL & SK_PADDLE) ? o : ((uint32_t)p->clk[CLK_114] + o) / DIV_15;
		uint32_t pc = p->pot_counter + inc;
		p->pot_counter = (uint8_t)(pc < 228 ? pc : 228);
	}
	p->p4 = (p->p4 + o) % 15;
	p->p5 = (p->p5 + o) % 31;
	p->p9 = (p->p9 + o) % 511;
	p->p17 = (p->p17 + o) % P17N;
	p->clk[CLK_28] = (int32_t)(((uint32_t)p->clk[CLK_28] + o) % DIV_64);
	p->clk[CLK_114] = (int32_t)(((uint32_t)p->clk[CLK_114] + o) % DIV_15);
	p->sc0 = t;
	p->ff = false;
}

// the chip stepped to its clock upto
static CL_HOT void pk_sync(pokey_t *p, uint64_t upto) {
	if (p->sc >= upto) return;
	if (cl_pokey_exact || !(p->SKCTL & SK_RESET)) {    // (the clocks stopped: the borrows run out, clock by clock)
		to_generic(p);
		while (p->sc < upto) {
			if (!cl_pokey_exact && !p->inval && !p->borrow[0] && !p->borrow[1] && !p->borrow[2] && !p->borrow[3]) {
				p->sc = upto;           // nothing more moves
				break;
			}
			step_one_clock(p);
			p->sc++;
		}
		return;
	}
	if (!p->ff) to_fast(p);
	ff_run(p, upto);
	if (p->sc - p->sc0 > (1u << 28)) {  // (the offsets stay 32-bit)
		to_generic(p);
		to_fast(p);
	}
}

// where the pots' scan ends (step_pot's sync: every POTx is 228, so at 228)
static CL_HOT void pot_plan(pokey_t *p) {
	if (!(p->SKCTL & SK_RESET) || p->pot_counter >= 228) p->pot_due = NEVER_CLK;
	else if (p->SKCTL & SK_PADDLE) p->pot_due = p->sc + (uint64_t)(228 - p->pot_counter) - 1;
	else p->pot_due = p->sc + (uint64_t)(DIV_15 - p->clk[CLK_114]) - 1 + (uint64_t)(228 - p->pot_counter - 1) * DIV_15;
}

// a POKEY's timeslice: its clocks counted, up to the pots' sync if it comes (which ends it)
static inline CL_HOT int pokey_run(int k, int cycles) {
	pokey_t *p = &pk[k];
	exec_t *e = &ex[EX_POKEY1 + k];
	if (p->pot_due < e->total + (uint64_t)cycles) {
		int n = (int)(p->pot_due - e->total) + 1;
		at_t t = (e->lc + n - 1) * e->aspc;
		bool first = t < head_expire();
		pend_add(t, SY_POT, k << 8 | 0xff);
		p->pot_due = NEVER_CLK;
		if (first) return n;            // (abort_timeslice: the clocks to here, this one too)
	}
	return cycles;
}

static CL_HOT void pokey_potgo(pokey_t *p) {
	p->ALLPOT = 0;
	p->pot_counter = 0;
	for (int i = 0; i < 8; i++) p->POTx[i] = 228;  // (no pot callbacks)
}

static CL_HOT void pokey_start(pokey_t *p, int id) {     // device_start
	memset(p, 0, sizeof *p);
	p->id = id;
	for (int i = 0; i < 4; i++) p->AUDC[i] = 0xb0;
	p->KBCODE = 0x09;
	p->IRQST = IRQ_SEROC;
	p->inval = true;
	p->level = level_of(0);
	p->pot_due = NEVER_CLK;
}

static CL_HOT void pokey_write(pokey_t *p, int offset, uint8_t data) {   // write_internal
	switch (offset & 15) {
	case 0x00: p->AUDF[CHAN1] = data; break;
	case 0x01: p->AUDC[CHAN1] = data; p->inval = true; break;
	case 0x02: p->AUDF[CHAN2] = data; break;
	case 0x03: p->AUDC[CHAN2] = data; p->inval = true; break;
	case 0x04: p->AUDF[CHAN3] = data; break;
	case 0x05: p->AUDC[CHAN3] = data; p->inval = true; break;
	case 0x06: p->AUDF[CHAN4] = data; break;
	case 0x07: p->AUDC[CHAN4] = data; p->inval = true; break;
	case 0x08:
		if (data == p->AUDCTL) return;
		p->AUDCTL = data;
		p->inval = true;
		break;
	case 0x09:                          // STIMER
		for (int i = 0; i < 4; i++) {
			chan_reset(p, i);
			p->output[i] = 0;
			p->filter[i] = i < 2 ? 1 : 0;
		}
		p->inval = true;
		break;
	case 0x0a: p->SKSTAT &= ~(SK_FRAME | SK_OVERRUN | SK_KBERR); break;
	case 0x0b:
		if (p->SKCTL & SK_RESET) pokey_potgo(p);
		pot_plan(p);
		break;
	case 0x0d: p->SEROUT = data; break;
	case 0x0e:                          // IRQEN (its IRQ output isn't connected)
		if (data & 7) cl_unsupported |= 2;
		if (p->IRQST & ~data) p->IRQST &= (IRQ_SEROC | data);
		p->IRQEN = data;
		break;
	case 0x0f:
		if (data == p->SKCTL) return;
		if (data & (SK_TWOTONE | SK_ICLK | SK_OCLK)) cl_unsupported |= 4;
		p->SKCTL = data;
		if (!(data & SK_RESET)) {
			pokey_write(p, 0x0e, 0);
			pokey_write(p, 0x0a, 0);
			p->p9 = p->p17 = p->p4 = p->p5 = 0;
			p->clk[0] = p->clk[1] = p->clk[2] = 0;
			p->SKSTAT &= ~SK_BUSY;
			tm_adjust(TM_SEROC1 + p->id, 0, 0);
		}
		if (!(data & SK_KEYSCAN)) p->SKSTAT &= ~SK_KEYBD;
		if ((data & SK_ASYNC) && !(p->SKSTAT & SK_BUSY)) {
			chan_reset(p, CHAN3);
			chan_reset(p, CHAN4);
		}
		p->inval = true;
		pot_plan(p);
		break;
	}
}

// the pots' counter at the chip's clock (the rest of the chip can wait)
static CL_HOT uint8_t pot_now(pokey_t *p, uint64_t clock) {
	if (!p->ff) pk_sync(p, clock);
	if (!p->ff || p->pot_counter >= 228) return p->pot_counter;
	uint32_t o = (uint32_t)(clock - p->sc0);
	uint32_t inc = (p->SKCTL & SK_PADDLE) ? o : ((uint32_t)p->clk[CLK_114] + o) / DIV_15;
	uint32_t pc = p->pot_counter + inc;
	return (uint8_t)(pc < 228 ? pc : 228);
}

static CL_HOT uint8_t pokey_read(int k, int offset) {
	pokey_t *p = &pk[k];
	uint64_t clock = ex[EX_POKEY1 + k].total;
	synchronize(SY_NONE, 0);            // force resync
#ifdef CL_COUNT_EVENTS
	cl_rd_count[k][offset & 15]++;
#endif
	switch (offset & 15) {
	case 0x0a: {                        // RANDOM: the polynomials' places at the chip's clock
		uint32_t n = (p->SKCTL & SK_RESET) ? (uint32_t)(clock - (p->ff ? p->sc0 : p->sc)) : 0;
		if (p->AUDCTL & POLY9) return poly9[(p->p9 + n) % 511] & 0xff;
		return p17_random((p->p17 + n) % P17N);
	}
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: {
		int pot = offset & 7;
		return (p->ALLPOT & (1 << pot)) ? p->POTx[pot] : pot_now(p, clock);
	}
	case 0x08:
		if ((p->SKCTL & SK_RESET) == 0) return p->ALLPOT;
		p->ALLPOT = k == 0 ? cl_in.start : cl_in.dsw;   // allpot_r: START, DSW
		return p->ALLPOT;
	case 0x09: return p->KBCODE;
	case 0x0d: return p->SERIN;
	case 0x0e: return p->IRQST ^ 0xff;
	case 0x0f: return p->SKSTAT ^ 0xff;
	default: return 0xff;
	}
}

void cl_pokey_regs(int n, cl_pokey_regs_t *r) {
	const pokey_t *p = &pk[n & 1];
	memcpy(r->audf, p->AUDF, 4);
	memcpy(r->audc, p->AUDC, 4);
	r->audctl = p->AUDCTL;
	r->skctl = p->SKCTL;
	r->out_raw = p->out_raw;
}

// ---------------------------------------------------------------------------
// the sound: each POKEY's level summed over its samples, into bins

#ifndef CL_NBIN
#define CL_NBIN 1024
#endif
static int32_t bin_sum[2][CL_NBIN];
static uint16_t bin_n[CL_NBIN];
static uint32_t bin_w[2], bin_r;
static uint64_t bin_step_fx;            // samples a bin, 32.32
static uint64_t bin_end[2];             // where each POKEY's current bin ends, 32.32
static int32_t bin_acc[2];

void cl_audio_rate_q8(uint32_t hz_q8) {
	bin_step_fx = ((uint64_t)CL_POKEY_CLOCK << 40) / hz_q8;
	for (int i = 0; i < 2; i++) {
		bin_end[i] = (pk[i].snd_pos << 32) + bin_step_fx;
		bin_acc[i] = 0;
	}
	bin_w[0] = bin_w[1] = bin_r = 0;
}

static CL_HOT void snd_advance(pokey_t *p, uint64_t upto) {
	int i = p->id;
	while (p->snd_pos < upto) {
		uint64_t end = (bin_end[i] + 0xffffffffu) >> 32;   // the bin's first sample past its end
		uint64_t m = (end < upto ? end : upto) - p->snd_pos;
		bin_acc[i] += p->level * (int32_t)m;
		p->snd_pos += m;
		if (p->snd_pos == end) {
			uint32_t w = bin_w[i];
			if (w - bin_r < CL_NBIN) {
				bin_sum[i][w % CL_NBIN] = bin_acc[i];
				if (i == 0) bin_n[w % CL_NBIN] = (uint16_t)(end - (((bin_end[i] - bin_step_fx) + 0xffffffffu) >> 32));
				bin_w[i] = w + 1;
			}
			bin_acc[i] = 0;
			bin_end[i] += bin_step_fx;
		}
	}
}

CL_HOT int cl_audio_take(int32_t *sum1, int32_t *sum2, uint16_t *count, int max) {
	for (int i = 0; i < 2; i++) {       // the chips to their clocks, the samples to there
		pk_sync(&pk[i], ex[EX_POKEY1 + i].total);
		snd_advance(&pk[i], ex[EX_POKEY1 + i].total);
	}
	int k = 0;
	while (k < max && bin_r != bin_w[0] && bin_r != bin_w[1]) {
		sum1[k] = bin_sum[0][bin_r % CL_NBIN];
		sum2[k] = bin_sum[1][bin_r % CL_NBIN];
		count[k] = bin_n[bin_r % CL_NBIN];
		bin_r++;
		k++;
	}
	return k;
}

// ---------------------------------------------------------------------------
// the latch, the watchdog, the palette, the graph processor, memory

static const uint8_t pal_level[8] = { 0, 74, 82, 157, 98, 173, 181, 255 };     // resistor weights 10k, 4.7k, 2.2k

uint32_t cl_pal_rgb(uint16_t pal) {
	unsigned v = ~pal;
	return (uint32_t)pal_level[(v >> 6) & 7] << 16 | (uint32_t)pal_level[(v >> 3) & 7] << 8 | pal_level[v & 7];
}

static void set_pen(int i) { cl_rgb[i] = cl_pal_rgb(cl_pal[i]); }

static CL_HOT void latch_bit(int bit, int state) {     // ls259_device::write_bit (Q3: flip_screen_set)
	if (((cl_latch >> bit) & 1) == state) return;
	cl_latch ^= (uint8_t)(1u << bit);
}

static inline CL_HOT uint8_t bm_get(int b, uint8_t x, uint8_t y) {
	uint8_t v = cl_bm[b][(y << 7) | (x >> 1)];
	return (x & 1) ? v >> 4 : v & 15;
}

static inline CL_HOT void bm_put(int b, uint8_t x, uint8_t y, uint8_t d) {
	uint8_t *v = &cl_bm[b][(y << 7) | (x >> 1)];
	*v = (x & 1) ? (uint8_t)((*v & 0x0f) | (d << 4)) : (uint8_t)((*v & 0xf0) | d);
}

static inline CL_HOT void adjust_xy(int offset) {
	switch (offset) {
	case 0: cl_bm_x--; cl_bm_y++; break;
	case 1: cl_bm_y--; break;
	case 2: cl_bm_x--; break;
	case 4: cl_bm_x++; cl_bm_y++; break;
	case 5: cl_bm_y++; break;
	case 6: cl_bm_x++; break;
	}
}

static CL_HOT uint8_t main_io_rd(uint16_t a) {
	switch (a >> 8) {
	case 0x10: if (a < 0x1010) return pokey_read(0, a & 15); break;
	case 0x18: if (a < 0x1810) return pokey_read(1, a & 15); break;
	case 0x20: if (a == 0x2000) return cl_in.p1; break;
	case 0x22: if (a == 0x2200) return 0xff; break;
	case 0x24: if (a == 0x2400) return (uint8_t)((cl_in.system & 0xfe) | (in_vblank() ? 0 : 1)); break;
	}
	return 0;
}

static CL_HOT void main_io_wr(uint16_t a, uint8_t d) {
	switch (a >> 8) {
	case 0x10: if (a < 0x1010) synchronize(SY_PWRITE, 0 << 16 | (a & 15) << 8 | d); break;
	case 0x18: if (a < 0x1810) synchronize(SY_PWRITE, 1 << 16 | (a & 15) << 8 | d); break;
	case 0x32:
		if (a < 0x3280) {
			cl_pal[a & 0x3f] = (uint16_t)(((a & 0x40) << 2) | d);
			set_pen(a & 0x3f);
		}
		break;
	case 0x38: if (a < 0x3808) latch_bit(a & 7, d >> 7); break;
	case 0x3a: if (a == 0x3a00) tm_adjust(TM_WATCHDOG, 3 * AS_PER_SEC, 0); break;
	case 0x3c: if (a == 0x3c00) set_input_line(0, CLEAR_LINE); break;
	case 0x3e: break;                   // nvram_enable_w: kept but never used
	}
}

static CL_HOT uint8_t slave_io_rd(uint16_t a) {
	if (a < 0x100) {
		if ((a & 0xf8) == 0x08) {       // graph_processor_r
			uint8_t r = bm_get(!cl_bm_sel, cl_bm_x, cl_bm_y);
			adjust_xy(a & 7);
			return r;
		}
		return cl_sram[a];
	}
	return 0;
}

static CL_HOT void slave_io_wr(uint16_t a, uint8_t d) {
	if (a < 0x100) {
		if ((a & 0xf8) == 0x08) {       // graph_processor_w
			switch (a & 7) {
			case 3: cl_bm_x = d; break;
			case 7: cl_bm_y = d; break;
			default:
				bm_put(cl_bm_sel, cl_bm_x, cl_bm_y, d & 0x0f);
				adjust_xy(a & 7);
				break;
			}
			return;
		}
		cl_sram[a] = d;
		return;
	}
	if (a == 0x1000) set_input_line(1, CLEAR_LINE);
	else if (a == 0x1200) {             // clearbmp_w
		update_partial(vpos());
		if ((d & 1) != cl_bm_sel) cl_bm_shows++;
		cl_bm_sel = d & 1;
		if (d & 2) memset(cl_bm[cl_bm_sel], 0, sizeof cl_bm[0]);
		cl_stats.flips++;
	}
}

static void map_pages(int n) {           // CPU n's pages into page_rd / page_wr
	const uint8_t **rd = page_rd;
	uint8_t **wr = page_wr;
	for (int p = 0; p < 256; p++) { rd[p] = zeros; wr[p] = sink; }
	for (int p = 0; p < 8; p++) rd[8 + p] = wr[8 + p] = cl_shared + (p << 8);
	if (!n) {
		for (int p = 0; p < 4; p++) { rd[p] = wr[p] = cl_ram + (p << 8); rd[4 + p] = wr[4 + p] = cl_vram + (p << 8); }
		rd[0x10] = rd[0x18] = rd[0x20] = rd[0x22] = rd[0x24] = NULL;
		wr[0x10] = wr[0x18] = wr[0x32] = wr[0x38] = wr[0x3a] = wr[0x3c] = wr[0x3e] = NULL;
		rd[0x28] = wr[0x28] = cl_nvram;
		rd[0x29] = wr[0x29] = cl_nvram + 0x100;
		rd[0x30] = wr[0x30] = cl_spr;
		for (int p = 0x40; p < 0x100; p++) rd[p] = roms->main + ((p - 0x40) << 8);
	} else {
		rd[0] = NULL; wr[0] = NULL;     // 0x0008-0x000f: the graph processor
		for (int p = 1; p < 8; p++) rd[p] = wr[p] = cl_sram + (p << 8);
		wr[0x10] = wr[0x12] = NULL;
		for (int p = 0x20; p < 0x100; p++) rd[p] = roms->slave + ((p - 0x20) << 8);
	}
}

// ---------------------------------------------------------------------------
// video (cloak.cpp's screen_update: the playfield, the bitmap, the sprites)

uint32_t cl_char_rows[256][8];          // 8 pens a row, x 0 in the low nibble
uint32_t cl_sprite_rows[128][16];

static void gfx_init(void) {
	// charlayout / spritelayout: 4 planes at bits 0-3 (plane 0 the pen's top
	// bit), x at bits 0x8000, 0x8004, 0, 4, 0x8008, 0x800c, 8, 12, 16 bits a row
	static const int xo[8] = { 0x8000, 0x8004, 0, 4, 0x8008, 0x800c, 8, 12 };
	for (int s = 0; s < 2; s++) {
		const uint8_t *g = s ? roms->sprites : roms->chars;
		int n = s ? 128 : 256, h = s ? 16 : 8, inc = s ? 256 : 128;
		for (int t = 0; t < n; t++)
			for (int y = 0; y < h; y++) {
				uint32_t row = 0;
				for (int x = 0; x < 8; x++) {
					int pen = 0;
					for (int pl = 0; pl < 4; pl++) {
						int bit = t * inc + pl + xo[x] + y * 16;
						if (g[bit >> 3] & (0x80 >> (bit & 7))) pen |= 8 >> pl;
					}
					row |= (uint32_t)pen << (4 * x);
				}
				if (s) cl_sprite_rows[t][y] = row;
				else cl_char_rows[t][y] = row;
			}
	}
}

void cl_render(cl_fb_t fb, int y0, int y1) {
	bool flip = (cl_latch >> 3) & 1;
	const uint8_t *bm = cl_bm[!cl_bm_sel];
	for (int y = y0; y <= y1; y++) {
		uint8_t *row = fb[y];
		int ty = flip ? 255 - y : y;
		for (int cx = 0; cx < 32; cx++) {
			if (!flip) {
				uint32_t r = cl_char_rows[cl_vram[(ty >> 3) * 32 + cx]][ty & 7];
				for (int x = 0; x < 8; x++) row[cx * 8 + x] = (uint8_t)((r >> (4 * x)) & 15);
			} else {
				uint32_t r = cl_char_rows[cl_vram[(ty >> 3) * 32 + (31 - cx)]][ty & 7];
				for (int x = 0; x < 8; x++) row[cx * 8 + x] = (uint8_t)((r >> (4 * (7 - x))) & 15);
			}
		}
		const uint8_t *b = bm + (y << 7);
		for (int x = 0; x < 256; x += 2) {
			uint8_t v = b[x >> 1];
			if (v & 7) row[(x - 6) & 0xff] = (uint8_t)(0x10 | ((x & 0x80) >> 4) | (v & 7));
			if (v & 0x70) row[(x - 5) & 0xff] = (uint8_t)(0x10 | ((x & 0x80) >> 4) | ((v >> 4) & 7));
		}
	}
	for (int offs = 63; offs >= 0; offs--) {
		int code = cl_spr[offs + 64] & 0x7f;
		bool fx = cl_spr[offs + 64] & 0x80, fy = false;
		int sx = cl_spr[offs + 192], sy = 240 - cl_spr[offs];
		if (flip) { sx -= 9; sy = 240 - sy; fx = !fx; fy = true; }
		for (int j = 0; j < 16; j++) {
			int py = sy + j;
			if (py < y0 || py > y1) continue;
			uint32_t r = cl_sprite_rows[code][fy ? 15 - j : j];
			for (int i = 0; i < 8; i++) {
				int px = sx + i;
				if (px < 0 || px > 255) continue;
				int pen = (r >> (4 * (fx ? 7 - i : i))) & 15;
				if (pen) fb[py][px] = (uint8_t)(32 + pen);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// timers

static CL_HOT void execute_timers(void) {
	for (;;) {
		int h = reg_head();
		bool p = npend && (h < 0 || PEND0.t < tm[h].expire || (PEND0.t == tm[h].expire && PEND0.seq < tm[h].seq));
		if (!p && h < 0) break;
		at_t e = p ? PEND0.t : tm[h].expire;
		if (e > basetime) break;
		in_callback = true;
		cb_expire = e;
		if (p) {                        // a synchronize()'s callback
			pend_t s = PEND0;
			npend--;
			cb_timer = -1;
			switch (s.type) {
			case SY_IRQ: irq_events(s.param); break;
			case SY_PWRITE: {
				pokey_t *pp = &pk[s.param >> 16];
#ifdef CL_COUNT_EVENTS
				{
					int r = (s.param >> 8) & 15;
					uint8_t old = r < 8 ? ((r & 1) ? pp->AUDC[r >> 1] : pp->AUDF[r >> 1]) : r == 8 ? pp->AUDCTL : r == 15 ? pp->SKCTL : 0xff;
					cl_wr_count[pp->id][r][old == (uint8_t)s.param]++;
				}
#endif
				int r = (s.param >> 8) & 15;
				if (r < 8 && !cl_pokey_exact && ((r & 1) ? pp->AUDC[r >> 1] : pp->AUDF[r >> 1]) == (uint8_t)s.param)
					break;              // (the same value again: nothing changes)
				pk_sync(pp, ex[EX_POKEY1 + pp->id].total);
				to_generic(pp);
				pokey_write(pp, r, (uint8_t)s.param);
				break;
			}
			case SY_POT: pk[s.param >> 8].ALLPOT |= (uint8_t)s.param; break;
			}
			in_callback = false;
			continue;
		}
		tmr_t *t = &tm[h];
		at_t expire = t->expire;
		if (!t->period) { t->expire = AT_NEVER; tm_head = -2; }    // a one shot is off now
		cb_timer = h;
		cb_modified = false;
		switch (h) {
		case TM_VBSTART: cb_vbstart(); break;
		case TM_VBEND: break;
		case TM_SCAN0: reset_partial_updates(); break;
		case TM_SND50: break;           // the sound manager's 50 Hz streams update
		case TM_MAINIRQ: set_input_line(0, HOLD_LINE); break;
		case TM_SLAVEIRQ: set_input_line(1, HOLD_LINE); break;
		case TM_WATCHDOG: watchdog_fired = true; break;
		case TM_SEROC1: case TM_SEROC2: pk[h - TM_SEROC1].IRQST |= IRQ_SEROC; break;
		}
		if (!cb_modified && t->period) {
			t->expire = expire + t->period;
			t->seq = ++tm_seq;
			tm_head = -2;
		}
		in_callback = false;
	}
}

// every time a second from the epoch: all times back by one
static CL_HOT void rebase(void) {
	epoch_s++;
	basetime -= AS_PER_SEC;
	for (int i = 0; i < TM_COUNT; i++)
		if (tm[i].expire != AT_NEVER) tm[i].expire -= AS_PER_SEC;
	for (int i = 0; i < npend; i++) pend[i].t -= AS_PER_SEC;
	for (int n = 0; n < EX_COUNT; n++) ex[n].lc -= ex[n].cps;
	vblank_start_time -= AS_PER_SEC;
	vblank_end_time -= AS_PER_SEC;
	last_partial_reset -= AS_PER_SEC;
}

// local < target (lu: the device's local time in units): target = max(local, basetime)
#define CL_PULL_BACK(lu_) do { \
	uint32_t l_ = (lu_); \
	if (l_ < tu || (l_ == tu && part)) { \
		at_t lt_ = (at_t)l_ * UNIT_AS; \
		if (lt_ > basetime) { target = lt_; tu = l_; part = false; } \
		else { target = basetime; tu = at_units(basetime, &part); } \
	} \
} while (0)

// MAME's timeslice loop (device_scheduler::timeslice), the execute list
// unrolled: the master (5 units a cycle), the slave and the POKEYs (4)
static CL_HOT void timeslice(void) {
	if (basetime >= AS_PER_SEC) rebase();
	at_t he;
	while (basetime < (he = head_expire())) {
		at_t target = basetime + QUANTUM_AS;
		if (he < target) target = he;
		cl_stats.slices++;
		bool part;
		uint32_t tu = at_units(target, &part);  // the target in units (and part of one)
		for (int n = 0; n < 2; n++) {           // the CPUs
			exec_t *e = &ex[n];
			int32_t cycles = (int32_t)(n ? tu >> 2 : tu / 5) - (int32_t)e->lc;
			if (cycles < 1) continue;
			m65p = &cpus[n];
			executing = n;
			int ran = m65_run(cycles);
			executing = -1;
			e->total += (uint64_t)ran;
			e->lc += ran;
			CL_PULL_BACK((uint32_t)e->lc * (n ? 4 : 5));
		}
		for (int n = EX_POKEY1; n <= EX_POKEY2; n++) {
			exec_t *e = &ex[n];
			int32_t cycles = (int32_t)(tu >> 2) - (int32_t)e->lc;
			if (cycles < 1) continue;
			int ran = pokey_run(n - EX_POKEY1, cycles);
			e->total += (uint64_t)ran;
			e->lc += ran;
			CL_PULL_BACK((uint32_t)e->lc * 4);
		}
		basetime = target;
	}
	execute_timers();
}

// ---------------------------------------------------------------------------

static void trace_hook(void) {
	cl_regs_t r = { m65p->NPC, m65p->SP, m65p->A, m65p->X, m65p->Y, m65p->P };
	int n = m65p->id;
	cl_trace(n, &r, ex[n].total + (uint64_t)m65_done());
}

void cl_regs(int n, cl_regs_t *r) {
	const m65_t *c = &cpus[n & 1];
	r->pc = c->NPC; r->sp = c->SP; r->a = c->A; r->x = c->X; r->y = c->Y; r->p = c->P;
}

uint64_t cl_main_cycles(void) { return ex[EX_MAIN].total + (executing == EX_MAIN ? (uint64_t)m65_done() : 0); }
uint64_t cl_slave_cycles(void) { return ex[EX_SLAVE].total + (executing == EX_SLAVE ? (uint64_t)m65_done() : 0); }
uint64_t cl_time(void) {
	bool part;
	return (uint64_t)epoch_s * CL_MAIN_CLOCK + at_units(basetime, &part) / ex[EX_MAIN].k;
}

static void cpu_setup(int n) {
	cpu_select(n);
	memset(m65p, 0, sizeof *m65p);
	m65_init();
	m65p->id = n;
	map_pages(n);
	m65_map(page_rd, page_wr);
	m65p->zp = n ? cl_sram : cl_ram;   // page 0's RAM
	m65p->zp_io = n;                    // the slave's graph processor at 0x0008-0x000f
	m65p->io_rd = n ? slave_io_rd : main_io_rd;
	m65p->io_wr = n ? slave_io_wr : main_io_wr;
	m65p->irq_ack = n ? irq_ack_slave : irq_ack_main;
	m65p->trace = cl_trace ? trace_hook : NULL;
}

static CL_HOT void board_reset(void) {         // every device's reset, in MAME's order
	for (int n = 0; n < 2; n++) {       // the CPUs, then their periodic interrupts (post reset)
		cpu_select(n);
		m65_reset();
		ex[n].total = 0;
		if (n == 0) tm_adjust(TM_MAINIRQ, MAIN_IRQ_AS, MAIN_IRQ_AS);
		else tm_adjust(TM_SLAVEIRQ, SLAVE_IRQ_AS, SLAVE_IRQ_AS);
	}
	for (int b = 0; b < 8; b++) latch_bit(b, 0);        // ls259: clear_outputs(0)
	tm_adjust(TM_WATCHDOG, AT_NEVER, 0);                // watchdog: off until the first kick
	for (int i = 0; i < 2; i++) {
		pk_sync(&pk[i], ex[EX_POKEY1 + i].total);
		to_generic(&pk[i]);
		pokey_potgo(&pk[i]);
		pot_plan(&pk[i]);
	}
}

void cl_init(const cl_roms_t *r) {
	roms = r;
	gfx_init();                         // first: the cart keeps the ROMs it reads in cl_bm's room
	for (int i = 0; i < TM_COUNT; i++) tm[i] = (tmr_t){ AT_NEVER, 0, 0 };
	tm_seq = 0;
	tm_head = -2;
	npend = 0;
	basetime = 0;
	epoch_s = 0;
	executing = -1;
	in_callback = false;
	exec_init(&ex[EX_MAIN], CL_MAIN_CLOCK);
	exec_init(&ex[EX_SLAVE], CL_SLAVE_CLOCK);
	exec_init(&ex[EX_POKEY1], CL_POKEY_CLOCK);
	exec_init(&ex[EX_POKEY2], CL_POKEY_CLOCK);
	memset(cl_ram, 0, sizeof cl_ram);
	memset(cl_vram, 0, sizeof cl_vram);
	memset(cl_shared, 0, sizeof cl_shared);
	memset(cl_spr, 0, sizeof cl_spr);
	memset(cl_sram, 0, sizeof cl_sram);
	memset(cl_bm, 0, sizeof cl_bm);
	memset(cl_pal, 0, sizeof cl_pal);
	for (int i = 0; i < 64; i++)          // palette_device: the pens start as a rainbow (rgbexpand<1,1,1>)
		cl_rgb[i] = (i & 1 ? 0xff0000u : 0) | (i & 2 ? 0x00ff00u : 0) | (i & 4 ? 0x0000ffu : 0);
	if (cl_frame_fb) memset(*cl_frame_fb, 0, sizeof(cl_fb_t));
	cl_bm_sel = cl_bm_x = cl_bm_y = 0;
	cl_bm_shows++;
	cl_frames = cl_resets = cl_unsupported = 0;
	memset(&cl_stats, 0, sizeof cl_stats);
	watchdog_fired = false;
	memset(line, 0, sizeof line);
	memset(zeros, 0, sizeof zeros);
	poly_init();
	vol_init();

	// device_start, in MAME's order: the CPUs, the latch, the screen's timers, the POKEYs
	cpu_setup(1);
	cpu_setup(0);
	cl_latch = 0;
	vblank_start_time = 0;
	vblank_end_time = VBLANK_AS;
	tm_adjust(TM_VBSTART, time_until_pos(0), 0);
	last_partial_reset = 0;             // (configure: vpos() is 0, so reset_partial_updates())
	last_partial_scan = 0;
	tm_adjust(TM_SCAN0, time_until_pos(0), 0);
	tm[TM_SND50] = (tmr_t){ SND50_AS, SND50_AS, ++tm_seq };
	tm_head = -2;
	pokey_start(&pk[0], 0);
	pokey_start(&pk[1], 1);
	if (!bin_step_fx) cl_audio_rate_q8(48000u << 8);
	else cl_audio_rate_q8((uint32_t)(((uint64_t)CL_POKEY_CLOCK << 40) / bin_step_fx));
	board_reset();
}

static CL_HOT void soft_reset(void) {          // schedule_soft_reset: every device's reset
	watchdog_fired = false;
	cl_resets++;
	board_reset();
}

CL_HOT uint32_t cl_run_to(uint64_t until) {
	uint32_t f0 = cl_frames;
	for (;;) {                          // while cl_time() < until
		int64_t u = (int64_t)(until - (uint64_t)epoch_s * CL_MAIN_CLOCK);
		if (basetime >= (u > 2 * CL_MAIN_CLOCK ? AT_NEVER : u * ex[EX_MAIN].aspc)) break;
		timeslice();
		if (watchdog_fired) soft_reset();
	}
	return cl_frames - f0;
}

bool cl_frame(void) {
	frame_done = false;
	while (!frame_done) timeslice();
	if (watchdog_fired) {
		soft_reset();
		return false;
	}
	return true;
}

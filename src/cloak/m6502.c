// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
//
// m6502.c -- the NMOS 6502 as MAME 0.288 emulates it (src/devices/cpu/m6502,
// m6502.cpp and om6502.lst), for Cloak & Dagger's two CPUs. m6502_body.h is
// made from MAME's instruction list by tools/m6502gen.py; this is the rest
// of m6502_device: its registers, the helpers the list calls and
// execute_run(). Each read or write is one cycle, and a timeslice can end
// after any of them, as in MAME.
//
// Q*bert's (../qbert/m6502.c) reworked for speed: the whole of execute_run()
// is one function (m65_run), whose locals are the registers, and m65p
// points at the running CPU's struct, where they're kept between
// timeslices. Each CPU's memory goes through its own page table (m65_map). MAME's
// license (BSD-3-Clause) is in README.md.
#include "m6502.h"

#include <stddef.h>
#include <stdint.h>

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define M65_HOT __attribute__((section(".time_critical.m65")))
// a table the hot code reads, in RAM too (gcc puts a static that is never
// written in .rodata, in flash, const or not)
#define M65_DATA __attribute__((section(".time_critical.m65_data")))
#else
#define M65_HOT
#define M65_DATA
#endif

enum { F_N = 0x80, F_V = 0x40, F_E = 0x20, F_B = 0x10, F_D = 0x08, F_I = 0x04, F_Z = 0x02, F_C = 0x01 };
#define STATE_RESET 0xff00

m65_t *m65p;

// (macros: in a function this size GCC doesn't inline them)
#define page_changing(base, delta) ((((uint16_t)(base) + (int)(delta)) ^ (uint16_t)(base)) & 0xff00)
#define set_l(base, val) ((uint16_t)(((base) & 0xff00) | (uint8_t)(val)))
#define set_h(base, val) ((uint16_t)(((base) & 0x00ff) | ((uint8_t)(val) << 8)))

void m65_init(void) {
	m65_t *c = m65p;
	c->PPC = c->PC = c->NPC = 0;
	c->A = 0x00; c->X = 0x80; c->Y = 0x00; c->P = 0x36; c->SP = 0x0100;
	c->TMP = 0; c->TMP2 = 0; c->IR = 0;
	c->nmi_state = c->irq_state = c->apu_irq_state = c->v_state = false;
	c->nmi_pending = c->irq_taken = false;
	c->inst_state = STATE_RESET;
	c->resume = NULL;
	c->inhibit_interrupts = false;
	c->icount = c->cycles_running = c->cycles_stolen = 0;
}

void m65_map(const uint8_t *const rd[256], uint8_t *const wr[256]) {
	m65_t *c = m65p;
	for (int p = 0; p < 256; p++) {
		c->map[p] = rd[p] ? (uint8_t *)(uintptr_t)(rd[p] - (p << 8)) : NULL;
		c->map[256 + p] = wr[p] ? wr[p] - (p << 8) : NULL;
	}
}

M65_HOT void m65_reset(void) {
	m65_t *c = m65p;
	c->inst_state = STATE_RESET;
	c->resume = NULL;
	c->nmi_pending = false;
	c->irq_taken = false;
	c->inhibit_interrupts = false;
}


M65_HOT void m65_set_irq(int state) { m65p->irq_state = state != 0; }

void m65_regs_of(const m65_t *c, m65_regs_t *r) {
	r->pc = c->NPC; r->sp = c->SP; r->a = c->A; r->x = c->X; r->y = c->Y; r->p = c->P;
}

// ---------------------------------------------------------------------------
// execute_run(), with the scheduler's accounting: MAME counts the cycles
// asked for, less those left (an instruction may end a timeslice one
// access past an abort) and those an abort took back

// the registers, as m6502_body.h (MAME's list) names them: m65_run's
// locals, but NPC and SP, which few instructions use
#define m_PC PC
#define m_NPC c->NPC
#define m_SP c->SP
#define m_TMP TMP
#define m_A A
#define m_X X
#define m_Y Y
#define m_P P
#define m_TMP2 TMP2
#define m_IR IR
#define m_inst_state c->inst_state
#define m65_icount icount
// the interrupt lines' state stays in the struct: the board moves it
#define m_nmi_pending c->nmi_pending
#define m_irq_state c->irq_state
#define m_apu_irq_state c->apu_irq_state
#define m_irq_taken c->irq_taken
#define m_inhibit_interrupts c->inhibit_interrupts

#define M65_SAVE() do { \
	c->PC = PC; c->TMP = TMP; c->A = A; c->X = X; c->Y = Y; c->P = P; c->TMP2 = TMP2; c->IR = IR; \
	c->icount = icount; \
} while (0)

// the address space: a page's bytes straight, or the board's handlers (a
// page without bytes, NULL in the map). The handlers that look at the time
// read the cycles done from the struct (m65_done), and one may end the
// timeslice (m65_abort), so the count goes there and back around them. (A
// page 0 that's I/O, the slave's, is its handler's too: its RAM but for the
// graph processor at 0x0008-0x000f. Its zero-page forms are read_zp's.)
#define M65_IO_RD(a_) ({ \
	uint8_t r_; \
	c->icount = icount; \
	r_ = c->io_rd(a_); \
	icount = c->icount; \
	r_; })
#define M65_IO_WR(a_, v_) do { \
	c->icount = icount; \
	c->io_wr(a_, v_); \
	icount = c->icount; \
} while (0)
#define read(adr) ({ \
	uint16_t a_ = (adr); \
	const uint8_t *p_ = c->map[a_ >> 8]; \
	uint8_t v_; \
	if (__builtin_expect(p_ != NULL, 1)) v_ = p_[a_]; \
	else v_ = M65_IO_RD(a_); \
	v_; })
#define write(adr, val) do { \
	uint16_t a_ = (adr); \
	uint8_t w_ = (uint8_t)(val); \
	uint8_t *p_ = c->map[256 + (a_ >> 8)]; \
	if (__builtin_expect(p_ != NULL, 1)) p_[a_] = w_; \
	else M65_IO_WR(a_, w_); \
} while (0)
// page 0 by the address's form (tools/m6502gen.py): its RAM, but for I/O at 0x0008-0x000f
#define read_zp(adr) ({ \
	uint8_t za_ = (uint8_t)(adr); \
	uint8_t v_; \
	if (__builtin_expect((za_ & 0xf8) == 0x08, 0) && c->zp_io) { \
		c->icount = icount; \
		v_ = c->io_rd(za_); \
		icount = c->icount; \
	} else \
		v_ = c->zp[za_]; \
	v_; })
#define write_zp(adr, val) do { \
	uint8_t za_ = (uint8_t)(adr); \
	uint8_t w_ = (uint8_t)(val); \
	if (__builtin_expect((za_ & 0xf8) == 0x08, 0) && c->zp_io) { \
		c->icount = icount; \
		c->io_wr(za_, w_); \
		icount = c->icount; \
	} else \
		c->zp[za_] = w_; \
} while (0)
#define read_sync(adr) read(adr)
#define read_arg(adr) read(adr)
#define read_pc() read(PC)

#define m65_irq_ack() c->irq_ack()
#define m65_nmi_ack() ((void)0)

// a bus access done: if the cycles have run out, the place to carry on from
#define M65_STOP(label) do { c->resume = &&label; goto m65_out; } while (0)

// the per-instruction hook (the tests' traces): the cart's build leaves it
// out (-DM65_NO_TRACE), a load and a branch an instruction
#ifndef M65_NO_TRACE
#define M65_TRACE
#endif
#define m65_traced (c->trace != NULL)
#define m65_trace_now() do { M65_SAVE(); c->trace(); } while (0)

#define dec_SP() (m_SP = set_l(m_SP, m_SP - 1))
#define inc_SP() (m_SP = set_l(m_SP, m_SP + 1))
#define prefetch_start() (m_NPC = PC)
#define prefetch_end() do { \
	if (__builtin_expect(c->lines != 0, 0) && \
	    (m_nmi_pending || ((m_irq_state || m_apu_irq_state) && !(P & F_I))) && !m_inhibit_interrupts) { \
		m_irq_taken = true; \
		IR = 0x00; \
	} else \
		PC++; \
} while (0)
#define prefetch_end_noirq() (PC++)

#define set_nz(v) do { \
	uint8_t nz_ = (v); \
	P = (uint8_t)((P & ~(F_Z | F_N)) | (nz_ & F_N) | (nz_ ? 0 : F_Z)); \
} while (0)

// the ALU helpers (m6502.cpp's do_*), on the locals
#define do_adc_d(v) do { \
	uint8_t val = (v); \
	uint8_t c_ = P & F_C ? 1 : 0; \
	P &= ~(F_N | F_V | F_Z | F_C); \
	uint8_t al = (A & 15) + (val & 15) + c_; \
	if (al > 9) al += 6; \
	uint8_t ah = (A >> 4) + (val >> 4) + (al > 15); \
	if (!(uint8_t)(A + val + c_)) P |= F_Z; \
	else if (ah & 8) P |= F_N; \
	if (~(A ^ val) & (A ^ (ah << 4)) & 0x80) P |= F_V; \
	if (ah > 9) ah += 6; \
	if (ah > 15) P |= F_C; \
	A = (ah << 4) | (al & 15); \
} while (0)
#define do_adc_nd(v) do { \
	uint8_t val = (v); \
	uint16_t sum = A + val + (P & F_C ? 1 : 0); \
	P &= ~(F_N | F_V | F_Z | F_C); \
	if (!(uint8_t)(sum)) P |= F_Z; \
	else if ((int8_t)(sum) < 0) P |= F_N; \
	if (~(A ^ val) & (A ^ sum) & 0x80) P |= F_V; \
	if (sum & 0xff00) P |= F_C; \
	A = sum; \
} while (0)
#define do_adc(v) do { uint8_t v1_ = (v); if (P & F_D) do_adc_d(v1_); else do_adc_nd(v1_); } while (0)
#define do_arr_nd() do { \
	bool c_ = P & F_C; \
	P &= ~(F_N | F_Z | F_C | F_V); \
	A >>= 1; \
	if (c_) A |= 0x80; \
	if (!A) P |= F_Z; \
	else if ((int8_t)(A) < 0) P |= F_N; \
	if (A & 0x40) P |= F_V | F_C; \
	if (A & 0x20) P ^= F_V; \
} while (0)
#define do_arr_d() do { \
	bool c_ = P & F_C; \
	P &= ~(F_N | F_Z | F_C | F_V); \
	uint8_t a = A >> 1; \
	if (c_) a |= 0x80; \
	if (!a) P |= F_Z; \
	else if ((int8_t)(a) < 0) P |= F_N; \
	if ((a ^ A) & 0x40) P |= F_V; \
	if ((A & 0x0f) >= 0x05) a = ((a + 6) & 0x0f) | (a & 0xf0); \
	if ((A & 0xf0) >= 0x50) { a += 0x60; P |= F_C; } \
	A = a; \
} while (0)
#define do_arr() do { if (P & F_D) do_arr_d(); else do_arr_nd(); } while (0)
#define do_cmp(v1, v2) do { \
	uint8_t x1_ = (v1), x2_ = (v2); \
	P &= ~(F_N | F_Z | F_C); \
	uint16_t r = x1_ - x2_; \
	if (!r) P |= F_Z; \
	else if ((int8_t)(r) < 0) P |= F_N; \
	if (!(r & 0xff00)) P |= F_C; \
} while (0)
#define do_sbc_d(v) do { \
	uint8_t val = (v); \
	uint8_t c_ = P & F_C ? 0 : 1; \
	P &= ~(F_N | F_V | F_Z | F_C); \
	uint16_t diff = A - val - c_; \
	uint8_t al = (A & 15) - (val & 15) - c_; \
	uint8_t ah = (A >> 4) - (val >> 4) - ((int8_t)(al) < 0); \
	if (!(uint8_t)(diff)) P |= F_Z; \
	else if (diff & 0x80) P |= F_N; \
	if ((A ^ val) & (A ^ diff) & 0x80) P |= F_V; \
	if (!(diff & 0xff00)) P |= F_C; \
	if ((int8_t)(al) < 0) al -= 6; \
	if ((int8_t)(ah) < 0) ah -= 6; \
	A = (ah << 4) | (al & 15); \
} while (0)
#define do_sbc_nd(v) do { \
	uint8_t val = (v); \
	uint16_t diff = A - val - (P & F_C ? 0 : 1); \
	P &= ~(F_N | F_V | F_Z | F_C); \
	if (!(uint8_t)(diff)) P |= F_Z; \
	else if ((int8_t)(diff) < 0) P |= F_N; \
	if ((A ^ val) & (A ^ diff) & 0x80) P |= F_V; \
	if (!(diff & 0xff00)) P |= F_C; \
	A = diff; \
} while (0)
#define do_sbc(v) do { uint8_t v1_ = (v); if (P & F_D) do_sbc_d(v1_); else do_sbc_nd(v1_); } while (0)
#define do_bit(v) do { \
	uint8_t val = (v); \
	P &= ~(F_N | F_Z | F_V); \
	uint8_t r = A & val; \
	if (!r) P |= F_Z; \
	if (val & 0x80) P |= F_N; \
	if (val & 0x40) P |= F_V; \
} while (0)
#define do_asl(v) ({ \
	uint8_t v_ = (v); \
	P &= ~(F_N | F_Z | F_C); \
	uint8_t r = v_ << 1; \
	if (!r) P |= F_Z; \
	else if ((int8_t)(r) < 0) P |= F_N; \
	if (v_ & 0x80) P |= F_C; \
	r; })
#define do_lsr(v) ({ \
	uint8_t v_ = (v); \
	P &= ~(F_N | F_Z | F_C); \
	if (v_ & 1) P |= F_C; \
	v_ >>= 1; \
	if (!v_) P |= F_Z; \
	v_; })
#define do_ror(v) ({ \
	uint8_t v_ = (v); \
	bool c_ = P & F_C; \
	P &= ~(F_N | F_Z | F_C); \
	if (v_ & 1) P |= F_C; \
	v_ >>= 1; \
	if (c_) v_ |= 0x80; \
	if (!v_) P |= F_Z; \
	else if ((int8_t)(v_) < 0) P |= F_N; \
	v_; })
#define do_rol(v) ({ \
	uint8_t v_ = (v); \
	bool c_ = P & F_C; \
	P &= ~(F_N | F_Z | F_C); \
	if (v_ & 0x80) P |= F_C; \
	v_ <<= 1; \
	if (c_) v_ |= 0x01; \
	if (!v_) P |= F_Z; \
	else if ((int8_t)(v_) < 0) P |= F_N; \
	v_; })

M65_HOT int m65_run(int cycles) {
	m65_t *const c = m65p;
	c->cycles_running = cycles;
	c->cycles_stolen = 0;
	int icount = cycles;
	uint16_t PC = c->PC, TMP = c->TMP;
	uint8_t A = c->A, X = c->X, Y = c->Y, P = c->P, TMP2 = c->TMP2, IR = c->IR;
	if (c->resume) {                    // mid-instruction
		const void *r = c->resume;
		c->resume = NULL;
		goto *r;
	}
	if (c->inst_state == STATE_RESET) goto op_reset;
	goto m65_next;
#include "m6502_body.h"
m65_out:
	M65_SAVE();
	return cycles - c->icount - c->cycles_stolen;
}

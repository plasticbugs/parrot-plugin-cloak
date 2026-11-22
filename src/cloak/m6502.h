// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
//
// m6502.h -- the NMOS 6502 as MAME 0.288 emulates it, for Cloak & Dagger's
// two CPUs. One copy of the code runs both: m65p points at the running
// CPU's state, which the board sets before each timeslice. m6502.c has the
// details.
#ifndef CL_M6502_H
#define CL_M6502_H

#include <stdbool.h>
#include <stdint.h>

typedef struct m65_s m65_t;

struct m65_s {
	// the board's: the CPU's address space, a pointer a page less the
	// page's address (map[a >> 8][a] is a's byte), the reads' pages then
	// the writes'; NULL: the page goes through io_rd / io_wr, but for a
	// page 0 there, which is zp's RAM but 0x0008-0x000f (m65_map). First,
	// so a page's pointer is a load from the struct with no offset.
	uint8_t *map[512];
	// MAME's m6502_device
	uint16_t PC, NPC, PPC, SP, TMP;
	uint8_t A, X, Y, P, TMP2, IR;
	union {                             // the lines each instruction's end looks at, in one word
		struct { bool irq_state, apu_irq_state, nmi_pending, inhibit_interrupts; };
		uint32_t lines;
	};
	bool nmi_state, v_state, irq_taken;
	int inst_state;
	const void *resume;                 // mid-instruction: the place to carry on from (else NULL)
	// device_execute_interface's count of this timeslice
	int icount, cycles_running, cycles_stolen;
	uint8_t *zp;                        // page 0's RAM (read_zp, write_zp; a page 0 that's I/O)
	bool zp_io;                         // 0x0008-0x000f of it are io_rd / io_wr's
	uint8_t (*io_rd)(uint16_t adr);
	void (*io_wr)(uint16_t adr, uint8_t val);
	void (*irq_ack)(void);              // standard_irq_callback(IRQ_LINE)
	void (*trace)(void);                // a test's per-instruction hook (MAME's trace)
	int id;
};

extern m65_t *m65p;                     // the CPU running now (the one the calls below act on)

void m65_init(void);                    // power on (MAME's device_start)
// the address space: a pointer a page for reads and for writes (NULL: io_rd / io_wr)
void m65_map(const uint8_t *const rd[256], uint8_t *const wr[256]);
void m65_reset(void);                   // MAME's device_reset()
int m65_run(int cycles);                // run a timeslice: the cycles it ran (MAME's accounting)
static inline void m65_abort(void) {   // MAME's abort_timeslice()
	m65_t *c = m65p;
	if (c->icount > 0) {
		c->cycles_stolen += c->icount;
		c->cycles_running -= c->icount;
		c->icount = 0;
	}
}
// cycles run so far in this timeslice (in an I/O handler: to the access)
static inline int m65_done(void) { return m65p->cycles_running - m65p->icount; }
void m65_set_irq(int state);            // execute_set_input

typedef struct { uint16_t pc, sp; uint8_t a, x, y, p; } m65_regs_t;
void m65_regs_of(const m65_t *c, m65_regs_t *r);

#endif

// clpad.h -- the controls, as Cloak & Dagger's two 8-way sticks (move and
// fire), its igniter button, the coin and the starts. Portable,
// header-only.
//
// A USB pad: the d-pad or the left stick moves; the right stick fires that
// way (past about 40% of its travel), and so do the face buttons (the left
// one fires left, the top one up, and so on, for a pad without a second
// stick); a shoulder button is the igniter; - is a coin, + one player.
//
// The ColecoVision controller (the kernel reads one): the joystick moves;
// held with the left fire button it fires that way instead (Cloak stands
// still to shoot); the right fire button is the igniter. The keypad: * or
// # a coin, 1 one player, 2 two players (the arcade plugins' keys).
//
// Each stick's opposite ways cancel, as MAME's 8-way sticks do. A coin is
// a pulse of CLPAD_COIN lists from the press, however long it is held;
// another needs the key up for CLPAD_COIN_UP lists (pmpad.h's).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../sdk/gamepad.h"
#include "cl.h"

enum { CLKEY_0 = 0xA, CLKEY_1 = 0xD, CLKEY_2 = 0x7, CLKEY_3 = 0xC, CLKEY_STAR = 0x9, CLKEY_HASH = 0x6, CLKEY_NONE = 0xF };

// a stick's ways as the board's P1 nibble has them (bit set = held): down, up, right, left
enum { CLDIR_DOWN = 1, CLDIR_UP = 2, CLDIR_RIGHT = 4, CLDIR_LEFT = 8 };

#define CLPAD_COIN    6
#define CLPAD_COIN_UP 3
#define CLPAD_STICK   820               // 40% of a stick's 2047

typedef struct {
    uint8_t coin_left;                  // lists of the coin pulse still to give the game
    uint8_t coin_wait;                  // lists the coin key must still be up before a press counts
} clpad_t;

static inline unsigned clpad_cancel(unsigned d) {
    if ((d & (CLDIR_UP | CLDIR_DOWN)) == (CLDIR_UP | CLDIR_DOWN)) d &= ~(unsigned)(CLDIR_UP | CLDIR_DOWN);
    if ((d & (CLDIR_LEFT | CLDIR_RIGHT)) == (CLDIR_LEFT | CLDIR_RIGHT)) d &= ~(unsigned)(CLDIR_LEFT | CLDIR_RIGHT);
    return d;
}

// the console's joystick nibble (low = held: bit 0 up, 1 right, 2 down, 3 left)
static inline unsigned clpad_joy_dirs(uint8_t joy) {
    return (!(joy & 1) ? CLDIR_UP : 0) | (!(joy & 2) ? CLDIR_RIGHT : 0) | (!(joy & 4) ? CLDIR_DOWN : 0) |
           (!(joy & 8) ? CLDIR_LEFT : 0);
}

// sdk/cvpad.h's cvpad_dirs() (GP_UP ...)
static inline unsigned clpad_gp_dirs(uint32_t gp) {
    return (gp & GP_UP ? CLDIR_UP : 0) | (gp & GP_DOWN ? CLDIR_DOWN : 0) | (gp & GP_LEFT ? CLDIR_LEFT : 0) |
           (gp & GP_RIGHT ? CLDIR_RIGHT : 0);
}

// a stick (x right, y up, -2048..2047) as 8 ways: none inside CLPAD_STICK,
// else the nearest of the 8 (a way is on within 67.5 degrees of it)
static inline unsigned clpad_stick_dirs(int x, int y) {
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    if (ax < CLPAD_STICK && ay < CLPAD_STICK) return 0;
    unsigned d = 0;
    if (ax * 5 > ay * 2) d |= x > 0 ? CLDIR_RIGHT : CLDIR_LEFT;     // tan 22.5 degrees is about 2 / 5
    if (ay * 5 > ax * 2) d |= y > 0 ? CLDIR_UP : CLDIR_DOWN;
    return d;
}

// One list's inputs. joy, key: the console's controller as cvf_input()
// gives them; fire: its fire buttons (bit 0 left, bit 1 right, low = held).
// pad: a USB pad's state, or NULL; pad_dirs its cvpad_dirs(). Sets the
// board's P1, SYSTEM and START (cl_inputs_t's active levels).
static inline void clpad_frame(clpad_t *s, uint8_t joy, uint8_t fire, uint8_t key, const gamepad_t *pad,
                               uint32_t pad_dirs, cl_inputs_t *in) {
    unsigned move = 0, shoot = 0;
    bool igniter = !(fire & 2), coin = key == CLKEY_STAR || key == CLKEY_HASH;
    bool start1 = key == CLKEY_1, start2 = key == CLKEY_2;
    if (!(fire & 1)) shoot = clpad_joy_dirs(joy);
    else move = clpad_joy_dirs(joy);
    if (pad) {
        uint32_t b = pad->buttons;
        move |= clpad_gp_dirs(pad_dirs);
        shoot |= clpad_stick_dirs(pad->rx, pad->ry);
        shoot |= (b & GP_WEST ? CLDIR_LEFT : 0) | (b & GP_NORTH ? CLDIR_UP : 0) | (b & GP_EAST ? CLDIR_RIGHT : 0) |
                 (b & GP_SOUTH ? CLDIR_DOWN : 0);
        igniter |= (b & (GP_L | GP_R | GP_ZL | GP_ZR)) != 0;
        coin |= (b & GP_MINUS) != 0;
        start1 |= (b & GP_PLUS) != 0;
    }
    move = clpad_cancel(move);
    shoot = clpad_cancel(shoot);
    in->p1 = (uint8_t)~(move << 4 | shoot);
    uint8_t sys = CL_SYSTEM;
    if (igniter) sys &= (uint8_t)~0x80;
    if (coin && !s->coin_wait) s->coin_left = CLPAD_COIN;
    if (coin) s->coin_wait = CLPAD_COIN_UP;
    else if (s->coin_wait) s->coin_wait--;
    if (s->coin_left) {
        sys &= (uint8_t)~0x08;          // COIN1
        s->coin_left--;
    }
    in->system = sys;
    in->start = (uint8_t)(CL_START | (start1 ? 0x80 : 0) | (start2 ? 0x40 : 0));
}

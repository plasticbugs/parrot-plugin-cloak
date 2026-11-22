// clrun: the cart's loop, a list at a time, shared by the plugin and the
// Mac tests so both run the board the same way (pmrun.c's way).
//
// Each list runs the board for the list's time (CLCV_LIST_T of the
// console's T: 16.77 ms, a little more than the board's 16.67 ms frame), to
// a master cycle count kept from the start so nothing drifts; CLCV_PCM_N
// samples of the sound go with the list. The board runs CLRUN_LEAD cycles
// ahead of the samples taken, so a list always finds its samples made.
//
// The picture: the playfield, the sprites and the colors at the last
// VBLANK the run passed, the bitmap shown as it is then (clcv.h). Each run
// a picture makes costs about 1,500 cycles on the M33, and some scenes
// change most of the screen every frame while the board itself takes 70%
// of a list. So the pictures have a credit of runs: CLRUN_CELLS more each
// list, up to CLCV_CELLS, and a picture makes at most what it has (the
// rest wait, clcv's pending rows). A burst (a new room) still goes out at
// once, and the lists keep up on average (test/armcount.sh).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cl.h"
#include "clcv.h"

#define CLRUN_LEAD 850                  // master cycles: 0.85 ms, about 5 samples
#ifndef CLRUN_CELLS
#define CLRUN_CELLS 384                 // the runs a list's picture may make, on average (the user's choice: smoother)
#endif

typedef struct {
    clcv_t *cv;
    uint32_t lists;                     // since the start
    uint64_t target;                    // the master cycle this list runs to
    uint32_t underruns;                 // samples a list had to make up
    uint32_t pictures;
    int32_t credit;                     // the runs the next picture may make
    bool whole;                         // the last picture made every run it wanted
    clcv_mix_t mix;
    int16_t pcm[CLCV_PCM_N];
} clrun_t;

// the board started (cl_init done) and clcv's hooks and sound with it
void clrun_start(clrun_t *r, clcv_t *cv);
// one list: the board for its time, then the list into out; returns its length
size_t clrun_list(clrun_t *r, uint8_t *out);

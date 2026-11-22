// clrun.c -- see clrun.h.
#include "clrun.h"

#include <string.h>

static clrun_t *running;

static CLCV_HOT void on_vblank(void) {
    if (running) clcv_vblank(running->cv);
}

void clrun_start(clrun_t *r, clcv_t *cv) {
    memset(r, 0, sizeof *r);
    r->cv = cv;
    running = r;
    cl_on_vblank = on_vblank;
    cl_audio_rate_q8(CLCV_RATE_Q8);
    r->target = cl_time();
    r->credit = CLCV_CELLS;
}

CLCV_HOT size_t clrun_list(clrun_t *r, uint8_t *out) {
    r->lists++;
    r->target = ((uint64_t)r->lists * CLCV_LIST_CYCLES_Q16 >> 16) + CLRUN_LEAD;
    cl_run_to(r->target);
    r->credit += CLRUN_CELLS;
    if (r->credit > CLCV_CELLS) r->credit = CLCV_CELLS;
    if (r->cv->fresh) {
        uint32_t c0 = r->cv->stats.cells;
        r->cv->cells_max = (uint16_t)(r->credit > 0 ? r->credit : 0);
        r->whole = clcv_picture(r->cv);
        r->credit -= (int32_t)(r->cv->stats.cells - c0);
        r->pictures++;
    }
    int32_t s1[CLCV_PCM_N], s2[CLCV_PCM_N];
    uint16_t cnt[CLCV_PCM_N];
    int n = cl_audio_take(s1, s2, cnt, CLCV_PCM_N);
    if (n < CLCV_PCM_N) r->underruns += (uint32_t)(CLCV_PCM_N - n);
    for (int k = 0; k < n; k++) r->pcm[k] = clcv_mix(&r->mix, s1[k], s2[k], cnt[k]);
    if (n == 0) r->pcm[n++] = 0;
    clcv_sound(r->cv, r->pcm, n);
    return clcv_tv_frame(r->cv, out);
}

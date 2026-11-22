// cloak -- Atari's Cloak & Dagger (1983) on the ColecoVision: the cart runs
// the arcade board (../cl.c, MAME 0.288's emulation: two 6502s, the
// bitmap, the playfield, the sprites and two POKEYs) from the user's ROMs
// on the card and sends the picture and the sound to the console as the
// Game Boy kernel's lists (../clcv.c). RESET goes back to the menu.
//
//   core 1  serves the kernel and a list a TV frame (cvframes.c)
//   core 0  this loop: a list at a time (../clrun.c), the board run for the
//           list's time, the changes to the picture since its last VBLANK
//           and the list's samples, handed to core 1 as soon as it will
//           take it
//
// The console runs the game's lists back to back (their sound is a sample
// stream), and each list runs the board for just the list's time (16.77
// ms), so the game keeps the arcade's speed. A list that isn't ready when
// the console wants it makes the console run the last one again (its sound
// once more) and puts the game a list behind. The note counts those.
//
// The ROMs (MAME's "cloak" set) are read from the card's /CLOAK folder,
// loose or zipped, or from /CLOAK.ZIP, each known by its size and CRC-32.
// The game's NVRAM (its high scores and settings) is /CLOAK/CLOAK.NV, the
// same bytes as MAME's nvram/cloak/nvram, saved on the way back to the menu.
//
// Controls (../clpad.h): the joystick moves, and with the left fire button
// held it fires that way; the right fire button is the igniter; keypad * or
// # a coin, 1 and 2 the starts. A USB pad: the d-pad or the left stick
// moves, the right stick or the face buttons fire, a shoulder button is the
// igniter, - a coin, + one player.
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include "cvdiag.h"
#include "cvframes.h"
#include "cvpad.h"
#include "cvsd.h"
#include "cvxip.h"
#include "vdp.h"
#include "gbkernel.h"
#include "handoff.h"
#include "clcard.h"
#include "clcv.h"
#include "clpad.h"
#include "clrun.h"

#define OPEN_US     20000           // serve the console this long before the card's (blocking) start
#define WATCHDOG_MS 4000            // a hang reboots into the menu
#define NOTE_US     1000000         // the menu's note is brought up to date this often
#define GAME        "Cloak & Dagger"    // in the menu's notes
#define TAG         "CD"

static clcv_t enc;
static clrun_t run;
static clpad_t pad;
static cl_roms_t roms;
static uint8_t list[CLCV_LIST_MAX];
static size_t list_len;
static bool list_pending;                   // built, not published yet
static uint32_t seen_frames;
static fs_t fs;
static bool card_ok, game_on;
static uint32_t nv_crc;                     // the NVRAM as it was read (or made)
static uint8_t *scratch;                    // clcv's room before the game: a message's canvas, the card's reads
static size_t scratch_n;

#define CANVAS      (*(canvas_t *)(void *)scratch)

// ---- the console ---------------------------------------------------------------------
static void tasks(void) {
    cvpad_task();
    cvf_task(time_us_32());
    watchdog_update();
}

static void publish(void) {
    if (list_pending && cvf_publish(list, list_len)) list_pending = false;
}

// TV frames the console has started since the last call (0: none yet, or
// the last list isn't out): for the messages' lists, a TV frame each
static uint32_t new_frames(void) {
    tasks();
    publish();
    if (list_pending) return 0;
    uint32_t f = cvf_frames(), n = f - seen_frames;
    seen_frames = f;
    return n;
}

static void send(void) {
    list_len = clcv_tv_frame(&enc, list);
    list_pending = true;
    publish();
}

// ---- messages ------------------------------------------------------------------------
static void center(int y, const char *s, uint8_t color, int scale) {
    int w = (int)strlen(s) * 8 * scale;
    canvas_text(&CANVAS, w < VDP_W ? (VDP_W - w) / 2 : 0, y, s, color, scale);
}

static void message(const char *head, const char *l1, const char *l2, const char *l3) {
    canvas_init(&CANVAS);
    center(24, "CLOAK &", C_LGREEN, 3);
    center(52, "DAGGER", C_LGREEN, 3);
    if (head) center(92, head, C_WHITE, 1);
    if (l1) center(112, l1, C_GRAY, 1);
    if (l2) center(124, l2, C_GRAY, 1);
    if (l3) center(136, l3, C_GRAY, 1);
    center(176, "RESET: BACK TO THE MENU", C_LBLUE, 1);
    clcv_canvas(&enc, &CANVAS.px[0][0]);
}

// serve the console, a list a frame, for us microseconds (forever: 0)
static void serve_for(uint32_t us) {
    uint32_t t0 = time_us_32();
    while (!us || time_us_32() - t0 < us) {
        if (!new_frames()) continue;
        clcv_sound(&enc, NULL, 0);
        send();
    }
}

static void __attribute__((noreturn)) fail(const char *why, const char *l2, const char *l3, const char *menu_note) {
    message("IT CAN'T START", why, l2, l3);
    cvf_note(menu_note);
    serve_for(0);
    for (;;) tight_loop_contents();
}

// between a file's sectors: the console keeps its picture
static void card_tick(void) {
    if (!new_frames()) return;
    clcv_sound(&enc, NULL, 0);
    send();
}

// on the way back to the menu: the NVRAM, if the game changed it
static void going_back(void) {
    if (!card_ok || !game_on || cl_crc32(0, cl_nvram, sizeof cl_nvram) == nv_crc) return;
    watchdog_update();
    fs_set_writer(&fs, cvsd_write);
    if (clcard_nv_save(&fs)) cvf_note(GAME ": scores saved");
    else cvf_note(GAME ": scores not saved (/CLOAK?)");
    watchdog_update();
}

// ---- the game ------------------------------------------------------------------------
static uint32_t published, frames_at_start, us_max, pics_at_note;
static bool xip_quad;                       // cvxip_fast() worked (else flash misses are ~20x slower)

// one list: the controls, the board for the list's time, the list made,
// then handed over as soon as core 1 takes it
static void game_list(void) {
    uint32_t t0 = time_us_32();
    uint8_t joy, fire, key;
    cvf_input(&joy, &fire, &key);
    gamepad_t g;
    bool have = cvpad_get(0, &g);
    clpad_frame(&pad, joy, fire, key, have ? &g : NULL, have ? cvpad_dirs(&g) : 0, &cl_in);
    list_len = clrun_list(&run, list);
    uint32_t us = time_us_32() - t0;
    if (us > us_max) us_max = us;
    while (!cvf_publish(list, list_len)) tasks();
    published++;
    tasks();
}

// the menu's bottom line, should RESET come: pictures a second, lists the
// console ran again for want of a new one (since the start), the slowest
// list this second, the board's watchdog resets and the bus's counts (and
// " x" if the flash was left in its slow serial reads)
static void update_note(void) {
    static uint32_t t_last;
    uint32_t t = time_us_32();
    if (t - t_last < NOTE_US) return;
    int32_t again = (int32_t)(cvf_frames() - frames_at_start) - (int32_t)(published - 1);
    if (again < 0) again = 0;
    unsigned per_s = MIN(run.pictures - pics_at_note, 99u), ms = MIN(us_max / 1000, 99u), tenths = us_max / 100 % 10;
    uint32_t unsel, early, unhdr;
    cvf_bus_counts(&unsel, &early, &unhdr);
    char n[48];
    snprintf(n, sizeof n, TAG " %u/s %ldL %u.%ums r%u s%u h%u%s", per_s, (long)MIN(again, 9999), ms, tenths,
             (unsigned)MIN(cl_resets, 9u), (unsigned)MIN(unsel, 999u), (unsigned)MIN(unhdr, 999u), xip_quad ? "" : " x");
    cvf_note(n);
    t_last = t;
    pics_at_note = run.pictures;
    us_max = 0;
}

int main(void) {
    static const cvf_config_t cfg = {gbkernel_bin, GBKERNEL_BIN_LEN, handoff_bin, HANDOFF_BIN_LEN};
    xip_quad = cvxip_fast();        // quad flash reads: before core 1 starts (cvxip.h)
    cvf_start(&cfg);
    cvd_start();
    cvd_stage(1);
    cvpad_init();
    clcv_init(&enc);
    scratch = clcv_scratch(&scratch_n);
    message("LOOKING FOR THE ROMS", "IN " CLCARD_HOME " ON THE CARD", NULL, NULL);
    serve_for(OPEN_US);

    if (!cvsd_card_in() || !cvsd_init()) fail(cvsd_card_in() ? "THE CARD DIDN'T ANSWER" : "NO CARD IN THE SLOT", NULL, NULL, GAME ": no card");
    if (fs_mount(&fs, cvsd_read) != FS_OK) fail("NO FAT OR EXFAT ON THE CARD", NULL, NULL, GAME ": the card didn't mount");
    card_ok = true;
    cvd_stage(2);
    watchdog_enable(WATCHDOG_MS, true);
    // the characters and the sprites at the end of the room, the card's reads
    // before them (over the canvas, whose last 768 bytes, its dirty marks,
    // are under the characters: it is done with once its list is made)
    cl_rom_gfx = scratch + scratch_n - CLROMS_GFX_SIZE;
    uint32_t found = clcard_load_roms(&fs, scratch, (uint32_t)scratch_n - CLROMS_GFX_SIZE, card_tick);
    cvd_stage(3);
    if (found != CLROMS_ALL) {
        int missing = 0;
        const char *first = "";
        for (int i = 0; i < CL_ROM_FILES; i++)
            if (!(found & 1u << i) && !missing++) first = cl_rom_files[i].name;
        char l2[40], note[48];
        if (!found) snprintf(l2, sizeof l2, "NONE OF ITS %d FILES IS THERE", CL_ROM_FILES);
        else snprintf(l2, sizeof l2, "%d OF %d MISSING: %.14s", missing, CL_ROM_FILES, first);
        for (char *c = l2; *c; c++)
            if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
        snprintf(note, sizeof note, GAME ": %d of %d ROMs missing", missing, CL_ROM_FILES);
        fail("PUT MAME'S CLOAK ROMS, OR", "CLOAK.ZIP, IN " CLCARD_HOME, l2, note);
    }
    memset(cl_nvram, 0, sizeof cl_nvram);           // MAME's start without a file (DEFAULT_ALL_0)
    clcard_nv_load(&fs);
    nv_crc = cl_crc32(0, cl_nvram, sizeof cl_nvram);
    clroms_roms(&roms);
    cl_init(&roms);
    clrun_start(&run, &enc);
    game_on = true;
    cvf_on_exit(going_back);
    cvd_stage(4);
    cvf_note(GAME);

    while (list_pending) tasks(), publish();          // the last message's list out first
    frames_at_start = cvf_frames();
    for (;;) {
        game_list();
        update_note();
    }
}

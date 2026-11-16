/* frontend.h - THE MENUS: the original's screens, from its own tables and art.
 *
 * A REPLACE layer: the screen
 * bodies are PUSHAL-woven code reached through tables, and are not
 * transcribed.  What IS the original's, read at run time and never checked in
 *
 *
 *   the screen tables      0x006FABF4.. in the user's RAL.EXE: every label,
 *                          item, position, font, neighbour and handler address
 *   the font records       0x00517ECC.. (their static fields), and which sheet
 *                          and LUT each takes - parsed out of FUN_004150C9's
 *                          own MOV run, 0x004151C1..
 *   the art                files\g\graphics\screens\*.png, sprites\mbuts0?.pcx,
 *                          d\fnt\fnt*.pcx + pal_00.pcx, cut by gfx2d/sheet.c
 *   the text               files\locale\ENGLISH.TXT (lexicon.h)
 *   the stage/car tables   the itinerary 0x006E7E85.., the stage sets
 *                          0x006E7539.., the car lists 0x0070C06A..
 *
 * THE DRAW IS TRANSCRIBED: the frame order of 0x0047133B, the item drawer
 * 0x0046E37D, the button capsules 0x004654CF / 0x004657EC / 0x00465B0B (with
 * the first one's two fall-through bugs, reproduced), the carousel caption
 * 0x004661F4, the highlight blend 0x00405BD9 (textflashmode 3), the label
 * list 0x0046E563. sections 12-14.
 *
 * THE EFFECTS ARE THE ORIGINAL'S, AT THE SAME ADDRESSES' MEANING: the item
 * handlers are dispatched by their ADDRESS in the table, each to a port
 * function that writes what the original's writes.  A handler with no port
 * function is UNPORTED and says so once on stdout; the screen stays.
 *
 * THE 3D MODELS: the carousels, car select's cars and REVIEW
 * OPTIONS' arrow, drawn by fe_draw_3d (menu3d.h) - the placement is the
 * original's code run in the interpreter, the model path is objlit.h.
 * NOT DRAWN, AND NAMED: SELECT STAGE's information panel (the rest of 0x0047DB0E: times,
 * length, surface, weather).  The service area's VIEW STAGE (LOAD /
 * SAVE SETTINGS are the, the original's code run).  Every screen off the
 * paths below.
 * has the rows.
 *
 * THE PATHS.  Single (gmode 0): MAIN MENU -> START RALLY -> SELECT NUMBER OF
 * PLAYERS -> car select -> SELECT STAGE -> the race.  Time trial (gmode 3): the same through car select, then SELECT STAGE -> the SERVICE AREA
 * (the car setup, setup.h) -> the race.  After either race: RESULTS ->
 * REVIEW OPTIONS -> SELECT STAGE.  The arcade (gmode 2): START
 * RALLY's 4th slot -> SELECT LEVEL -> car select -> six rounds of NEXT STAGE
 * -> the race -> RESULTS (-> BEST TIMES) -> REVIEW OPTIONS, then TOP SCORES,
 * the bonus car or the lost RESULTS, and the next level or a restart - the
 * flow FUN_00483E20 as screen transitions, its helpers and draws the
 * original's code in the interpreter.  The race's arcade rules are not ported
 * fe_after_race writes time left 0 and position 1 for a finish,
 * STAND-INS.  The Mobil 1 championship (gmode 1): START RALLY's 2nd slot -> NEW SIMULATION -> car select -> the
 * Mobil 1 picture -> WELCOME -> per stage LEG DETAILS -> NEXT STAGE -> (after
 * a service halt) the SERVICE AREA -> the race -> CLASS / OVERALL STAGE
 * RESULTS -> the rally's standings -> REVIEW OPTIONS; a rally's end CLASS /
 * OVERALL CHAMPIONSHIP; RETIRE on ESC - FUN_0049CB25 as transitions, its
 * helpers (the standings too) the original's code;.
 *
 * THE SERVICE AREA, RESULTS AND THE PLAYER RECORD run the original's
 * own DRAW CODE in a small x86 interpreter (x86vm.h) over a copy of the image's
 * data - the screens are hundreds of constant-position draw calls, and reading
 * them out of the user's RAL.EXE at run time is how the port avoids copying
 * their layout by hand.  The screens' LOGIC is ported natively (setup.h) and
 * is checked against the original's code by `setup_check`.
*/
#ifndef RALLY_GAME_FRONTEND_H
#define RALLY_GAME_FRONTEND_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "gfx2d.h"
#include "wear.h"

/* The keys, edge-triggered, as FUN_00462C28 dispatches them. */
enum {
    FE_UP = 1, FE_DOWN = 2, FE_LEFT = 4, FE_RIGHT = 8,
    FE_ENTER = 16, FE_ESC = 32
};

enum { FE_RUN = 0, FE_LAUNCH = 1, FE_QUIT = 2, FE_VIEW = 3 /* play fe_view_path */ };

/* What a launch hands the race - the fields graham_to_shaun copies, each from
 * the menu-side global the original writes. */
typedef struct {
    int32_t gmode;          /* [0x70CC40] - FUN_00480461's EBX, 0 for single  */
    int32_t track;          /* [0x70CC3C] shauntracknum = stage record +0x34  */
    int32_t direction;      /* [0x70CC58] shauncrsdirec = [0x708230]          */
    int32_t carnum;         /* player+0x08 -> shauncarnum1                    */
    int32_t gearbox;        /* player+0x30 -> [0x70CD98] -> car+0x38 autom    */
    int32_t rally, stage;   /* [0x708238] 0..5, [0x708234] 1..6               */
    /* the arcade's start position, the last stage's arcadepo
     * [0x70CF6C] (graham_to_shaun 0x00445A91); the player's race number
     * +0x28 -> [0x70CD88] (0x00480CFC); player 2's gearbox [0x70CF04] ->
     * [0x5D6748] (0x00445407), which FUN_00435148 gives car slot 1 */
    int32_t arcadepo, racenum, gearbox2;
    char    level[16];      /* "LEVELnn" from `track`                         */
    /* the car's setup as the player record holds it at the launch -
     * player+0x10C..0x120, in menu units (setup.h scales them into the car) */
    int32_t has_setup;
    int32_t tyre, bbal, seth, sets, stes, setg;
    /* the player record's twelve key bytes, +0x1C..+0x27 - the
     * launcher's byte copy into 0x70CDE8.. (0x00480DA7), which
     * INITCONTROLDATA turns into the scheme's bindings (car_keys.h) */
    unsigned char keys[12];
    /* its eleven joystick mapping dwords, +0x50..+0x78 - the
     * launcher's copy into 0x70CDB8.. (0x00480D4F; car_keys.h) */
    uint32_t joymap[11];
    /* PLAYER DETAILS' co-driver, player+0x154 -> [0x70CD8C]
     * (0x00480D04) -> the player record's +0x298 (0x004158CB): 0 male */
    int32_t codriver;
    /* the device's dead zone as the original configures it -
     * joylowerdz from SETUP CONTROLLER's lower dz (player +0x44), joyupperdz
     * always 0: the upper dz slider (+0x40) never reaches the device */
    int32_t lowerdz, upperdz;
    /* AUDIO SETUP's in-game music switch [0x0070CC68], 1 on */
    int32_t race_music_on;
    /* VISUAL OPTIONS' lens flare, [0x707FB8] (the row's arrow,
     * 0x46CC8C, toggles it; 0x46CC98 copies it to [0x70CC6C], and so does the
     * launcher, 0x004804E5/EA); graham_to_shaun hands it on to [0x5D675C]
     * (0x0044505B).  var\ral.dat saves it (0x707FA8 + 0x10).  1 on */
    int32_t lens_flare;
    /* the other four detail rows, the launcher's copies
     * (0x004804EF..0x00480513: [0x707FC4] -> [0x70CC78], [0x707FC8] ->
     * [0x70CC7C], [0x707FCC] -> [0x70CC80], [0x707FD0] -> [0x70CC84]) and
     * graham_to_shaun's hand-on to [0x5D6760..0x5D676C] (0x00445069..0x0044508D):
     * reflections 0/1 (image default 1), car shadows 0/1 (1), car detail 0..2
     * (1), headlights 0/1 (0 - the cars' lamps only light the ground for the
     * players' own cars).  var/ral.dat saves all four (0x707FA8 + 0x1C..0x28) */
    int32_t reflections, car_shadows, car_detail, headlights;
    /* car+0x220, the HUD mask graham_to_shaun builds from VISUAL
     * PREFERENCES' nine flags (player +0x158..+0x178, the launcher's copy
     * 0x00480B02..), or player +0x284 whole when it is not -1 */
    uint32_t hud_mask;
    /* [0x70CC44] -> [0x5D6774], RETIRE?'s third answer: the launcher
     * (0x00480E07..0x00480E3E) sets 1 unless [0x7082F4] is 4, 5, 10 or 15
     * or [0x7082F0] is 1 */
    int32_t retire_all;
    /* player +0x278 -> [0x70CF80] -> car +0x298 carpartsleft
     * (0x00480A5D, 0x00445A79), and player +0x288 -> [0x70CF70] -> car
     * +0x88 catitken in a championship (0x004835C1, 0x00434F1F) */
    int32_t carparts, champ88;
    /* the cheat word [0x0070AA78] (bit 16: no out of time) */
    uint32_t cheats;
    /* [0x70CC38] -> [0x5D6770] (graham_to_shaun 0x00444FAD), the
     * championship's 90 rivals - FUN_0046F785 points it at the opponents'
     * block [0x70805C] (0x0046F85A).  have_ghosts 0: the pointer was 0 (the
     * race keeps FUN_0043560F's defaults).  Each 0x50-byte record's +0x00
     * name id, +0x08 car type, +0x0C stage total in centiseconds. */
    int32_t have_ghosts;
    struct { int32_t name, type, total; } ghosts[90];
    /* the car's wear as the launcher copies it out of the
     * record (wear.h); have_wear 0 without the
     * menus' interpreter */
    int32_t    have_wear;
    wear_block wear;
    /* PLAYER 1 DETAILS' camera +0x0C -> [0x70CD90] (0x00480D0F) ->
     * the view the race starts in, [0x6C2794] (0x00445429); SETUP
     * CONTROLLER's device +0x2C, axis mode +0x34 and shifter +0x3C ->
     * [0x70CD94] / [0x70CD9C] / [0x70CDA4] (0x00480D17..), which 0x00408A1B
     * hands the scheme (axis mode = the jstype [esi+8], 0 -> 2; shifter ->
     * [esi+0x414]); and +0x280, the steering sensitivity: its one reader
     * that reaches a race is the new career's copy into the slider +0x11C
     * (0x0046F2D3), which `stes` above already is */
    int32_t camera, device, axismode, shifter, sens;
} fe_launch;

typedef struct fe fe;

/* Loads everything; NULL with `err` filled on failure.  `assets` is the
 * install root, `image` its RAL.EXE. */
fe  *fe_open(const char *assets, const char *image, char *err, size_t n);
void fe_close(fe *f);

/* print, for every address that can reach the interpreter, whether a C twin answers it (frontend.c; `list` is
 * tools/vm_census.py's output, may be NULL).  Returns the count with none, -1 on an error. */
int fe_vm_census(fe *f, const char *list, FILE *out);

/* One frame: the keys pressed since the last call, the clock in ms
 * (GetTickCount's role, [0x0051272C]).  Draws into the canvas. */
int  fe_frame(fe *f, unsigned keys, uint32_t now_ms);

/* the screen's 3D models (menu3d.h), drawn through plat.h OVER the
 * canvas once it is on screen - the 0x0047133B frame's draw list, which comes
 * after the labels, items and pre-draw list and before the post-draw list.
 * `win_w` x `win_h` is the window.  fe_has_3d: this screen draws any. */
void fe_draw_3d(fe *f, int win_w, int win_h);
int  fe_has_3d(const fe *f);
/* One line per model the last fe_draw_3d drew: type, model, the placement
 * the original's code set, and what the lit path did with it (menu3d.h). */
void fe_report_3d(const fe *f, FILE *out);

/* The 800x600 canvas, 0xAARRGGBB, alpha 0xFF. */
const gfx2d_surface *fe_canvas(const fe *f);

/* the post-draw layer - what the screen draws after its 3D models
 * (the original's post-draw list), alpha 0 elsewhere; NULL when there is
 * none.  It is on the canvas too: present it over fe_draw_3d's models. */
const gfx2d_surface *fe_post(fe *f);

/* where "var\" and "savedata\" are - the user's saves (NULL = the
 * working directory, as the original's) - and whether the port writes there.
 * A read that finds no file there falls back to the install's own copy; a
 * write never goes to the install.  Call before fe_load_saves. */
void fe_set_user(fe *f, const char *dir, int writes);

/* the boot's readers (0x00411013: var\ral.dat, var\hst.dat, the run
 * counter, the default names) and the main menu's entry (0x00478937: the
 * best-times tables allocated, and SEEDED when no hst.dat was read), all the
 * original's code.  `read` 0: as if neither file existed.  1 if a ral.dat
 * was read.  Call before the first frame.  The original's writer (0x0041123E)
 * then runs where it does: when the menus are left by their own QUIT
 * (0x00414FDA - not when the window is closed: the quit flag skips it) and
 * when an arcade level is won (0x00486DA9). */
int  fe_load_saves(fe *f, int read);

/* a TEST HOOK (rally --unlock-a8): [0x0070CC34] = 1, as winning the
 * Mobil 1 championship (0x0049D1A7), cheat bits 0x11 / 0x12 (0x0047CD28) or a
 * ral.dat that has it set do - START RALLY's 3rd slot opens.  Call after
 * fe_load_saves. */
void fe_unlock_a8(fe *f);

/* the sound requests the menus make (sound.h), NULL for none.  Call
 * after fe_load_saves: it reads the volumes the file set, and makes the
 * boot's requests - the frontend sound mode and the main menu's track 4. */
struct snd;
void fe_set_sound(fe *f, struct snd *s);

/* SAVE SETTINGS types a slot's name (FUN_0046BE55).  fe_typing: a
 * name is being typed now; fe_type queues one key as its DirectInput scan
 * code (DIK_A = 0x1E ...; the original's own table 0x00407B8A makes the
 * character), with SHIFT held or not.  RETURN and ESC still come through
 * fe_frame's keys. */
int  fe_typing(const fe *f);
void fe_type(fe *f, int dik, int shift);

/* REDEFINE KEYBOARD (and program controller) wait for ANY key, which
 * the screen's own code takes from the key mailbox 0x00407CDC.  While
 * fe_awaits_key, queue each key pressed with fe_type as its FOLDED code (the
 * numbering 0x00407809 makes, car_keys.h: DIK for the main block, 0x60+ for
 * the extended keys); fe_frame's arrows, RETURN and ESC stand in for a
 * script. */
int  fe_awaits_key(const fe *f);

/* The port's, not the original's: when on, the main menu's quit (its EXIT
 * item, or back) asks first, on the original's RETIRE screen titled EXIT
 * GAME - yes quits, no or back stays.  Off
 * by default, so scripted runs stay the original's; the interactive loop
 * turns it on unless --no-quit-confirm. */
void fe_set_quit_confirm(fe *f, int on);

/* the finish scene.  The three finish routines run whole in the
 * interpreter; when one reaches its scene (0x004264D7) this is called with
 * [0x0070D068] (which of LEVEL01-06), [0x0070D070] (the car), [0x0070D06C] (its
 * race number) and [0x0070D074] (the fireworks: the championship won), and
 * returns when the scene is over; the routine's own tail then stops the music
 * and plays track 3.  NULL: the scene is left out, as before. */
typedef void (*fe_scene_fn)(void *ud, long scene, long car, long racenum,
                            int fireworks);
void fe_set_scene_cb(fe *f, fe_scene_fn fn, void *ud);

/* After FE_LAUNCH: what was chosen. */
void fe_launch_get(const fe *f, fe_launch *out);

/* What the race hands back, as the launcher copies it into the player record
 * (0x00481518..): the finish time in hundredths of a second, the top speed
 * x 10. */
typedef struct {
    int32_t finished;       /* the car finished (car.gof == 2 or past the line) */
    int32_t time_cs;        /* car+0x088, penalties included                    */
    int32_t speed_x10;      /* [0x70CF90] as an integer, x 10                   */
    int32_t time_left_cs;   /* car+0x08C catileft -> [0x70CF68] (0x00447A4C),
                             * the arcade's time left; -1: none known  */
    int32_t arcadepo;       /* car+0x654 -> [0x70CF6C] (0x00447A56), the
                             * streamer's position frozen at the finish
                             * -1: none known                         */
    /* what shaun_to_graham hands the launcher's return
     * (0x0048120F..): the wear block (the launch's, with the car's fields
     * over it), car+0x298 -> [0x70CF80] -> P+0x278, car+0x220 ->
     * [0x70CF78] -> P+0x284.  have_wear 0: none (a command-line race) */
    int32_t    have_wear;
    wear_block wear;
    int32_t    carparts;
    uint32_t   hud_mask;
    /* the race was recorded (var\tmprpl is whole): [0x00574548], which
     * fmt_replay_stop_record sets and REVIEW OPTIONS' SAVE VIDEO REPLAY reads */
    int32_t    replay;
} fe_result;

/* After FE_VIEW: the replay file to play (var\tmprpl from REVIEW OPTIONS,
 * a slot from OPTIONS).  fe_after_view puts the menus back where it was chosen. */
const char *fe_view_path(const fe *f);
void fe_after_view(fe *f, uint32_t now_ms);

/* After the race: RESULTS, then REVIEW OPTIONS, then back to SELECT STAGE. */
void fe_after_race(fe *f, uint32_t now_ms, const fe_result *r);

/* For a check: the screen's name, the cursor. */
const char *fe_screen_name(const fe *f);

/* the original's instructions interpreted so far (x86vm.c) - the
 * menus' cost, read by rally.c's once-a-second rate line. */
uint64_t fe_vm_steps(const fe *f);

#endif

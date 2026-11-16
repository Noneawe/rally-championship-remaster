/* frontend.c - the menus.  See frontend.h.
 *
 * Every address below is the original's;
 * sections 12-14 has the reading behind each, instruction by instruction. */
#include "frontend.h"
#include "car_input.h"
#include "fe_champ.h"
#include "fe_logic.h"
#include "fd_service.h"
#include "fd_stage.h"
#include "fd_career.h"
#include "fd_engine.h"
#include "fd_standings.h"
#include "fd_svcdraw.h"
#include "fd_cspecs.h"
#include "image.h"
#include "lexicon.h"
#include "menu3d.h"
#include "pcx.h"
#include "plat.h"
#include "png.h"
#include "rplrec.h"
#include "setup.h"
#include "sheet.h"
#include "sound.h"
#include "vfs.h"
#include "x86vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the shipped game has NO interpreter - x86vm.c is linked into the gates only.  Every address that can reach the
 * original's code has a C twin or answers nothing (ctest vm-census), so these three are the way out of a path that cannot
 * be taken: orig_call says so and fails.  A debugging build (cmake -DRALLY_INTERP=ON, the pixel comparison's and
 * RALLY_NO_TWIN's) keeps the interpreter and they are the interpreter's own. */
#ifdef RALLY_INTERP
#define orig_call  vm_call
#define orig_error vm_error
#define orig_regs  vm_regs
#else
static int orig_call(vm *v, uint32_t va, const uint32_t regs[8])
{
    (void)v; (void)regs;
    fprintf(stderr, "frontend: 0x%08X has no C twin, and this build has no interpreter\n", va);
    return -1;
}
static const char *orig_error(const vm *v) { (void)v; return "no interpreter in this build"; }
static void orig_regs(const vm *v, uint32_t out[8]) { (void)v; memset(out, 0, 8 * sizeof out[0]); }
#endif

#define W 800
#define H 600

/* ---- the original's addresses ------------------------------------------- */

enum {
    /* the five screens of the single path, their tables */
    T_MAIN    = 0x006FB62C,     /* frnt_mainscr_470ee2_q                    */
    T_SRAL    = 0x00700C54,     /* START RALLY, body 0x00471F34             */
    T_PLAYERS = 0x006FB3FC,     /* SELECT NUMBER OF PLAYERS, 0x0047D807     */
    T_CAR     = 0x006FE28C,     /* car select, frnt_cspecs_476f13_q         */
    T_STAGE   = 0x006FAF94,     /* SELECT STAGE, FUN_0047e12f               */
    /* time trial's path, and what follows a race */
    T_TTPLAYERS = 0x006FB1CC,   /* SELECT NUMBER OF PLAYERS, time trial's   */
    T_TTSTAGE   = 0x006FB0B0,   /* SELECT STAGE, FUN_00487fad               */
    T_SETUP     = 0x006FFBC0,   /* PLAYER n SERVICE AREA, FUN_0046eea9      */
    T_TTBEST    = 0x0070094C,   /* BEST TIMES, frnt_btimes_488b70_q */
    S_ART_TTBEST = 0x006E4A7F,  /* 0x00488BF7                               */
    T_RESULTS   = 0x00703440,   /* RESULTS, frnt_results1_481592_q          */
    T_REVIEW    = 0x00703698,   /* REVIEW OPTIONS, frnt_roptions_481f63_q   */
    S_ART_RESULTS = 0x006E54A2, /* 0x00481623                               */
    S_ART_REVIEW  = 0x006E5399, /* 0x0048205E                               */
    A_SERVART     = 0x0070C52E, /* [car*4]: the service area's picture      */
    /* the arcade, START RALLY's 4th slot (0x0048398D) and the flow
     * FUN_00483E20; */
    T_ARCLEVEL   = 0x006FB800,  /* SELECT LEVEL, body 0x004839A2            */
    T_ARCNEXT    = 0x006FFA30,  /* NEXT STAGE, FUN_004857FD                 */
    T_ARCRES     = 0x006FFAF8,  /* RESULTS, frnt_results1_4851e3_q          */
    T_ARCOVER    = 0x006FFA94,  /* the untitled RESULTS, 0x00484F40         */
    T_ARCBEST    = 0x007008E8,  /* BEST TIMES, frnt_btimes_4863e5_q         */
    T_ARCTOP     = 0x007009B0,  /* TOP SCORES, frnt_bascores_48668f_q       */
    T_ARCCONT    = 0x0070386C,  /* REVIEW OPTIONS: continue, 0x0048494C     */
    T_ARCNEXTLV  = 0x00703A40,  /* REVIEW OPTIONS: start next level, 0x004842D6 */
    T_ARCRESTART = 0x00703C14,  /* REVIEW OPTIONS: restart level, 0x00484611 */
    T_ARCCAR     = 0x006FE34C,  /* the bonus car, frnt_cspecs_477253_q      */
    S_ART_ARCLEVEL = 0x006E5643, /* 0x00483A74: Selarc.png                  */
    S_ART_ARCRES   = 0x006E5667, /* 0x00485274                              */
    S_ART_ARCOVER  = 0x006E547C, /* 0x00484FD1                              */
    S_ART_ARCBEST  = 0x006E4A5B, /* 0x0048646C                              */
    S_ART_ARCTOP   = 0x006E4AA3, /* 0x00486716                              */
    S_ART_ARCCONT  = 0x006E53BF, /* 0x00484A47                              */
    S_ART_ARCNEXTLV = 0x006E540A, /* 0x004843DB                             */
    S_ART_ARCRESTART = 0x006E5430, /* 0x00484716                            */
    S_ART_ARCCAR   = 0x006E49A5, /* 0x0047735A                              */
    A_ARCITIN     = 0x006E7ECD, /* [level*4] + stage*0xA4: 0x00486DE1       */
    A_ARCCARS     = 0x0070C0D2, /* the arcade's car list, 0x00483EA7        */
    A_ALLCARS     = 0x0070C432, /* 0x0047CDD5: with cheat bit 1             */
    A_BONUSCARS   = 0x0070C19E, /* 0x00486D94: one per level won            */
    A_CHEATS      = 0x0070AA78, /* the cheat word: BTS at 0x0047CD93        */
    A_ARCLEVEL    = 0x0070AB54, /* the level, 0..5                          */
    A_ARCSTAGE    = 0x0070AB58, /* the stage, 1..6                          */
    A_ARCFAIL     = 0x0070AB5C, /* the level is lost                        */
    A_ARCWON      = 0x0070AB68, /* [level]: byte 1 once the level is won    */
    A_ARCPOS      = 0x0070AAD0, /* the race's arcadepo, via [0x70CF6C]      */
    A_NEWBEST     = 0x00708280, /* 0x00484274 / 0x00484244: a new record    */
    CAP_KIND5     = 0x0070ADEA, /* 0x0046628D: "arcade 1".. "return to previous" */
    H_ARCADE      = 0x0048398D, /* START RALLY 4: arcade                    */
    H_ARCLV1      = 0x00483C9E, /* SELECT LEVEL 1; 2..6 every 0x47 after 0x483CBD */
    H_ARCAGAIN    = 0x0046E341, /* start next level / restart level         */
    /* the Mobil 1 championship, START RALLY's 2nd slot (0x004727FE)
     * and the flow FUN_0049CB25; */
    T_CHNEW      = 0x006FBE3C,  /* NEW SIMULATION, body 0x004727FE          */
    T_CHWELCOME  = 0x00703034,  /* WELCOME, FUN_0046E84F                    */
    T_CHLEG      = 0x00700478,  /* LEG DETAILS, frnt_ukmap_46ebf5_q         */
    T_CHNEXT     = 0x007003B0,  /* NEXT STAGE, FUN_00470180                 */
    T_CHCLASS    = 0x00702454,  /* CLASS STAGE RESULTS, 0x00462F22          */
    T_CHOVER     = 0x00702514,  /* OVERALL STAGE RESULTS, 0x004631EB        */
    T_CHRCLASS   = 0x007025D4,  /* the rally's class standings, 0x004634B4  */
    T_CHRALLY    = 0x00702694,  /* the rally's standings, 0x0046377D        */
    T_CHCCLASS   = 0x00702880,  /* CLASS CHAMPIONSHIP, 0x004A07E0           */
    T_CHCHAMP    = 0x00702940,  /* OVERALL CHAMPIONSHIP, 0x00463D3B         */
    T_CHBEST     = 0x00700884,  /* BEST TIMES, 0x0049FB6E                   */
    T_CHBESTCH   = 0x00700A14,  /* BEST CHAMPIONSHIP SCORES, 0x004A4444     */
    T_CHBESTOV   = 0x00700A78,  /* BEST OVERALL SCORES, 0x004A46EE          */
    T_CHREVIEW   = 0x00702A00,  /* REVIEW OPTIONS after a service, 0x00479668 */
    T_CHREVIEW2  = 0x007075FC,  /* REVIEW OPTIONS between stages, 0x0047D124 */
    T_CHREVIEWR  = 0x00702CE8,  /* REVIEW OPTIONS at a rally's end, 0x0047CDDC */
    T_CHREVIEWF  = 0x00707888,  /* REVIEW OPTIONS at the end, 0x0047D4BF    */
    T_RETIRE     = 0x00706E48,  /* RETIRE ("are you sure?"), 0x0047BB57     */
    T_CHINSPECT  = 0x00702FD0,  /* the INSPECTION AREA, 0x00479CB6   */
    T_VIEWSTAGE  = 0x00700414,  /* VIEW STAGE, FUN_0047BDFA          */
    T_TTVIEWSTAGE = 0x00707598, /* time trial's, FUN_0048AEB9: one item, no arrows */
    H_VIEWSTAGE  = 0x0046CFE3,  /* the service area's item 22               */
    /* the service area's LOAD / SAVE SETTINGS, the carset?? slots */
    T_SULOAD     = 0x00704758,  /* 0x0046B545, 30 slots a page              */
    T_SUSAVE     = 0x007052E0,  /* 0x0046B826                               */
    H_SULOAD     = 0x0046B545,  /* the service area's item 20               */
    H_SUSAVE     = 0x0046B826,  /* item 21                                  */
    H_SUPAGE     = 0x0046BE3D,  /* the slot screens' page cycle, [0x707BB8] */
    A_SUPAGE     = 0x00707BB8,
    A_SUNAMING   = 0x00708010,  /* 1: a slot's name is being typed          */
    /* LOAD CHAMPIONSHIP and save rally, the svgame?? slots */
    T_CHLOAD     = 0x00703EA8,  /* 0x0047ADF5, ten slots a page             */
    T_CHSAVE     = 0x00704300,  /* 0x0047B0F7                               */
    S_ART_LOADSV = 0x006E48F1,  /* 0x0047AEA3: loadsv.png                   */
    S_ART_SAVESV = 0x006E4915,  /* 0x0047B1A5                               */
    /* SAVE VIDEO REPLAY's slot screen, 0x0047B83E (0x0046C3E8 with EAX 1) */
    T_RPLSAVE    = 0x00705E68,  /* twenty slots a page, five pages          */
    S_ART_RPLSAVE = 0x006E4939, /* 0x0047B8E0                               */
    H_RPLSAVE    = 0x0046D194,  /* REVIEW OPTIONS' save video replay        */
    /* VIEW VIDEO REPLAY's slot screen, 0x0047B54E (0x0046C3E8 with EAX 0) */
    T_RPLLOAD    = 0x00706658,  /* the save screen's twin                   */
    S_ART_RPLLOAD = 0x006E495D, /* 0x0047B5FC                               */
    H_RPLVIEW    = 0x0046D1A3,  /* REVIEW OPTIONS' view video replay        */
    H_RPLOPT     = 0x0046D18E,  /* OPTIONS' video replay                    */
    H_CHLOAD     = 0x0047ADF5,  /* the main menu's item 2                   */
    H_CHSAVE     = 0x0047B0F7,  /* REVIEW OPTIONS' save rally               */
    S_ART_CHNEW    = 0x006E4E3B, /* 0x004728E5: sral.png                     */
    S_ART_CHSPLASH = 0x006E4671, /* 0x0047B419: mob_welc.png                 */
    S_ART_CHLEG    = 0x006E4EC7, /* 0x0046EC86: ukmap.png                    */
    S_ART_CHCLASS  = 0x006E4F56, /* 0x00462FC8                               */
    S_ART_CHOVER   = 0x006E4F7C, /* 0x00463291                               */
    S_ART_CHRCLASS = 0x006E4FA2, /* 0x0046355A                               */
    S_ART_CHRALLY  = 0x006E4FC8, /* 0x00463823                               */
    S_ART_CHCCLASS = 0x006E532A, /* 0x004A0867                               */
    S_ART_CHCHAMP  = 0x006E534F, /* 0x00463DC2                               */
    S_ART_CHBEST   = 0x006E4A37, /* 0x0049FC06                               */
    S_ART_CHBESTCH = 0x006E4AC9, /* 0x004A44CB                               */
    S_ART_CHBESTOV = 0x006E4AEF, /* 0x004A4775                               */
    S_ART_CHREVIEW = 0x006E5085, /* 0x0047975E                               */
    S_ART_CHREVIEW2 = 0x006E50D1, /* 0x0047D21A                              */
    S_ART_CHREVIEWR = 0x006E50AB, /* 0x0047CED2                              */
    S_ART_CHREVIEWF = 0x006E50F7, /* 0x0047D5B5                              */
    S_ART_RETIRE   = 0x006E464C, /* 0x0047BBE8                               */
    S_ART_CHINSPECT = 0x006E5060, /* 0x00479D3D: service.png, no frame       */
    A_CHWELART    = 0x0070BBA2, /* [rally*4]: WELCOME's picture, 0x004650FC  */
    A_CHRALLYWAV  = 0x0070C326, /* [rally*4]: the rally's narration, 0x0047B4E6 */
    S_CHRALLY0    = 0x005045CA, /* 0x0047B3E5: rally0.wav                    */
    A_CHCARS      = 0x0070BFBA, /* the championship's car list, 0x0049E2E8  */
    A_CHITIN      = 0x006E7EE9, /* the itinerary, 0xA4-byte entries          */
    A_CHRALLY     = 0x007082C4, /* the rally, 0..5                           */
    A_CHSTAGE     = 0x007082C0, /* the itinerary entry within it             */
    A_CHRETIRE    = 0x00707BC0, /* RETIRE's yes (0x0046C6A9 toggles it)      */
    A_HEAP        = 0x00C00000, /* the interpreter's heap (x86vm.c),  */
    A_HEAP_END    = 0x00E00000, /* what VirtualAlloc hands out               */
    H_CHNEW       = 0x004727FE, /* START RALLY 2: the Mobil 1 championship   */
    H_CH_1P       = 0x0049E278, /* NEW SIMULATION 1: one player              */
    H_RETIRE_TOGGLE = 0x0046C6A9, /* RETIRE's LEFT / RIGHT                   */
    H_CH_RALLYLB  = 0x004A50BC, /* REVIEW OPTIONS: rally leaderboard         */
    H_CH_CHAMPLB  = 0x004A5153, /* REVIEW OPTIONS: championship leaders      */

    /* the art each body passes FUN_0047B0C2 (LEA EAX,[s] just before) */
    S_ART_MAIN    = 0x006E46E2, /* 0x00470F97 */
    S_ART_SRAL    = 0x006E4707, /* 0x00472011 */
    S_ART_PLAYERS = 0x006E5374, /* 0x0047D8E4 */
    S_ART_CAR     = 0x006E4981, /* 0x00477030 */
    S_ART_UKMAP   = 0x006E4BF3, /* 0x00465084, FUN_004115E1's first image   */
    A_STAGEMAPS   = 0x0070BBD2, /* 0x0046507D: [idx*4 + 0x70BBD2], the map  */

    S_MBUTS       = 0x006E56FB, /* 0x004787B4: "...sprites\mbuts00.pcx"     */
    FONT_BUILD    = 0x00415121, /* FUN_004150C9: PAL_00, then the 4 sheets  */
    FONT_RECS     = 0x004151C1, /* the MOV run that wires records to them   */
    FONT_RECS_END = 0x004157B9, /* its POPAD                                */

    /* the carousel caption lists (0x004661F4, by [0x70C9E0]) */
    CAP_KIND0 = 0x0070AD7A, CAP_KIND1 = 0x0070AD92, CAP_KIND2 = 0x0070ADAE,

    /* handlers, dispatched by address */
    H_BACK        = 0x0046E300, /* [0x707ECD] = 1: leave the screen         */
    H_TTPLAYERS   = 0x00487CBE, /* START RALLY 5: time trial                */
    H_TT_1P       = 0x00486E1B, /* time trial's players 1: the flow, gmode 3 */
    H_REVIEW_MAIN = 0x0047A97E, /* REVIEW OPTIONS 5: return to main menu    */
    H_SRAL        = 0x00471F34, /* main 1: START RALLY                      */
    H_A8          = 0x004A528F, /* START RALLY 3: the A8 championship       */
    A_A8LOCK      = 0x0070CC34, /* 1 = A8's slot open (0x004A5290)          */
    /* the A8 championship, 0x004A557A (NEW SIMULATION over its own
     * table, same screen and art) and the flow FUN_004A60A7 */
    T_A8NEW       = 0x006FC06C, /* NEW SIMULATION's table, 0x004A557A       */
    H_A8_1P       = 0x004A586B, /* its item 1: one player                   */
    A_A8CARS      = 0x0070C022, /* the A8 championship's car list           */
    S_ART_A8WELC  = 0x006E4697, /* 0x0047B48A: a8_welc.png                  */
    H_PLAYERS     = 0x0047D807, /* START RALLY 1: single                    */
    H_SINGLE_1P   = 0x0047E40F, /* players 1: the single flow, gmode 0      */
    H_CAR_PREV    = 0x00478F98, H_CAR_NEXT  = 0x00478FF9,
    H_GEAR_PREV   = 0x004665C2, H_GEAR_NEXT = 0x00466576,
    H_DIR         = 0x0047E041,
    H_RALLY_NEXT  = 0x0047E04E, H_RALLY_PREV = 0x0047E06F,
    H_STAGE_NEXT  = 0x0047E090, H_STAGE_PREV = 0x0047E0B1,
    H_NOP         = 0x0046E2FE, /* a bare RET                               */

    /* tables of data */
    A_PLAYER0     = 0x007083B8, /* [0x708DE8]: player 1's record           */
    A_CARLIST     = 0x0070C06A, /* 0x0047E44C: the default car list         */
    A_ITIN        = 0x006E7E85, /* [dir*4] -> [rally*4] -> +stage*0xA4      */
    A_DIRTOK      = 0x0070AA80, /* [dir*4]: "simulation" / "arcade"         */
    A_SETS        = 0x006E7539, /* the stage-set chain, FUN_0046F686        */
    A_TEXTFLASH   = 0x004EEF48, /* textflashmode, default 3                 */
    A_ITEMFLAG    = 0x00708014, /* 1: the selected item's phase is 0        */
    A_BLENDLUT    = 0x004EEB48  /* 0x00405C3A: the blended LUT              */
};

/* (the user's decision, 2026-10-10): multi-player and network play
 * are deferred.  Their entries stay; while the cursor is on one, the
 * original's own NOT YET AVAILABLE! box (0x00415A0C, token 0x1FE - SELECT
 * LEVEL's locked level) is drawn with THE PORT'S words, not the original's:
 * a deliberate addition. */
#define TOK_NOT_YET 0x1FE
#define TOK_EXIT_T   1337   /* tokExitT, the position of its line in ENGLISH.TXT */
#define TOK_RETIRE_T 1338   /* tokRetireT */
#define COMING_SOON "COMING SOON"

#define TOK_STR    0x1000          /* bit 0x1000: the runtime string table   */
#define K_NONE     0x6F            /* +24..+30: no neighbour                 */
#define K_CALL     0xFFFFFC1Au     /* +24..+30: call +34..+40 instead        */

/* ---- the data, read from the image -------------------------------------- */

typedef struct {
    int32_t  x, y, tx, ty, id, tok, font, kind, img;
    uint32_t nb[4], cb[4];         /* UP DOWN LEFT RIGHT */
    uint32_t sel, selectable, font4c, font50, arg54, perglyph;
    int      oty;                  /* ty as loaded, before any row was closed up */
    int      hidden;               /* the port's copy only, see HIDE[] */
} fe_item;

typedef struct {
    uint32_t va;
    int      nlab;
    int32_t  lab[8][4];            /* {x, y, font, token} */
    int      n;
    fe_item  it[40];
    int      vy_n;                 /* /: see vm_y */
    int      vy_from[8], vy_pitch[8];
} fe_table;

typedef struct {
    uint32_t   va;
    gfx2d_font f;
} fe_font;

enum { SC_MAIN, SC_SRAL, SC_PLAYERS, SC_CAR, SC_STAGE, SC_TTPLAYERS,
       SC_TTSTAGE, SC_SETUP, SC_RESULTS, SC_REVIEW,
       SC_ARCLEVEL, SC_ARCNEXT, SC_ARCRES, SC_ARCOVER, SC_ARCBEST, SC_ARCTOP,
       SC_ARCCONT, SC_ARCNEXTLV, SC_ARCRESTART, SC_ARCCAR,
       /* the championship */
       SC_CHNEW, SC_CHSPLASH, SC_CHWELCOME, SC_CHLEG, SC_CHNEXT, SC_CHCLASS,
       SC_CHOVER, SC_CHRCLASS, SC_CHRALLY, SC_CHCCLASS, SC_CHCHAMP, SC_CHBEST,
       SC_CHBESTCH, SC_CHBESTOV, SC_CHREVIEW, SC_CHREVIEW2, SC_CHREVIEWR,
       SC_CHREVIEWF, SC_RETIRE,
       
       SC_SULOAD, SC_SUSAVE,
       
       SC_CHLOAD, SC_CHSAVE,
       /* OPTIONS and what it opens */
       SC_OPTIONS, SC_PDET, SC_P1DET, SC_AUDIO, SC_ADVAUDIO /* retired, no OPT[] row; the slot stays */, SC_VISPREF,
       SC_REDEF, SC_CTRL, SC_CTRLPROG,
       /* time trial's BEST TIMES */
       SC_TTBEST,
       /* the INSPECTION AREA */
       SC_CHINSPECT,
       /* the service area's VIEW STAGE */
       SC_VIEWSTAGE,
       /* the same page from time trial, FUN_0048AEB9 */
       SC_TTVIEWSTAGE,
       /* INFORMATION and what it opens */
       SC_INFO, SC_INFOTIMES, SC_INFOSCORES, SC_INFOSTAGE,
       /* VISUAL OPTIONS */
       SC_VISOPT,
       /* INFORMATION's CAR SPECIFICATIONS */
       SC_CSPECS,
       /* CAR SPECIFICATIONS' SLIDESHOW */
       SC_SLIDES,
       /* SAVE VIDEO REPLAY's slot screen */
       SC_RPLSAVE,
       /* VIEW VIDEO REPLAY's slot screen */
       SC_RPLLOAD, SC_COUNT };
static const char *const sc_name[SC_COUNT] = {
    "MAIN MENU", "START RALLY", "SELECT NUMBER OF PLAYERS", "car select",
    "SELECT STAGE", "SELECT NUMBER OF PLAYERS (time trial)",
    "SELECT STAGE (time trial)", "PLAYER 1 SERVICE AREA", "RESULTS",
    "REVIEW OPTIONS",
    "SELECT LEVEL", "NEXT STAGE", "RESULTS (arcade)", "RESULTS (arcade, lost)",
    "BEST TIMES (arcade)", "TOP SCORES", "REVIEW OPTIONS (arcade: continue)",
    "REVIEW OPTIONS (arcade: start next level)",
    "REVIEW OPTIONS (arcade: restart level)", "the bonus car",
    "NEW SIMULATION", "the championship's welcome (mob_welc.png)", "WELCOME",
    "LEG DETAILS", "NEXT STAGE (championship)", "CLASS STAGE RESULTS",
    "OVERALL STAGE RESULTS", "the rally's class standings",
    "the rally's standings", "CLASS CHAMPIONSHIP", "OVERALL CHAMPIONSHIP",
    "BEST TIMES (championship)", "BEST CHAMPIONSHIP SCORES",
    "BEST OVERALL SCORES", "REVIEW OPTIONS (championship: after a service)",
    "REVIEW OPTIONS (championship)", "REVIEW OPTIONS (championship: a rally's end)",
    "REVIEW OPTIONS (championship: the end)", "RETIRE",
    "LOAD SETTINGS", "SAVE SETTINGS", "LOAD CHAMPIONSHIP", "SAVE CHAMPIONSHIP",
    "OPTIONS", "PLAYER DETAILS", "PLAYER 1 DETAILS", "AUDIO SETUP",
    "ADVANCED AUDIO SETUP", "VISUAL PREFERENCES", "REDEFINE KEYBOARD",
    "SETUP CONTROLLER", "SETUP CONTROLLER (program controller)",
    "BEST TIMES (time trial)", "INSPECTION AREA", "VIEW STAGE",
    "VIEW STAGE (time trial)",
    "INFORMATION", "BEST TIMES (information)", "TOP SCORES (information)",
    "STAGE INFORMATION", "VISUAL OPTIONS", "CAR SPECIFICATIONS", "SLIDESHOW",
    "SAVE VIDEO REPLAY", "VIEW VIDEO REPLAY"
};
static const uint32_t sc_table[SC_COUNT] = {
    T_MAIN, T_SRAL, T_PLAYERS, T_CAR, T_STAGE, T_TTPLAYERS, T_TTSTAGE, T_SETUP,
    T_RESULTS, T_REVIEW, T_ARCLEVEL, T_ARCNEXT, T_ARCRES, T_ARCOVER, T_ARCBEST,
    T_ARCTOP, T_ARCCONT, T_ARCNEXTLV, T_ARCRESTART, T_ARCCAR,
    T_CHNEW, T_CHWELCOME, T_CHWELCOME, T_CHLEG, T_CHNEXT, T_CHCLASS, T_CHOVER,
    T_CHRCLASS, T_CHRALLY, T_CHCCLASS, T_CHCHAMP, T_CHBEST, T_CHBESTCH,
    T_CHBESTOV, T_CHREVIEW, T_CHREVIEW2, T_CHREVIEWR, T_CHREVIEWF, T_RETIRE,
    T_SULOAD, T_SUSAVE, T_CHLOAD, T_CHSAVE,
    0x00700E84, 0x00701174, 0x00701348, 0x006FF634, 0x007072A8, 0x006FABF4,
    0x006FE4D4, 0x006FC29C, 0x006FC584,
    T_TTBEST, T_CHINSPECT, T_VIEWSTAGE, T_TTVIEWSTAGE,
    0x00702280, 0x00700654, 0x00703DE8, 0x007004DC,         
    0x00706EAC,                                  /* VISUAL OPTIONS */
    0x006FDF9C,                                  /* CAR SPECIFICATIONS */
    0x006FE170,                                  /* SLIDESHOW */
    T_RPLSAVE,                                   /* SAVE VIDEO REPLAY */
    T_RPLLOAD                                    /* VIEW VIDEO REPLAY  */
};

struct fe {
    ral_image  im;
    vfs        fs;
    lexicon    lx;
    char       assets[1024];

    uint8_t   *mbuts;  size_t mbuts_len;     /* [0x00707EED]                 */
    uint8_t   *fnt[4]; size_t fnt_len[4];
    uint32_t   fnt_var[4];                   /* 0x5179E8 / E4 / E0 / DC      */
    uint32_t   lut[256];                     /* PAL_00, [0x5179D8]           */
    fe_font    font[80];
    int        nfont;
    gfx2d_font scratch;                      /* 0x00517EAC                   */
    uint32_t   blend[256];                   /* 0x004EEB48                   */
    int        textflash, itemflag;

    uint32_t   bg[W * H];                    /* [0x004EF9B0], the canvas     */
    uint32_t   px[W * H];                    /* the framebuffer              */
    gfx2d_surface surf;

    char       str[16][256];                 /* str#n, kept natively */
    fe_table   tab[SC_COUNT];

    int        sc;                           /* the screen                   */
    int        cursor;                       /* [0x707E91]                   */
    int        saved[SC_COUNT];              /* each screen's own dword      */
    int        carousel_kind;                /* [0x70C9E0], -1 = none        */
    uint32_t   key_ms;                       /* [0x70802C]                   */
    uint32_t   now;
    int        launched;
    int        soon;                         /* 0x1FE says COMING SOON */
    int        soon_on, soon_sc, soon_cur;   /* the box, until the cursor
                                                leaves the chosen entry */
    uint32_t   post[W * H];                  /* the post-draw layer, the */
    gfx2d_surface post_surf;                 /* box alone, over the 3D models   */
    int        post_on;                      /* this frame drew a box onto post */
    int        quit_confirm;                 /* the port's: ask before the main
                                                menu's quit (fe_set_quit_confirm) */
    int        quit_screen;                  /* RETIRE is up as the quit's
                                                question (quit_ask), not a
                                                championship's              */
    int        mode;                         /* [0x7082BC]: 0 single, 3 time trial;
                                                2 the arcade (the port's: the arcade's
                                                flow does not write [0x7082BC]) */
    uint32_t   carlist;                      /* car select's list (ESI at 0x00476F13) */
    vm        *vm;                           /* the original's data and draw code (x86vm.h) */
    menu3d    *m3d;                          /* the 3D models, NULL = not drawn */
    uint32_t   scr_ms;                       /* [0x00512730]: the screen loop's start */
    int        win_w, win_h;                 /* the window the models draw into     */
    int        vm_bad;                       /* the VM stopped: said once          */
    /* the platform under the original's save code - the file layer's
     * primitives (0x00402BCD..0x00402C5A), VirtualAlloc (0x004028FF) */
    vfs        inst;                         /* the install's own var\, read only */
    int        writes;                       /* 0: an open for write fails       */
    int        no_read;                      /* var\ral.dat / hst.dat are absent */
    plat_file *fh[4];
    char       fname[4][64];                 /* the game's name, for the log     */
    int        fwr[4];                       /* opened for write                 */
    uint32_t   heap_top;                     /* the next page VirtualAlloc gives */
    int        said_nowrite;
    int        dik;                          /* the key 0x00407CDC hands over    */
    uint8_t    typed[16], typed_shift[16];   /* fe_type's keys, one a frame      */
    int        ntyped;
    uint32_t   su_ecx;                       /* the save loop's ECX (su_typing)  */
    uint32_t   vs_stage[2];                  /* [0x7082C0] / [0x7082C4]   */
    uint32_t   ch_loaded;                    /* what the load went on to  */
    int        a8;                           /* the A8 championship (FUN_004A60A7), not the Mobil 1's */
    int        a8_tab;                       /* SC_CHNEW holds A8's table (0x6FC06C) and its own cursor */
    int        a8_saved;                     /* NEW SIMULATION's cursor dword [0x6FA274] (the Mobil 1's is saved[]) */
    int        chsave_ret;                   /* the REVIEW save rally came from  */
    int        rpl_ret;                      /* the REVIEW the replay save came from */
    char       view_path[1100];              /* the replay FE_VIEW plays   */
    int        view_ret;                     /* the screen the viewer was chosen on */
    int        su_valid;                     /* a setup screen has been committed  */
    setup_sliders su_sl;                     /* player+0x10C..0x120 at the launch  */

    /* the state the handlers write */
    int32_t    p_car, p_gear, p_90;          /* player+0x08, +0x30, +0x90    */
    int32_t    car_index, car_max;           /* [0x7082FC], [0x708E08]       */
    int32_t    dir, rally, stage;            /* [0x708230/38/34]             */
    int32_t    track, crsdirec;              /* [0x70CC3C], [0x70CC58]       */
    char       said[64][16];                 /* unported handlers, once each */
    int        nsaid;
    snd       *snd;                          /* NULL = silent         */

    /* the championship's flow, FUN_0049CB25 cut at each screen (the
     * state it keeps is the original's globals in the VM; these are only
     * where it resumes) */
    int        ch_pc;                        /* the label the next leave resumes at */
    int        ch_k;                         /* a class loop's EBX                  */
    int        ch_after;                     /* where 0x0049F81D's screen goes on   */
    int        ch_phase;                     /* what RETIRE's yes does (ch_yes)     */
    int        ch_lb, ch_lb_pc;              /* the REVIEW a leaderboard returns to */
    int        ret_sc;                       /* the screen RETIRE came from         */
    uint32_t   ch_req;                       /* a hooked screen body was called     */
    int        ch_launch;                    /* the launcher 0x00480461 was called  */
    int32_t    ch_champ88;                   /* player+0x288 at that call   */
    int        fin_scene;                    /* 0x004264D7 was reached       */
    fe_scene_fn scene_cb;                    /* runs the scene       */
    void      *scene_ud;
    uint32_t   splash_ms;                    /* the welcome picture's start         */

    /* the OPTIONS tree - where each screen's BACK goes (the body that
     * called it), and whether its platform hooks are in */
    int        opt_parent[SC_COUNT];
    int        opt_hooked;
    int        ctl_armed;         /* program controller: the pad has been at
                                     rest since the row was chosen */
    uint32_t   ctl_map[11];       /* the mapping dwords last logged */
};

static uint32_t rd(const fe *f, uint32_t va)
{
    const unsigned char *p = ral_image_at(&f->im, va, 4);
    return p ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24) : 0u;
}

/* the interpreter's copy of the data, where the original's code has
 * written it (the car lists the bonus car rewrites, 0x00482BCE); the
 * image's before the interpreter exists. */
static uint32_t drd(const fe *f, uint32_t va)
{
    return f->vm ? vm_rd32(f->vm, va) : rd(f, va);
}

static const char *rdstr(const fe *f, uint32_t va)
{
    const unsigned char *p = ral_image_at(&f->im, va, 1);
    return p ? (const char *)p : "";
}

/* a host path, read whole */
static int load_host_file(const char *path, uint8_t **buf, size_t *len, char *err,
                          size_t n)
{
    plat_file *h;
    int64_t sz;
    h = plat_file_open(path, 0);
    if (!h) { snprintf(err, n, "cannot open %s", path); return -1; }
    sz = plat_file_size(h);
    *buf = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1);
    if (!*buf || sz <= 0 || plat_file_read(h, *buf, (size_t)sz) != (size_t)sz) {
        plat_file_close(h);
        free(*buf);
        *buf = NULL;
        snprintf(err, n, "cannot read %s", path);
        return -1;
    }
    plat_file_close(h);
    *len = (size_t)sz;
    return 0;
}

static int load_file(const fe *f, const char *game_path, uint8_t **buf,
                     size_t *len, char *err, size_t n)
{
    char path[1024];
    if (vfs_resolve(&f->fs, game_path, path, sizeof path) != 0) {
        snprintf(err, n, "missing: %s", path);
        return -1;
    }
    return load_host_file(path, buf, len, err, n);
}

/* ---- the art ------------------------------------------------------------ */

/* FUN_0047B0C2: the background into the canvas, then the frame (sprite 0xA0)
 * over it at (0, 0). */
static int load_art(fe *f, const char *game_path, char *err, size_t n)
{
    uint8_t *buf;
    size_t len;
    unsigned w, h;
    int rc;
    if (load_file(f, game_path, &buf, &len, err, n)) return -1;
    rc = png_decode(buf, len, f->bg, W * H, &w, &h, err, n);
    free(buf);
    if (rc) return -1;
    if (w != W || h != H) {
        snprintf(err, n, "%s is %ux%u, not 800x600", game_path, w, h);
        return -1;
    }
    {
        gfx2d_surface s;
        gfx2d_surface_init(&s, f->bg, W, H);
        gfx2d_blit(&s, f->mbuts, f->mbuts_len, 0xA0, NULL, 0, 0);
    }
    return 0;
}

/* FUN_004115E1 then the frame: ukmap.png, the stage's map over it where it is
 * not 0xFF00FF (0x0041165A), at (0, 0) - SELECT STAGE's background,
 * FUN_00465011 (its single-stage arm, [0x708068] == 1: and the
 * arcade's NEXT STAGE, [0x708068] == 2, from the arcade itinerary). */
/* FUN_004115E1: the picture `base`, then `over` on it wherever `over` is not
 * 0xFF00FF (0x0041165A), at (0, 0); then, as 0x0047B0C2 does, the frame
 * sprite 0xA0 when `frame` (FUN_004650F5's WELCOME and 0x0047B3D7's
 * welcome picture do not draw it). */
static int load_art_over(fe *f, const char *base, const char *over, int frame,
                         char *err, size_t n)
{
    uint8_t *buf;
    size_t len;
    unsigned w, h, x, y;
    static uint32_t tmp[W * H];
    if (load_file(f, base, &buf, &len, err, n)) return -1;
    if (png_decode(buf, len, f->bg, W * H, &w, &h, err, n)) { free(buf); return -1; }
    free(buf);
    if (!over) w = h = 0;                        /* the picture alone */
    else {
        if (load_file(f, over, &buf, &len, err, n)) return -1;
        if (png_decode(buf, len, tmp, W * H, &w, &h, err, n)) { free(buf); return -1; }
        free(buf);
    }
    for (y = 0; y < h && y < H; y++)
        for (x = 0; x < w && x < W; x++) {
            uint32_t v = tmp[y * w + x];
            if ((v & 0xFFFFFFu) != 0xFF00FFu) f->bg[y * W + x] = v;
        }
    if (frame) {
        gfx2d_surface s;
        gfx2d_surface_init(&s, f->bg, W, H);
        gfx2d_blit(&s, f->mbuts, f->mbuts_len, 0xA0, NULL, 0, 0);
    }
    return 0;
}

/* SLIDESHOW's picture, FUN_004115E1 (0x0046512D calls it, not 0x0047B0C2: no
 * frame sprite).  The slides are 640x480; 0x00411573 makes the canvas the picture's own
 * size, pitch width * 4, and 0x0041164A..0x0041167A then lays the title bar (800 x 85)
 * over it row by row at that pitch - 800 pixels written a row of 640, so each row's last
 * 160 spill onto the next row's first 160, which the next row's own write covers again;
 * only the LAST row's spill (into row 85, x 0..159) stays.  `over` NULL: the picture
 * alone (0x00411573, the fall-back's one).  The port's canvas is 800x600: the 640x480
 * canvas goes in at (0, 0), the rest black.  READ from the code, not seen: no capture of
 * the original's slideshow exists. */
static int load_slide(fe *f, const char *base, const char *over, char *err, size_t n)
{
    uint8_t *buf;
    size_t len;
    unsigned w, h, tw = 0, th = 0, x, y;
    static uint32_t cv[W * H], tmp[W * H];
    uint32_t black;
    if (load_file(f, base, &buf, &len, err, n)) return -1;
    if (png_decode(buf, len, cv, W * H, &w, &h, err, n)) { free(buf); return -1; }
    free(buf);
    if (w > W || h > H) { snprintf(err, n, "%s is %ux%u, larger than 800x600", base, w, h); return -1; }
    black = cv[0] & 0xFF000000u;
    if (over) {
        if (load_file(f, over, &buf, &len, err, n)) return -1;
        if (png_decode(buf, len, tmp, W * H, &tw, &th, err, n)) { free(buf); return -1; }
        free(buf);
        for (y = 0; y < th; y++)
            for (x = 0; x < tw; x++) {
                uint32_t v = tmp[y * tw + x];
                if ((v & 0xFFFFFFu) != 0xFF00FFu && y * w + x < W * H) cv[y * w + x] = v;
            }
    }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            f->bg[y * W + x] = (x < w && y < h) ? cv[y * w + x] : black;
    return 0;
}

/* FUN_004115E1 then the frame: ukmap.png, the stage's map over it - SELECT
 * STAGE's background, FUN_00465011 (its single-stage arm, [0x708068] == 1;
 * and the arcade's NEXT STAGE, [0x708068] == 2, from the arcade
 * itinerary: and the championship's, 3, from its own). */
static uint32_t arc_entry(const fe *f);
static uint32_t ch_entry(const fe *f, int ahead);
static int load_stage_art(fe *f, char *err, size_t n)
{
    uint32_t e = f->sc == SC_ARCNEXT ? arc_entry(f)        /* 0x00465048 */
               : f->sc == SC_CHNEXT || f->sc == SC_VIEWSTAGE
                   ? ch_entry(f, 0)                        /* 0x00465064 */
               : rd(f, rd(f, A_ITIN + 4u * (uint32_t)f->dir) +
                    4u * (uint32_t)f->rally) + 0xA4u * (uint32_t)f->stage;
    uint32_t map = rd(f, A_STAGEMAPS + 4u * rd(f, e + 0x3C));
    return load_art_over(f, rdstr(f, S_ART_UKMAP), rdstr(f, map), 1, err, n);
}

/* 0x00478799: mbuts00.pcx, mbuts01.pcx ... (vfs_build_numbered_path, the
 * "00" from the end) into one bundle, until a file is missing. */
static int load_buttons(fe *f, char *err, size_t n)
{
    sheet_builder b;
    char name[256];
    int k, first = -1;
    static uint32_t px[W * H];
    sheet_builder_init(&b);
    for (k = 0; k < 100; k++) {
        char probe[1024];
        uint8_t *buf;
        size_t len;
        unsigned w, h, i;
        char *z;
        snprintf(name, sizeof name, "%s", rdstr(f, S_MBUTS));
        z = strstr(name, "00");
        if (!z) break;
        z[0] = (char)('0' + k / 10);
        z[1] = (char)('0' + k % 10);
        if (vfs_resolve(&f->fs, name, probe, sizeof probe) != 0) break;
        if (load_file(f, name, &buf, &len, err, n)) { sheet_builder_free(&b); return -1; }
        if (pcx_decode_request(buf, len, px, W * H, W, H, &w, &h, err, n)) {
            free(buf);
            sheet_builder_free(&b);
            return -1;
        }
        free(buf);
        for (i = 0; i < w * h; i++) px[i] &= 0x00FFFFFFu;
        {
            int got = sheet_cut(&b, px, (int)w, (int)h, NULL);
            if (got < 0) { sheet_builder_free(&b); snprintf(err, n, "out of memory"); return -1; }
            if (first < 0) first = got;
        }
    }
    if (first != 0xA0 || b.n < 0xA1u) {
        snprintf(err, n, "the button sheets cut to %d + %u sprites; the code "
                 "draws the frame as sprite 0xA0 (0x0047B0E1), so the first "
                 "must give 160", first, b.n);
        sheet_builder_free(&b);
        return -1;
    }
    f->mbuts = sheet_bundle(&b, &f->mbuts_len);
    sheet_builder_free(&b);
    return f->mbuts ? 0 : -1;
}

/* FUN_004150C9's loads, parsed out of its own code: each is
 *   8D 05 <path>  E8 <rel32 -> 0x00411D4C>  A1 1C 31 51 00  A3 <var>
 * The first is PAL_00 (its palette becomes the LUT, FUN_00411C8F); the rest
 * are the four sheets.  A shape this does not recognise is an error. */
static int load_fonts(fe *f, char *err, size_t n)
{
    uint32_t a = FONT_BUILD;
    int nsheet = 0, k;
    for (k = 0; k < 5; k++) {
        const unsigned char *p;
        uint32_t path, var;
        /* scan forward to the next LEA EAX,[imm32] */
        for (;;) {
            p = ral_image_at(&f->im, a, 2);
            if (!p || a > FONT_RECS) { snprintf(err, n, "FUN_004150C9: load %d not found", k); return -1; }
            if (p[0] == 0x8D && p[1] == 0x05) break;
            a++;
        }
        path = rd(f, a + 2);
        p = ral_image_at(&f->im, a + 6, 16);
        if (!p || p[0] != 0xE8 || p[5] != 0xA1 || p[10] != 0xA3 ||
            a + 11 + (uint32_t)(p[1] | p[2] << 8 | p[3] << 16 | (uint32_t)p[4] << 24) != 0x00411D4Cu) {
            snprintf(err, n, "FUN_004150C9: load %d at 0x%08X has an unknown shape", k, a);
            return -1;
        }
        var = rd(f, a + 17);
        a += 21;
        {
            uint8_t *buf, *idx;
            size_t len;
            unsigned w, h, i;
            uint32_t pal[256], *px;
            if (load_file(f, rdstr(f, path), &buf, &len, err, n)) return -1;
            idx = (uint8_t *)malloc(W * H);
            px = (uint32_t *)malloc(W * H * sizeof *px);
            if (!idx || !px || pcx_decode_index(buf, len, idx, W * H, pal, &w, &h, err, n)) {
                free(buf); free(idx); free(px);
                return -1;
            }
            free(buf);
            if (k == 0) {
                /* PAL_00: FUN_00411C8F converts its palette into the LUT */
                for (i = 0; i < 256; i++) f->lut[i] = 0xFF000000u | pal[i];
            } else {
                sheet_builder b;
                int got;
                for (i = 0; i < w * h; i++) px[i] = idx[i];
                sheet_builder_init(&b);
                got = sheet_cut(&b, px, (int)w, (int)h, pal);
                if (got != 0xFF - 0x20 + 1) {
                    snprintf(err, n, "%s cuts to %d glyphs, not 224", rdstr(f, path), got);
                    sheet_builder_free(&b); free(idx); free(px);
                    return -1;
                }
                f->fnt[nsheet] = sheet_bundle(&b, &f->fnt_len[nsheet]);
                f->fnt_var[nsheet] = var;
                nsheet++;
                sheet_builder_free(&b);
            }
            free(idx);
            free(px);
        }
    }
    return nsheet == 4 ? 0 : -1;
}

static fe_font *font_slot(fe *f, uint32_t rec)
{
    int i;
    for (i = 0; i < f->nfont; i++) if (f->font[i].va == rec) return &f->font[i];
    if (f->nfont >= (int)(sizeof f->font / sizeof f->font[0])) return NULL;
    f->font[f->nfont].va = rec;
    memset(&f->font[f->nfont].f, 0, sizeof f->font[f->nfont].f);
    return &f->font[f->nfont++];
}

static void font_static(const fe *f, gfx2d_font *g, uint32_t rec)
{
    const unsigned char *p = ral_image_at(&f->im, rec, 0x20);
    if (!p) return;
    g->first   = p[0];
    g->last    = p[1];
    g->align   = p[2];
    g->flags   = p[3];
    g->gap     = (int32_t)rd(f, rec + 0x04);
    g->line_h  = (int32_t)rd(f, rec + 0x08);
    g->y_origin = (int32_t)rd(f, rec + 0x0C);
    g->esc_gap = (int32_t)rd(f, rec + 0x10);
    g->field_w = (int32_t)rd(f, rec + 0x1C);
}

/* The MOV run 0x004151C1..0x004157B9: A1 <src> [83 C0 ib | 05 id] A3 <dst>.
 * A store to rec+0x14 is the LUT (PAL_00's, + the byte offset); to rec+0x18
 * the bundle (src is the sheet's variable). */
static int wire_fonts(fe *f, char *err, size_t n)
{
    uint32_t a = FONT_RECS, src = 0, add = 0;
    int k;
    while (a < FONT_RECS_END) {
        const unsigned char *p = ral_image_at(&f->im, a, 6);
        if (!p) break;
        if (p[0] == 0xA1) { src = rd(f, a + 1); add = 0; a += 5; continue; }
        if (p[0] == 0x83 && p[1] == 0xC0) { add = (uint32_t)(int32_t)(int8_t)p[2]; a += 3; continue; }
        if (p[0] == 0x05) { add = rd(f, a + 1); a += 5; continue; }
        if (p[0] == 0xA3) {
            uint32_t dst = rd(f, a + 1);
            uint32_t field = (dst - 0x00517ECCu) & 0x1Fu;
            fe_font *s = font_slot(f, dst - field);
            if (!s) { snprintf(err, n, "too many font records"); return -1; }
            if (field == 0x14) {
                if (src != 0x005179D8u || add % 4 || add / 4 > 255) {
                    snprintf(err, n, "font LUT at 0x%08X: unknown source", a);
                    return -1;
                }
                s->f.lut = f->lut + add / 4;
            } else if (field == 0x18) {
                for (k = 0; k < 4; k++)
                    if (f->fnt_var[k] == src) {
                        s->f.bundle = f->fnt[k];
                        s->f.bundle_len = f->fnt_len[k];
                    }
                if (!s->f.bundle) {
                    snprintf(err, n, "font bundle at 0x%08X: unknown sheet 0x%08X", a, src);
                    return -1;
                }
            } else {
                snprintf(err, n, "font run at 0x%08X stores +0x%X", a, field);
                return -1;
            }
            a += 5;
            continue;
        }
        snprintf(err, n, "font run: unknown instruction at 0x%08X", a);
        return -1;
    }
    /* 0x005185CC keeps its static LUT pointer, 0x005179EC: the palette copy
     * 0x00415152 makes - the same colours as the converted LUT's base. */
    for (k = 0; k < f->nfont; k++) {
        font_static(f, &f->font[k].f, f->font[k].va);
        if (!f->font[k].f.lut) f->font[k].f.lut = f->lut;
    }
    font_static(f, &f->scratch, 0x00517EACu);
    return 0;
}

static gfx2d_font *font(fe *f, uint32_t rec)
{
    int i;
    if (rec == 0x00517EACu) return &f->scratch;
    for (i = 0; i < f->nfont; i++) if (f->font[i].va == rec) return &f->font[i].f;
    return NULL;
}

/* ---- tables ------------------------------------------------------------- */

static uint32_t drd(const fe *f, uint32_t va);

/* ----: rows the port does not show ---------------------------------
 *
 * One line per hidden row: {table, item, sel}, where `table` is the screen's
 * table address (0 = any table), `item` the row's number, 1-based (0 = any),
 * and `sel` its handler, +0x44 (0 = any).  A row matching every non-zero
 * field is hidden in the PORT'S copy of the table (fe_table), after every
 * load - the image's and the interpreter's live reload alike: it is not drawn,
 * its neighbours' UP / DOWN links step over it, and the rows below it (a
 * larger y; BACK is a footer and stays put) move up by the row's pitch.  The interpreter's memory is never written, so the
 * original's handlers, the item numbers they index by, the cursor dwords and
 * var\ral.dat are exactly as they were (one exception, not here: opt_enter
 * seeds SETUP CONTROLLER's device). / add rows here.
 * vy_from / vy_pitch (vm_y) hold up to eight closed-up rows per table (VISUAL OPTIONS closes five), in the coordinates the original draws at. */
static const struct { uint32_t table; int item; uint32_t sel; } HIDE[] = {
    { 0x00700E84u, 0, 0x0046D1F3u },  /* OPTIONS: return to intro            */
    { 0x006FF634u, 0, 0x0047C8E4u },  /* AUDIO SETUP: advanced audio         */
    { 0x006FC29Cu, 3, 0 },            /* SETUP CONTROLLER: the shifter, +0x3C */
    /* Both builds for the release (the user, 2026-11-05).  Resolution: the
     * desktop's own window-size row is post-release (Alt+Enter and --window
     * cover it).  Mipmapping: comes back as HD TEXTURES with the HD track. */
    { 0x00706EACu, 3, 0 },            /* VISUAL OPTIONS: resolution, arrows 0x46CC32 / 0x46CC46 */
    { 0x00706EACu, 5, 0 },            /* VISUAL OPTIONS: mipmapping, arrows 0x46CC0A */
    /* (the user, 2026-11-05): the pad is always live, so the
     * device picker changes nothing; hidden on both builds.  opt_enter seeds
     * a 0 device to 1, or the screen's other rows stay greyed. */
    { 0x006FC29Cu, 2, 0 },            /* SETUP CONTROLLER: the device, +0x2C, arrows 0x467174 / 0x467128 */
    { 0,           0, 0x004799BAu },  /* every REVIEW OPTIONS: IN-GAME OPTIONS */
    /* /: the replay rows work now, none is hidden.  (OPTIONS' "video replay"
     * was never hidden: hiding it moved every row under it, and the OPTIONS walks of
     * rally-menus / rally-saves count them.) */
    { 0x00706EACu, 2, 0 },            /* VISUAL OPTIONS: video card, arrows 0x46CC6E */
    { 0x00706EACu, 4, 0 },            /* VISUAL OPTIONS: buffering, arrows 0x46CC1E  */
    { 0x00706EACu, 0, 0x0046D027u },  /* VISUAL OPTIONS: set new mode               */
};

static void hide_rows(fe_table *t)
{
    int h, k, j;
    for (h = 1; h <= t->n; h++) {
        fe_item *it = &t->it[h - 1];
        uint32_t up, dn;
        int pitch = 0, hit = 0, moved = 0;
        for (k = 0; k < (int)(sizeof HIDE / sizeof HIDE[0]); k++)
            if ((!HIDE[k].table || HIDE[k].table == t->va) &&
                (!HIDE[k].item || HIDE[k].item == h) &&
                (!HIDE[k].sel || HIDE[k].sel == it->sel)) hit = 1;
        if (!hit || it->hidden) continue;
        it->hidden = 1;
        up = it->nb[0]; dn = it->nb[1];
        if (up >= 1 && up <= (uint32_t)t->n && t->it[up - 1].y < it->y)
            pitch = it->y - t->it[up - 1].y;
        else if (dn >= 1 && dn <= (uint32_t)t->n && t->it[dn - 1].y > it->y)
            pitch = t->it[dn - 1].y - it->y;
        for (j = 0; j < t->n; j++) {
            fe_item *o = &t->it[j];
            if (j == h - 1) continue;
            if (o->nb[0] == (uint32_t)h) o->nb[0] = up;
            if (o->nb[1] == (uint32_t)h) o->nb[1] = dn;
            if (o->y > it->y && o->sel != H_BACK) {
                o->y -= pitch; o->ty -= pitch;
                moved = 1;
            }
        }
        /* vm_y's list, in the ORIGINAL coordinates the interpreter draws at
         * (a screen may close up several rows) */
        if (moved && pitch && t->vy_n < (int)(sizeof t->vy_from / sizeof t->vy_from[0])) {
            t->vy_from[t->vy_n] = it->oty; t->vy_pitch[t->vy_n] = pitch;
            t->vy_n++;
        }
    }
}

/* The draws the interpreter's own code makes for a screen whose rows were
 * closed up (SETUP CONTROLLER's pre-draw 0x004671BD writes each row's value
 * and slider at that row's ty, as a constant): the hidden row's own value is
 * dropped and every one below it moves up with its row.  0 = do not draw. */
static int vm_y(const fe *f, int y, int *out)
{
    const fe_table *t = &f->tab[f->sc];
    int k;
    *out = y;
    for (k = 0; k < t->vy_n; k++) if (y == t->vy_from[k]) return 0;
    for (k = 0; k < t->vy_n; k++) if (y > t->vy_from[k]) *out -= t->vy_pitch[k];
    return 1;
}

/* `live` reads the table from the interpreter's data - the OPTIONS
 * tree's handlers rewrite their own items (0x00468156 / 0x0046816F grey one
 * out: +0x48 and its font +0x18) - and the image otherwise */
static uint32_t trd(const fe *f, int live, uint32_t va)
{
    return live ? drd(f, va) : rd(f, va);
}

static void load_table_from(fe *f, fe_table *t, uint32_t va, int live)
{
    uint32_t lab = trd(f, live, va), a = va + 4;
    memset(t, 0, sizeof *t);
    t->va = va;
    while (lab && trd(f, live, lab) != 0xFFFFFFFFu && t->nlab < 8) {
        int k;
        for (k = 0; k < 4; k++) t->lab[t->nlab][k] = (int32_t)trd(f, live, lab + 4u * k);
        t->nlab++;
        lab += 16;
    }
    while (trd(f, live, a) != 0xFFFFFFFFu && t->n < 40) {
        fe_item *it = &t->it[t->n++];
        int k;
        it->x = (int32_t)trd(f, live, a + 0x00);  it->y = (int32_t)trd(f, live, a + 0x04);
        it->tx = (int32_t)trd(f, live, a + 0x08); it->ty = (int32_t)trd(f, live, a + 0x0C);
        it->id = (int32_t)trd(f, live, a + 0x10); it->tok = (int32_t)trd(f, live, a + 0x14);
        it->font = (int32_t)trd(f, live, a + 0x18); it->kind = (int32_t)trd(f, live, a + 0x1C);
        it->img = (int32_t)trd(f, live, a + 0x20);
        for (k = 0; k < 4; k++) {
            it->nb[k] = trd(f, live, a + 0x24 + 4u * k);
            it->cb[k] = trd(f, live, a + 0x34 + 4u * k);
        }
        it->oty = it->ty;
        it->sel = trd(f, live, a + 0x44);       it->selectable = trd(f, live, a + 0x48);
        it->font4c = trd(f, live, a + 0x4C);    it->font50 = trd(f, live, a + 0x50);
        it->arg54 = trd(f, live, a + 0x54);     it->perglyph = trd(f, live, a + 0x58);
        a += 0x5C;
    }
    hide_rows(t);
}

static void load_table(fe *f, fe_table *t, uint32_t va)
{
    load_table_from(f, t, va, 0);
}

static fe_item *item(fe *f, int id)
{
    fe_table *t = &f->tab[f->sc];
    if (id < 1 || id > t->n) return NULL;
    return &t->it[id - 1];                       /* 0x0046E362: by position */
}

/* a cursor that arrives on a hidden row (a remembered or default one)
 * steps to the next visible row, DOWN else UP.  Not reachable today - every
 * hidden row has a visible default and none could be selected - but a stale
 * cursor dword would otherwise sit on a row that is not drawn. */
static void cursor_visible(fe *f)
{
    const fe_table *t = &f->tab[f->sc];
    int guard;
    for (guard = 0; guard < 40; guard++) {
        const fe_item *it = item(f, f->cursor);
        uint32_t nb;
        if (!it || !it->hidden) return;
        nb = it->nb[1];
        if (nb < 1 || nb > (uint32_t)t->n) nb = it->nb[0];
        if (nb < 1 || nb > (uint32_t)t->n) return;
        f->cursor = (int)nb;
    }
}

/* ---- text --------------------------------------------------------------- */

/* the screens whose string table the original's own code fills, in
 * the interpreter (the results' page range 0x0049DC42, RETIRE's no / yes
 * 0x0046C6B1): their str#n is read from there, through the original's own
 * pointer table, [0x0051F8BC + 4 * tok] with the 0x1000 bit kept
 * (0x00401FD7): not 0xA2BF54 + (n-1) * 0x100, which holds for
 * #3..#6 only - #7..#16 are four bytes apart from 0xA2C558 */
static int vmstr_screen(int sc)
{
    return (sc >= SC_CHCLASS && sc <= SC_CHCHAMP) || sc == SC_RETIRE ||
           sc == SC_CHLOAD || sc == SC_CHSAVE ||        /* str#4, #7..16 */
           sc == SC_RPLSAVE || sc == SC_RPLLOAD ||      /* /: str#4, the page's range */
           sc == SC_VIEWSTAGE ||                        /* the selector */
           sc == SC_INFOTIMES || sc == SC_INFOSCORES ||   /* str#3 / #4 */
           sc == SC_INFOSTAGE ||                        /*   written by their own code */
           sc == SC_CSPECS || sc == SC_SLIDES;          /* str#4 the car, #5 the page (the picture row's) */
}

/* str_emit_value_q, 0x00401F8F */
static const char *tok_text(const fe *f, int32_t tok)
{
    if (tok & TOK_STR) {
        int k = tok & 0xFFF;
        if (k >= 1 && k <= 16 && f->vm && vmstr_screen(f->sc))
            return vm_cstr(f->vm, vm_rd32(f->vm, 0x0051F8BCu + 4u * (uint32_t)tok));
        return (k >= 1 && k < 16) ? f->str[k] : "";
    }
    if (f->soon && tok == TOK_NOT_YET) return COMING_SOON;      
    if (f->quit_screen && f->sc == SC_RETIRE && tok == TOK_RETIRE_T)
        return lexicon_text(&f->lx, TOK_EXIT_T);                /* quit_ask */
    return lexicon_text(&f->lx, tok);
}

static void text(fe *f, uint32_t rec, const char *s, int x, int y)
{
    gfx2d_font *g = font(f, rec), tmp;
    if (!g || !g->bundle) return;
    tmp = *g;                                    /* the record is not written */
    gfx2d_text(&f->surf, &tmp, s, x, y);
}

/* 0x004022F3 / 0x004022D2: the width of a token in a MEASURE record */
static int measure(fe *f, uint32_t rec, int32_t tok)
{
    gfx2d_font *g = font(f, rec), tmp;
    if (!g || !g->bundle) return 0;
    tmp = *g;
    tmp.field_w = 0;                             /* 0x004022D5 */
    tmp.measured = 0;                            /* 0x004022DC (+0x14)      */
    tmp.align = GFX2D_ALIGN_MEASURE;
    gfx2d_text(&f->surf, &tmp, tok_text(f, tok), 0, 0);
    return tmp.field_w;
}

/* 0x00405BD9 with ebx = 0x100: the selected item's font.  Copies the record
 * whose half of the 512 ms cycle this is into 0x00517EAC and, unless
 * textflashmode is 0, blends 15 LUT entries toward the other.  The blend is
 * done on DISPLAY values (0x00405A41 unpacks them, 0x004059D1 repacks): the
 * original's display was 5-6-5, so each input is cut to it first, which is
 * what makes the port's blended pixels the original's to the bit. */
static uint32_t blend_font(fe *f, uint32_t normal, uint32_t hl, uint32_t t)
{
    gfx2d_font *a = font(f, (t & 0x100u) ? normal : hl);
    gfx2d_font *b = font(f, (t & 0x100u) ? hl : normal);
    int i, c;
    if (!a || !b) return normal;
    f->scratch = *a;
    if (f->textflash == 0) return 0x00517EACu;
    {
        int32_t k = (int32_t)(t & 0xFFu);
        if (f->textflash == 2) {
            k = (k * k) >> 8;
            k ^= 0xFF; k = (k * k) >> 8; k ^= 0xFF;
        }
        for (i = 0; i < 15; i++) {
            uint32_t va = a->lut[i], vb = b->lut[i], o = 0xFF000000u;
            static const uint32_t m565[3] = { 0xF8u, 0xFCu, 0xF8u };
            for (c = 0; c < 3; c++) {
                int x = (int)((va >> (8 * c)) & m565[c]);
                int y = (int)((vb >> (8 * c)) & m565[c]);
                int v = x + (((y - x) * k) >> 8);
                o |= (uint32_t)(v & 0xFF) << (8 * c);
            }
            f->blend[i] = o;
        }
    }
    f->scratch.lut = f->blend;
    return 0x00517EACu;
}

/* 0x00402311 */
static void draw_tok(fe *f, uint32_t rec, int32_t tok, int x, int y)
{
    text(f, rec, tok_text(f, tok), x, y);
}

/* ---- the capsules ------------------------------------------------------- */

static void spr(fe *f, uint32_t idx, int x, int y)
{
    gfx2d_blit(&f->surf, f->mbuts, f->mbuts_len, idx, NULL, x, y);
}

/* The CLEAN shape, shared by 0x004657EC (highlighted: caps +8 / +0C) and
 * 0x00465B0B (caps +0 / +4); the middle is +10 in all three. */
static void capsule(fe *f, uint32_t L, uint32_t R, uint32_t M, int w, int x,
                    int y, int mode)
{
    int n, r, i;
    if (w == 0) return;                          /* guru 0x004D536F         */
    n = w / 32; r = w % 32;
    if (mode == 1) x -= (w >> 1) + 0x22;
    if (mode == 1 || mode == 2) {
        spr(f, L, x, y); x += 0x22;
        for (i = 0; i < n; i++) { spr(f, M, x, y); x += 0x20; }
        if (r) { x -= 0x20; x += r; spr(f, M, x, y); x += 0x20; }
        spr(f, R, x, y);
    } else if (mode == 3) {
        spr(f, R, x, y); x -= 0x20;
        for (i = 0; i < n; i++) { spr(f, M, x, y); x -= 0x20; }
        if (r) { x += 0x20; x -= r; spr(f, M, x, y); x -= 0x22; spr(f, L, x, y); }
        else   { x -= 2; spr(f, L, x, y); }
    }
}

/* 0x004654CF: caps +0 / +4, middle +10 - and its mode 3 is NOT the clean
 * shape.  Transcribed jump for jump:
 *   0x00465670  JNE 0x0046574A   a second middle continues in MODE 1's loop
 *                                (x += 0x20 from there on, then mode 1's tail)
 *   0x004656D1  with no remainder: x -= 2, the left cap, and then FALLS
 *                                THROUGH into mode 1 at 0x004656F6, which
 *                                draws a whole centred capsule again. */
static void capsule_4654cf(fe *f, const uint32_t img[3], int w, int x, int y,
                           int mode)
{
    uint32_t L = img[0], R = img[1], M = img[2];
    int n, r;
    if (w == 0) return;
    if (mode == 2 || mode == 1) { capsule(f, L, R, M, w, x, y, mode); return; }
    if (mode != 3) return;                       /* JMP 0x004D5324: a guru  */
    spr(f, R, x, y);                             /* 0x00465606 */
    x -= 0x20;
    n = w / 32; r = w % 32;
    if (n != 0) {
        spr(f, M, x, y); x -= 0x20; n--;         /* 0x0046564A */
        if (n != 0) {                            /* -> 0x0046574A           */
            do { spr(f, M, x, y); x += 0x20; } while (--n);
            goto tail1;                          /* 0x00465772              */
        }
    }
    if (r) {                                     /* 0x0046567C */
        x += 0x20; x -= r; spr(f, M, x, y); x -= 0x22; spr(f, L, x, y);
        return;
    }
    x -= 2; spr(f, L, x, y);                     /* 0x004656D1 */
    /* fall through into mode 1, 0x004656F6 */
    x -= (w >> 1) + 0x22;
    spr(f, L, x, y); x += 0x22;
    n = w / 32; r = w % 32;
    while (n) { spr(f, M, x, y); x += 0x20; n--; }
tail1:
    if (r) { x -= 0x20; x += r; spr(f, M, x, y); x += 0x20; }
    spr(f, R, x, y);
}

static void imgrec(const fe *f, uint32_t va, uint32_t out[5])
{
    int k;
    for (k = 0; k < 5; k++) out[k] = rd(f, va + 4u * (uint32_t)k);
}

/* ---- the item drawer, 0x0046E37D ---------------------------------------- */

static void draw_items(fe *f)
{
    fe_table *t = &f->tab[f->sc];
    int i;
    for (i = 0; i < t->n; i++) {
        fe_item *it = &t->it[i];
        int sel = f->cursor == it->id, x, y;
        uint32_t ir[5], rec;
        if (it->hidden) continue;                                
        imgrec(f, (uint32_t)it->img, ir);
        if (!sel) {
            if (it->kind != -0x57) {
                if (it->kind == -0x5B) {                         /* 0x465EB1 */
                    uint32_t c[3] = { ir[0], ir[1], ir[4] };
                    capsule_4654cf(f, c, it->tx, it->x, it->y, it->ty);
                } else if (it->kind == -0x59) {                  /* 0x465E29 */
                    capsule(f, ir[0], ir[1], ir[4],
                            measure(f, it->font4c, it->tok) + 0xC, it->x, it->y, 3);
                } else if (it->kind == -0x5A) {                  /* 0x465E4B */
                    uint32_t c[3] = { ir[0], ir[1], ir[4] };
                    capsule_4654cf(f, c, measure(f, it->font4c, it->tok) + 0xC,
                                   it->x, it->y, 1);
                } else if (it->kind != -0x56) {
                    spr(f, (uint32_t)it->img, it->x, it->y);
                }
            }
        } else if ((uint32_t)it->img != (uint32_t)-0x56 && it->kind != -0x57) {
            if (it->kind == -0x5B) {                             /* 0x465EB1 */
                capsule(f, ir[2], ir[3], ir[4], it->tx, it->x, it->y, it->ty);
            } else if (it->kind == -0x59) {                      /* 0x465E8F */
                capsule(f, ir[2], ir[3], ir[4],
                        measure(f, it->font4c, it->tok) + 0xC, it->x, it->y, 3);
            } else if (it->kind == -0x5A) {                      /* 0x465E6D */
                capsule(f, ir[2], ir[3], ir[4],
                        measure(f, it->font4c, it->tok) + 0xC, it->x, it->y, 1);
            } else {
                spr(f, (uint32_t)it->img, it->x, it->y);
            }
        }
        /* 0x0046E465: the text */
        x = it->x; y = it->y - 3;
        if (it->kind != -0x59 && it->kind != -0x5B && it->kind != -0x5A) {
            x = it->tx; y = it->ty;
        }
        if (it->kind == -0x59) x -= 4;
        rec = (uint32_t)it->font;
        if (sel)
            rec = blend_font(f, (uint32_t)it->font, it->font50,
                             f->itemflag == 1 ? 0u : f->now - f->key_ms);
        if (it->perglyph == 1) {                                 /* 0x46E512 */
            const char *s = tok_text(f, it->tok);
            char one[2];
            x -= 0x1A;
            for (; *s; s++) {
                one[0] = *s; one[1] = 0;
                text(f, rec, one, x, y);
                x += 0x12;
            }
        } else {
            draw_tok(f, rec, it->tok, x, y);
        }
    }
}

/* 0x0046E563 */
static void draw_labels(fe *f)
{
    fe_table *t = &f->tab[f->sc];
    int i;
    for (i = 0; i < t->nlab; i++)
        draw_tok(f, (uint32_t)t->lab[i][2], t->lab[i][3], t->lab[i][0], t->lab[i][1]);
}

/* 0x004661F4: the carousel's caption, from the cursor */
static void draw_caption(fe *f)
{
    static const uint32_t lists[6] = { CAP_KIND0, CAP_KIND1, CAP_KIND2, 0, 0, CAP_KIND5 };
    int w;
    if (f->carousel_kind < 0 || f->carousel_kind > 5 || !lists[f->carousel_kind]) return;
    snprintf(f->str[4], sizeof f->str[4], "%s",
             lexicon_text(&f->lx, (int)rd(f, lists[f->carousel_kind] +
                                              4u * (uint32_t)(f->cursor - 1))));
    w = measure(f, 0x0051834Cu, TOK_STR | 4) + 0xC;
    capsule(f, 0xF, 0x10, 0xB, w, 0x197, 0x1FE, 1);
    draw_tok(f, blend_font(f, 0x0051848Cu, 0x0051836Cu, f->now - f->key_ms),
             TOK_STR | 4, 0x195, 0x1FB);
}

/* ---- the screens' pre-draw handlers that write strings ------------------ */

/* 0x004664A1 / 0x004670C2 / the gearbox text 0x0046660E: car select */
static void pre_car(fe *f)
{
    uint32_t ent = drd(f, f->carlist + 4u * (uint32_t)f->car_index);
    snprintf(f->str[4], sizeof f->str[4], "%s", lexicon_text(&f->lx, (int)rd(f, ent + 4)));
    snprintf(f->str[5], sizeof f->str[5], "%s", lexicon_text(&f->lx, 0x21F + f->p_gear));
    snprintf(f->str[6], sizeof f->str[6], "%s %s", lexicon_text(&f->lx, 0x191 + 0),
             lexicon_text(&f->lx, 0x170));
}

/* FUN_0046F686: an itinerary entry to its stage record */
static uint32_t stage_record(const fe *f, uint32_t e)
{
    uint32_t idx = rd(f, e + 4), id = rd(f, e + 8), s = A_SETS;
    int guard = 0;
    while (rd(f, s + 4) != id) {
        s = rd(f, s);
        if (!s || ++guard > 64) return 0;        /* guru 0x004D54FF          */
    }
    return s + 0x34 + idx * 0x40;
}

static int ensure_vm(fe *f);
static int vm_run(fe *f, uint32_t va, const char *what);

/* 0x0047DB0E, the part that is ported: the three items' text, the direction
 * and the track.  The panel (times, length, surface, weather) is NOT. */
static void pre_stage(fe *f)
{
    fe_table *t = &f->tab[f->sc];
    uint32_t r = rd(f, rd(f, A_ITIN + 4u * (uint32_t)f->dir) + 4u * (uint32_t)f->rally);
    uint32_t e = r + 0xA4u * (uint32_t)f->stage, rec;
    t->it[0].tok = (int32_t)rd(f, A_DIRTOK + 4u * (uint32_t)f->dir);   /* 0x47DB30 */
    f->crsdirec = f->dir;                                               /* 0x47DB38 */
    t->it[1].tok = (int32_t)rd(f, r + 0x38);                            /* 0x47DB63 */
    rec = stage_record(f, e);
    /* str#3, 0x0047DB73..0x0047DBAE: token 0x482 ("ss"), 0x00401F15 a space,
     * 0x00401F51 (EAX = entry +0x40, EDX = 6) decimal '/' decimal,
     * 0x00401F1F ':', a space, then the stage record's name token */
    snprintf(f->str[3], sizeof f->str[3], "%s %d/6: %s", lexicon_text(&f->lx, 0x482),
             (int)rd(f, e + 0x40), rec ? lexicon_text(&f->lx, (int)rd(f, rec)) : "");
    if (rec) f->track = (int32_t)rd(f, rec + 0x34);                     /* 0x47DCDF */
    if (ensure_vm(f)) {
        vm_wr32(f->vm, 0x00707EF5, f->sc == SC_STAGE ? T_STAGE : T_TTSTAGE);
        vm_wr32(f->vm, 0x00708230, (uint32_t)f->dir);
        vm_wr32(f->vm, 0x00708238, (uint32_t)f->rally);
        vm_wr32(f->vm, 0x00708234, (uint32_t)f->stage);
        vm_run(f, 0x0047DB0E, "SELECT STAGE's panel");
        gfx2d_set_clip(&f->surf, 0, 0, W - 1, H - 1);
    }
}

/* ---- the car setup screen: the original's draw code, run in the VM -------- */

/* The hooks answer every call the draw code makes that leaves the draw layer
 * (x86vm.h).  Each is a stand-in for ONE original function, named. */

/* 0x00402311: ECX x, EDX y, EAX token, EBX font record */
static void vh_tok(vm *v, uint32_t r[8], void *ud)
{
    int y;
    (void)v;
    if (vm_y((fe *)ud, (int)r[VM_EDX], &y))
        draw_tok((fe *)ud, r[VM_EBX], (int32_t)r[VM_EAX], (int)r[VM_ECX], y);
}

/* 0x0040239C: ECX x, EDX y, EAX font record, EBX the string */
static void vh_text(vm *v, uint32_t r[8], void *ud)
{
    int y;
    if (vm_y((fe *)ud, (int)r[VM_EDX], &y))
        text((fe *)ud, r[VM_EAX], vm_cstr(v, r[VM_EBX]), (int)r[VM_ECX], y);
}

/* 0x004066F9: ECX x, EDX y, EAX sprite, ESI the bundle ([0x00707EED]); the
 * clip is the four globals 0x004F0010.. (INCLUSIVE, gfx2d.h) */
static void vh_blit(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    int y;
    if (!vm_y(f, (int)r[VM_EDX], &y)) return;
    gfx2d_set_clip(&f->surf, (int)vm_rd32(v, 0x004F0010), (int)vm_rd32(v, 0x004F0018),
                   (int)vm_rd32(v, 0x004F0014), (int)vm_rd32(v, 0x004F001C));
    gfx2d_blit(&f->surf, f->mbuts, f->mbuts_len, r[VM_EAX], NULL, (int)r[VM_ECX], y);
}

/* 0x004495D3: the original's flat translucent quad - it builds a four-vertex
 * polygon (x1 [0x5D6CA4], x2 [0x5D6CA8], y1 [0x5D6CB4], y2 [0x5D6CB8], colour
 * [0x5D6C7C] 0x00RRGGBB, alpha [0x5D6C80]) and hands it to the 3D layer, whose
 * blend was the DISPLAY CARD'S, not the game's - no instruction of the game
 * computes it.  THE PORT'S BLEND IS A STAND-IN, in two parts:
 *
 *   colour 0x11166A, alpha 0xEE (the four margin quads of the service area):
 *   the card's transfer function FITTED to run 25's snapshots - per channel,
 *   on the 5-6-5 values, out = (src * 69 + K) >> 10 with K = 1973 / 5150 /
 *   12616 for R / G / B.  (shift, slope) = (10, 69) is the one pair all three
 *   channels agree on; it reproduces the left margin's 70,200 channel values
 *   with 0 misses and every margin pixel of frame 001 to the bit.  A fit, not
 *   a derivation: no formula from (colour, alpha) gave it (B wants 108.7 where
 *   the game says 106); the plain linear blend leaves 6.8% of the channel
 *   values one step off.
 *
 *   any other colour or alpha (the panels' own, 0x4FCF / 0x7F00 at alpha 0xBA,
 *   and the opaque borders): that linear blend, which is exact for opaque
 *   quads and one step off in some of the panel interiors - 125,000 of frame
 *   001's 480,000 pixels, none by more than one step.
 *
 * owns the 3D layer; what the card did there is its question. */
static void vh_rect(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    int x1 = (int)vm_rd32(v, 0x5D6CA4), x2 = (int)vm_rd32(v, 0x5D6CA8);
    int y1 = (int)vm_rd32(v, 0x5D6CB4), y2 = (int)vm_rd32(v, 0x5D6CB8);
    uint32_t col = vm_rd32(v, 0x5D6C7C), al = vm_rd32(v, 0x5D6C80) & 0xFFu;
    int x, y, c;
    /* a channel's output depends only on its 5- or 6-bit value for
     * one quad, so it is tabled once per call - the per-pixel division was
     * ~90% of the service area's frame (6.7 ms on the host).  Same values. */
    uint32_t lut[3][64];
    (void)r;
    for (c = 0; c < 3; c++) {                               /* 0 = B, 1 = G, 2 = R */
        int n = c == 1 ? 6 : 5, m = (1 << n) - 1;
        uint32_t cc = (col >> (8 * c)) & 0xFFu, sv;
        static const uint32_t KB[3] = { 12616u, 5150u, 1973u };     /* B G R */
        for (sv = 0; sv <= (uint32_t)m; sv++) {
            uint32_t q;
            if (col == 0x0011166Au && al == 0xEEu)
                q = (sv * 69u + KB[c]) >> 10;
            else
                q = (sv * (255u - al) * 255u + cc * al * (uint32_t)m) / (255u * 255u);
            lut[c][sv] = ((q << (8 - n)) | (q >> (2 * n - 8))) << (8 * c);   /* widened by replication */
        }
    }
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > W) x2 = W;
    if (y2 > H) y2 = H;
    for (y = y1; y < y2; y++)
        for (x = x1; x < x2; x++) {
            uint32_t p = f->px[y * W + x];
            f->px[y * W + x] = 0xFF000000u | lut[0][(p & 0xFFu) >> 3]
                             | lut[1][((p >> 8) & 0xFFu) >> 2]
                             | lut[2][((p >> 16) & 0xFFu) >> 3];
        }
}

/* the flat fills, at the 16-bit depth the game runs at, into
 * the clip [0x4F0010..0x4F001C] (inclusive).  The colour is LUT[EAX] with
 * the LUT in ESI - always PAL_00, [0x5179D8], here, so the port's copy of it
 * is used.  (At 32 bits 0x004065B0 draws nothing: 0x004066B2 jumps to the
 * end - not modelled; the game ran at 16.) */
static void fill(fe *f, vm *v, int x, int y, int w, int h, uint32_t idx)
{
    int x0 = (int)vm_rd32(v, 0x004F0010u), x1 = (int)vm_rd32(v, 0x004F0014u);
    int y0 = (int)vm_rd32(v, 0x004F0018u), y1 = (int)vm_rd32(v, 0x004F001Cu);
    int i, j;
    uint32_t c = f->lut[idx & 0xFFu];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (y1 > H - 1) y1 = H - 1;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            if (i >= x0 && i <= x1 && j >= y0 && j <= y1) f->px[j * W + i] = c;
}

/* A routine left out (each use says why). */
static void vh_nop(vm *v, uint32_t r[8], void *ud)
{
    (void)v; (void)r; (void)ud;
}

/* 0x004065B0: EBX -> {x, y, w, h}, EAX the colour */
static void vh_fill(vm *v, uint32_t r[8], void *ud)
{
    uint32_t b = r[VM_EBX];
    fill((fe *)ud, v, (int)vm_rd32(v, b), (int)vm_rd32(v, b + 4),
         (int)vm_rd32(v, b + 8), (int)vm_rd32(v, b + 12), r[VM_EAX]);
}

/* 0x00406457: a row, ECX x, EDX y, EBX long; 0x004064F6: a column */
static void vh_hline(vm *v, uint32_t r[8], void *ud)
{
    fill((fe *)ud, v, (int)r[VM_ECX], (int)r[VM_EDX], (int)r[VM_EBX], 1, r[VM_EAX]);
}

static void vh_vline(vm *v, uint32_t r[8], void *ud)
{
    fill((fe *)ud, v, (int)r[VM_ECX], (int)r[VM_EDX], 1, (int)r[VM_EBX], r[VM_EAX]);
}

static void vm_put(vm *v, uint32_t r[8], const char *s)
{
    size_t i, n = strlen(s);
    for (i = 0; i < n; i++) vm_wr8(v, r[VM_EBX] + (uint32_t)i, (uint8_t)s[i]);
    r[VM_EBX] += (uint32_t)n;
}

/* 0x00401F8F: EAX a token, appended at EBX, EBX advanced (no NUL) */
static void vh_tokstr(vm *v, uint32_t r[8], void *ud)
{
    vm_put(v, r, tok_text((fe *)ud, (int32_t)r[VM_EAX]));
}

/* The number formatters (0x004019CD, 0x004019D7, 0x00401A25, 0x00401AFC, all
 * FUN_00402011's) are not hooked themselves; FUN_00402011 is (vh_fmt_num: fel_fmt_num, gated against the original's code), which was most
 * of what they cost. */

/* 0x004022D2: ECX = the width of the string at EBX in the MEASURE record at
 * EAX (the original calls FUN_0040239C with the record's own alignment set
 * to measure, and reads the width out of the record's +0x1C).  EDX is the
 * record's +8, the line height, loaded before that call (0x004022E3) and
 * returned: the box 0x00415A0C is EDX + 0x19 high (52 px in the
 * user's screenshot 175, 0x1B + 0x19). */
static void vh_measure(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    gfx2d_font *g = font(f, r[VM_EAX]), tmp;
    r[VM_EDX] = vm_rd32(v, r[VM_EAX] + 8u);
    if (!g || !g->bundle) { r[VM_ECX] = 0; return; }
    tmp = *g;
    tmp.field_w = 0;
    tmp.measured = 0;
    tmp.align = GFX2D_ALIGN_MEASURE;
    gfx2d_text(&f->surf, &tmp, vm_cstr(v, r[VM_EBX]), 0, 0);
    r[VM_ECX] = (uint32_t)tmp.field_w;
}

/* ----: the championship's hooks ------------------------------------
 *
 * The championship's helpers run in the interpreter.  Some of them END in a
 * screen (0x0049DC6C prepares OVERALL STAGE RESULTS' rows and calls its body
 * 0x004631EB); the port's screens are a frame loop, so each such body is a
 * hook that only records which screen was asked for (ch_req) - the caller
 * returns and frontend.c enters it.  The same for the launcher. */

static const uint32_t CH_BODY[][2] = {          /* the body, the screen */
    { 0x00462F22, SC_CHCLASS }, { 0x004631EB, SC_CHOVER },
    { 0x004634B4, SC_CHRCLASS }, { 0x0046377D, SC_CHRALLY },
    { 0x004A07E0, SC_CHCCLASS }, { 0x00463D3B, SC_CHCHAMP },
    { 0x0049FB6E, SC_CHBEST }, { 0x004A4444, SC_CHBESTCH },
    { 0x004A46EE, SC_CHBESTOV }, { 0x00479CB6, 0 /* INSPECTION AREA */ }
};

static void vh_body(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    (void)r;
    f->ch_req = vm_last_call(v);                 /* the body it stands in for */
    /* the INSPECTION AREA (0x00479CB6) is SC_CHINSPECT, entered from
     * its two callers in the flow (CH_SVC_DONE, CH_INSP), never through
     * here; a helper that still reaches it is reported (ch_show) */
}

/* 0x00480461, FUN_00480461 the launcher: EAX the player, EBX gmode */
static void launch_note(fe *f, vm *v, uint32_t gmode)
{
    if (gmode == 1) {
        /* the launcher copies player+0x288 to [0x70CF70] (the
         * championship's catitken) HERE; FUN_0046F785's last call,
         * 0x004A526C (0x0047017A), zeroes every player's +0x288 once the
         * launcher has returned, so the race must be handed the value of
         * THIS moment - read after the call it was always 0 */
        if (!f->ch_launch) f->ch_champ88 = (int32_t)vm_rd32(v, setup_player(v) + 0x288u);
        f->ch_launch = 1;
    }
}

static void vh_launch(vm *v, uint32_t r[8], void *ud)
{
    launch_note((fe *)ud, v, r[VM_EBX]);
}

/* a ported handler called INSIDE the interpreter (0x004775CB, by
 * 0x0046F785's launch): the C twin runs in its place.  Only for handlers that
 * leave every register as they found it (the sorts: PUSHAD ... POPAD, which
 * fe_logic_check verifies) and whose call site tests no flag. */
static void vh_native(vm *v, uint32_t r[8], void *ud)
{
    fel_fn fn = fel_lookup(vm_last_call(v));
    (void)r; (void)ud;
    if (fn) fn(v);
}

/* 0x004112A4, var\ral.cfg's keys into their words (ESI the text, every
 * register kept: PUSHAD ... POPAD).  The flags are those of its last compare,
 * the NUL that ends the key table with 0: ZF set, the rest clear.  The one
 * caller, 0x00411259, returns straight on. */
static void vh_cfg_keys(vm *v, uint32_t r[8], void *ud)
{
    (void)ud;
    fel_cfg_parse_keys(v, r[VM_ESI]);
    vm_set_zf(v, 1);
    vm_set_cf(v, 0);
}

/* 0x00403071, the numbered file name (EAX the template, EBX the index;
 * PUSHAD ... POPAD, then EAX = 0x008F8550).  The flags are those of the ADD ESP
 * in 0x00402011's tail: ZF, CF clear; no caller tests them. */
static void vh_numbered_path(vm *v, uint32_t r[8], void *ud)
{
    (void)ud;
    fel_numbered_path(v, r[VM_EAX], r[VM_EBX]);
    r[VM_EAX] = 0x008F8550u;
    vm_set_zf(v, 0);
    vm_set_cf(v, 0);
}

/* the game's generator.  0x004018A0 returns the new state in EAX (EBX
 * kept); 0x0040188E returns state mod EAX (ECX, EDX kept).  The original's
 * DIV leaves the flags undefined and no caller reads them.  A zero modulus
 * would be the original's #DE: the C answers 0. */
static void vh_rng_next(vm *v, uint32_t r[8], void *ud)
{
    (void)ud;
    r[VM_EAX] = fel_rng_next(v);
}

static void vh_rng_mod(vm *v, uint32_t r[8], void *ud)
{
    uint32_t out = 0;
    (void)ud;
    fel_rng_mod(v, r[VM_EAX], &out);
    r[VM_EAX] = out;
}

/* 0x00402011, the number formatter (EAX the value, ECX the flags and
 * width, EBX the destination, advanced; every other register kept).  The flags
 * are those of the ADD ESP in its tail: ZF, CF clear. */
static void vh_fmt_num(vm *v, uint32_t r[8], void *ud)
{
    (void)ud;
    r[VM_EBX] = fel_fmt_num(v, r[VM_EAX], r[VM_ECX], r[VM_EBX]);
    vm_set_zf(v, 0);
    vm_set_cf(v, 0);
}

/* 0x00478937's best-times seeding (fel_seed's seven routines,
 * PUSHAD ... POPAD: every register kept).  0x00401F8F inside them is this
 * file's token text, as vh_tokstr answers it.  ZF as the original leaves it,
 * CF clear (its last CMP); no caller tests them. */
static void seed_tokstr(void *ud, vm *v, int32_t tok, uint32_t dest)
{
    const char *s = tok_text((fe *)ud, tok);
    size_t i, n = strlen(s);
    for (i = 0; i < n; i++) vm_wr8(v, dest + (uint32_t)i, (uint8_t)s[i]);
}

static void vh_seed(vm *v, uint32_t r[8], void *ud)
{
    int zf = fel_seed(v, vm_last_call(v), r[VM_EDI], seed_tokstr, ud);
    vm_set_zf(v, zf == 1);
    vm_set_cf(v, 0);
}

/* 0x0040F566: the narration, EAX the path, ECX the volume; 0x0040F57A: its
 * stop; 0x0040C37C: ZF = still playing */
static void vh_speech(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    if (f->snd) snd_speech(f->snd, 2, vm_cstr(v, r[VM_EAX]), (int32_t)r[VM_ECX]);
}

/* 0x0047B0C2: a screen's art, EAX the path (the page of CAR SPECIFICATIONS,
 * reached by the arrows as well as the entry).  CF is its failure. */
static void vh_art(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    char err[256];
    if (load_art(f, vm_cstr(v, r[VM_EAX]), err, sizeof err)) {
        fprintf(stderr, "frontend: %s\n", err);
        vm_set_cf(v, 1);
    } else
        vm_set_cf(v, 0);
}

/* SLIDESHOW's three platform calls under 0x0046512D.  0x00402CA3: EAX a game path,
 * CF = 1 when there is no such file (it opens and closes it); 0x004115E1: EAX the picture,
 * EBX the title bar over it, CF = failure; 0x00411573: EAX the picture alone, CF = failure. */
static void vh_exists(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    char path[1024];
    vm_set_cf(v, vfs_resolve(&f->fs, vm_cstr(v, r[VM_EAX]), path, sizeof path) != 0);
}

static void vh_slide(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    char err[256], pic[512];
    snprintf(pic, sizeof pic, "%s", vm_cstr(v, r[VM_EAX]));       /* vm_cstr's buffer is one */
    if (load_slide(f, pic, vm_cstr(v, r[VM_EBX]), err, sizeof err)) {
        fprintf(stderr, "frontend: %s\n", err);
        vm_set_cf(v, 1);
    } else
        vm_set_cf(v, 0);
}

static void vh_slide_alone(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    char err[256];
    if (load_slide(f, vm_cstr(v, r[VM_EAX]), NULL, err, sizeof err)) {
        fprintf(stderr, "frontend: %s\n", err);
        vm_set_cf(v, 1);
    } else
        vm_set_cf(v, 0);
}

static void vh_speech_stop(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    (void)v; (void)r;
    if (f->snd) snd_speech(f->snd, 0, NULL, 0);
}

static void vh_speech_busy(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    (void)r;
    vm_set_zf(v, f->snd && f->snd->n_on);
}

/* 0x00407CDC, the key read: no key (ZF = 1, AL = 0) - the menus' keys are
 * frontend.c's */
static void vh_nokey(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    /* a slot's name being typed is handed its key (0x0046BE56) */
    r[VM_EAX] = (r[VM_EAX] & ~0xFFu) | (uint32_t)(f->dik & 0xFF);
    vm_set_zf(v, (f->dik & 0xFF) == 0);
    f->dik = 0;
}

/* ----: the platform under the save code ------------------------------
 *
 * The original's readers and writers (var\ral.dat, var\hst.dat, the slots)
 * run in the interpreter as written; these hooks are the Win32 calls under
 * them, at the file layer's own primitives (CF = 1 is
 * FAILURE).  "var\" and "savedata\" are the user's (vfs_set_user); a read
 * falls back to the install's own copy, a write never goes there. */

static int is_user_path(const char *p)
{
    const char *a = "var\\", *b = "save";
    size_t i;
    for (i = 0; a[i] && (p[i] | 0x20) == (a[i] | 0x20); i++) {}
    if (!a[i]) return 1;
    for (i = 0; b[i] && (p[i] | 0x20) == b[i]; i++) {}
    return !b[i];
}

/* Each directory of `path` below the user directory, made. */
static void make_dirs(const fe *f, const char *path)
{
    char d[1100];
    size_t i, from = f->fs.user[0] ? strlen(f->fs.user) + 1 : 0;
    snprintf(d, sizeof d, "%s", path);
    for (i = from; d[i]; i++)
        if (d[i] == '/') { d[i] = 0; plat_dir_create(d); d[i] = '/'; }
}

/* 0x00402BCD: EAX the game's path, EBX the mode (the table 0x004DB6D2: 0
 * read an existing file, 1 write a new one over any old, 2 write only a new
 * one, 3 both) -> EAX the handle, CF = failure (CreateFileA's -1). */
static void vh_fopen(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    char name[64], path[1100];
    int k, mode = (int)r[VM_EBX], user;
    plat_file *h = NULL;
    snprintf(name, sizeof name, "%s", vm_cstr(v, r[VM_EAX]));
    user = is_user_path(name);
    for (k = 0; k < 4 && f->fh[k]; k++) {}
    if (k < 4 && mode == 0) {
        int dat = !strcmp(name, "var\\ral.dat") || !strcmp(name, "var\\hst.dat");
        if (!(dat && f->no_read)) {
            if (vfs_resolve(&f->fs, name, path, sizeof path) == 0 ||
                (user && vfs_resolve(&f->inst, name, path, sizeof path) == 0))
                h = plat_file_open(path, 0);
        }
    } else if (k < 4 && (mode == 1 || mode == 2) && user && f->writes) {
        vfs_resolve_dir(&f->fs, name, path, sizeof path);
        make_dirs(f, path);
        if (!(mode == 2 && plat_file_exists(path))) h = plat_file_open(path, 1);
        if (!h) fprintf(stderr, "frontend: cannot write %s\n", path);
    } else if (k < 4 && mode != 0 && user && !f->writes && !f->said_nowrite) {
        fprintf(stderr, "frontend: %s is not written (the saves are off)\n", name);
        f->said_nowrite = 1;
    }
    if (!h) { r[VM_EAX] = 0xFFFFFFFFu; vm_set_cf(v, 1); return; }
    f->fh[k] = h;
    f->fwr[k] = mode != 0;
    snprintf(f->fname[k], sizeof f->fname[k], "%s", name);
    r[VM_EAX] = (uint32_t)k + 1u;
    vm_set_cf(v, 0);
}

static plat_file *fh_of(fe *f, uint32_t h)
{
    return h >= 1 && h <= 4 ? f->fh[h - 1] : NULL;
}

/* 0x00402C22: EAX the handle, EBX the buffer, ECX the count -> EAX the bytes
 * read, CF = ReadFile failed (a short read at the end is not a failure). */
static void vh_fread(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    plat_file *h = fh_of(f, r[VM_EAX]);
    uint8_t buf[4096];
    uint32_t got = 0, n = r[VM_ECX];
    if (!h) { vm_set_cf(v, 1); return; }
    while (got < n) {
        size_t want = n - got < sizeof buf ? n - got : sizeof buf, i;
        size_t k = plat_file_read(h, buf, want);
        for (i = 0; i < k; i++) vm_wr8(v, r[VM_EBX] + got + (uint32_t)i, buf[i]);
        got += (uint32_t)k;
        if (k < want) break;
    }
    r[VM_EAX] = got;
    vm_set_cf(v, 0);
}

/* 0x00402C3C: EAX the handle, EBX the buffer, ECX the count -> EAX the bytes
 * written, CF = WriteFile failed or wrote fewer (CMP EAX,ECX) */
static void vh_fwrite(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    plat_file *h = fh_of(f, r[VM_EAX]);
    uint8_t buf[4096];
    uint32_t put = 0, n = r[VM_ECX];
    if (!h) { vm_set_cf(v, 1); return; }
    while (put < n) {
        size_t want = n - put < sizeof buf ? n - put : sizeof buf, i;
        for (i = 0; i < want; i++) buf[i] = vm_rd8(v, r[VM_EBX] + put + (uint32_t)i);
        if (plat_file_write(h, buf, want) != want) break;
        put += (uint32_t)want;
    }
    r[VM_EAX] = put;
    vm_set_cf(v, put < n);
}

/* 0x00402C5A: EAX the handle, ECX the distance, EBX the method -> CF =
 * SetFilePointer's -1 */
static void vh_fseek(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    plat_file *h = fh_of(f, r[VM_EAX]);
    int64_t at = h ? plat_file_seek(h, (int64_t)(int32_t)r[VM_ECX], (int)r[VM_EBX]) : -1;
    vm_set_cf(v, at < 0);
}

/* 0x00402C08: EAX the handle -> CF = CloseHandle failed */
static void vh_fclose(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    uint32_t k = r[VM_EAX];
    plat_file *h = fh_of(f, k);
    if (!h) { vm_set_cf(v, 1); return; }
    if (f->fwr[k - 1]) printf("frontend: %s written\n", f->fname[k - 1]);
    plat_file_close(h);
    f->fh[k - 1] = NULL;
    vm_set_cf(v, 0);
}

/* 0x004028FF: VirtualAlloc(0, EAX, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
 * -> EAX the pages, zeroed; CF = it failed.  The interpreter's heap, a page
 * at a time, never given back (0x00402920, VirtualFree, is a no-op): the
 * original allocates these blocks once a session (0x004787D0's guard). */
static void vh_valloc(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    uint32_t n = r[VM_EAX], i;
    if (!n) { vm_set_cf(v, 0); return; }
    n = (n + 0xFFFu) & ~0xFFFu;
    if (f->heap_top + n > A_HEAP_END) {
        fprintf(stderr, "frontend: the interpreter's heap is full (0x%X more)\n", n);
        r[VM_EAX] = 0;
        vm_set_cf(v, 1);
        return;
    }
    r[VM_EAX] = f->heap_top;
    for (i = 0; i < n; i += 4) vm_wr32(v, f->heap_top + i, 0);
    f->heap_top += n;
    vm_set_cf(v, 0);
}

/* 0x00410D81: GetLocalTime packed as has it - year 12
 * bits, month 4, day 5, hour 5, minute 6 */
static void vh_stamp(vm *v, uint32_t r[8], void *ud)
{
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    (void)v; (void)ud;
    r[VM_EAX] = lt ? ((((((uint32_t)(lt->tm_year + 1900) << 4 | (uint32_t)(lt->tm_mon + 1))
                         << 5 | (uint32_t)lt->tm_mday) << 5 | (uint32_t)lt->tm_hour) << 6)
                      | (uint32_t)lt->tm_min) : 0u;
}

/* 0x00410642 GetUserNameA / 0x00410630 GetComputerNameA: failed (CF = 1),
 * the buffer untouched - the boot then names the players from the lexicon
 * (0x0041104B: token 0x18C..0x18F) */
static void vh_noname(vm *v, uint32_t r[8], void *ud)
{
    (void)r; (void)ud;
    vm_set_cf(v, 1);
}

static void vh_model(vm *v, uint32_t r[8], void *ud);
static uint32_t nat_c_svc(void *ud, vm *v, uint32_t va, uint32_t r[8]);
static int ensure_vm(fe *f)
{
    if (f->vm) return 1;
    f->vm = vm_new(&f->im);
    if (!f->vm) return 0;
    {   /* fd_career's platform services are the hooks below */
        static fdc_host ch = { nat_c_svc, NULL };
        fdc_set_host(&ch);
    }
    vm_hook(f->vm, 0x00402311, vh_tok, f);
    vm_hook(f->vm, 0x0040239C, vh_text, f);
    vm_hook(f->vm, 0x004066F9, vh_blit, f);
    vm_hook(f->vm, 0x00401F8F, vh_tokstr, f);
    vm_hook(f->vm, 0x004495D3, vh_rect, f);
    vm_hook(f->vm, 0x004022D2, vh_measure, f);
    vm_hook(f->vm, 0x004065B0, vh_fill, f);              
    vm_hook(f->vm, 0x00406457, vh_hline, f);
    vm_hook(f->vm, 0x004064F6, vh_vline, f);
    {   
        size_t k;
        for (k = 0; k < sizeof CH_BODY / sizeof CH_BODY[0]; k++)
            vm_hook(f->vm, CH_BODY[k][0], vh_body, f);
        /* the INSPECTION AREA's panel slides (0x0049D846 in,
         * 0x0049D981 out, 0x0049DA2C the failed car's) are timed frame
         * loops into the 3D layer, which stop the interpreter
         * (0x0044C5C7); NOT PORTED, so skipped - FUN_004A412F's own tests
         * then run to their end.  The screen 0x00479CB6 is vh_body's,
         * which leaves [0x707BC0] at 0: the player did not retire there. */
        vm_hook(f->vm, 0x0049D846, vh_nop, f);
        vm_hook(f->vm, 0x0049D981, vh_nop, f);
        vm_hook(f->vm, 0x0049DA2C, vh_nop, f);
        vm_hook(f->vm, 0x00480461, vh_launch, f);
        vm_hook(f->vm, 0x004775CB, vh_native, f);       /* 0x0046F785's sort */
        vm_hook(f->vm, 0x004112A4, vh_cfg_keys, f);     /* ral.cfg's parser */
        vm_hook(f->vm, 0x00403071, vh_numbered_path, f); /* the numbered names */
        vm_hook(f->vm, 0x004018A0, vh_rng_next, f);      /* the game's generator */
        vm_hook(f->vm, 0x0040188E, vh_rng_mod, f);
        vm_hook(f->vm, 0x00402011, vh_fmt_num, f);       /* the number formatter */
        for (k = 0; k < (size_t)fel_seed_count(); k++)       /* the best-times seeding */
            vm_hook(f->vm, fel_seed_va((int)k), vh_seed, f);
        vm_hook(f->vm, 0x0040F566, vh_speech, f);
        vm_hook(f->vm, 0x0040F57A, vh_speech_stop, f);
        vm_hook(f->vm, 0x0040C37C, vh_speech_busy, f);
        vm_hook(f->vm, 0x00407CDC, vh_nokey, f);
        /* the log (its lock 0x004012F6 spins), as setup_check's interpreter
         * does: the championship's helpers log as they go (0x00460F79..) */
        {
            static const uint32_t logs[] = { 0x004012F6, 0x004013B7, 0x004013CA,
                0x00401413, 0x00401417, 0x0040141E, 0x00401425, 0x0040142C,
                0x0040145E, 0x00401462, 0x00401469, 0x004015BB, 0x004015C2,
                0x00401612, 0x00401FE4, 0x004020E4,
                0x00410DF4, 0x00410DE6 };               /* and the two sleeps */
            size_t i;
            for (i = 0; i < sizeof logs / sizeof logs[0]; i++)
                vm_hook(f->vm, logs[i], vh_nop, f);
        }
        /* the save code's platform - and 0x004787D0's blocks (the
         * stage results [0x708054], the standings [0x708058], the opponents
         * [0x70805C], hst.dat's six) are now the original's own allocation,
         * at the main menu's entry (fe_load_saves) */
        vm_hook(f->vm, 0x00402BCD, vh_fopen, f);
        vm_hook(f->vm, 0x00402C22, vh_fread, f);
        vm_hook(f->vm, 0x00402C3C, vh_fwrite, f);
        vm_hook(f->vm, 0x00402C5A, vh_fseek, f);
        vm_hook(f->vm, 0x00402C08, vh_fclose, f);
        vm_hook(f->vm, 0x004028FF, vh_valloc, f);
        vm_hook(f->vm, 0x00402920, vh_nop, f);
        vm_hook(f->vm, 0x00410D81, vh_stamp, f);
        vm_hook(f->vm, 0x00410642, vh_noname, f);
        vm_hook(f->vm, 0x00410630, vh_noname, f);
        f->heap_top = A_HEAP;
    }
    vm_wr32(f->vm, 0x004EF974, W);                       /* the screen's size */
    vm_wr32(f->vm, 0x004EF978, H);
    vm_wr32(f->vm, 0x00707EED, 1);                       /* the bundle: mbuts */
    vm_wr32(f->vm, 0x004F0010, 0);   vm_wr32(f->vm, 0x004F0014, W - 1);
    vm_wr32(f->vm, 0x004F0018, 0);   vm_wr32(f->vm, 0x004F001C, H - 1);
    if (f->m3d) {
        /* FUN_00424997's store (0x004249A6) for every type with a
         * model, so FUN_00420F8A finds it; and the model draw is ours */
        int nt, t;
        const uint32_t *mod = menu3d_models(f->m3d, &nt);
        for (t = 1; t < nt; t++)
            if (mod[t]) vm_wr32(f->vm, 0x00581FC4u + 4u * (uint32_t)t, mod[t]);
        vm_hook(f->vm, 0x0048C4AC, vh_model, f);
    }
    return 1;
}

static void save_dat(fe *f);
static int su_slots_enter(fe *f);
static void ch_slots_enter(fe *f);
static void vm_draw(fe *f, uint32_t va, const char *what);

/* batch 5: the draw layer the native handlers (fe_logic.c) draw through -
 * the two functions the VM's hooks 0x00402311 / 0x0040239C stand in for */
static void fd_tok(void *ud, uint32_t font, int32_t token, int x, int y)
{
    draw_tok((fe *)ud, font, token, x, y);
}

static void fd_text(void *ud, uint32_t font, uint32_t str, int x, int y)
{
    fe *f = (fe *)ud;
    text(f, font, vm_cstr(f->vm, str), x, y);
}

/* batch 7: the platform services the native handlers end in - what the
 * VM's hooks 0x00409A8B / 0x0040F651 / 0x0040EE7C / 0x0040F566 / 0x00410E0C
 * (opt_hooks, vh_speech) stand in for.  The stage map 0x00465011 is the
 * caller's (load_stage_art, after the arrow), as it is with the original's
 * code, where the call is a no-op hook. */
static void fe_snd(fe *f, uint32_t mask);
static uint32_t fd_svc(void *ud, uint32_t va, uint32_t eax, uint32_t ebx, uint32_t ecx,
                       uint32_t edx, uint32_t ebp)
{
    fe *f = (fe *)ud;
    (void)edx; (void)ebp;
    switch (va) {
    case 0x00401F8F: {          /* the token's text at EBX: its length back, the NUL is the caller's */
        const char *t = tok_text(f, (int32_t)eax);
        size_t i, n = strlen(t);
        for (i = 0; i < n; i++) vm_wr8(f->vm, ebx + (uint32_t)i, (uint8_t)t[i]);
        return (uint32_t)n;
    }
    case 0x00409A8B: if (f->snd) snd_overall(f->snd, (int32_t)eax); break;
    case 0x0040F651: if (f->snd) snd_music_volume(f->snd, (int32_t)eax); break;
    case 0x0040EE7C: fe_snd(f, eax); break;
    case 0x0040F566: if (f->snd) snd_speech(f->snd, 2, vm_cstr(f->vm, eax), (int32_t)ecx); break;
    case 0x00410E0C: return f->now;
    default: break;
    }
    return eax;
}

/* the championship's driver (0x0046F785) and the loop after a retirement
 * (0x0046E580) are C (fe_champ.c, gated against the original's code).  The
 * driver's platform services are the ones the VM's hooks stood in for: the
 * token text, the launcher's note (launch_note), and the two engine calls
 * 0x0047D4A3 makes, which are NOT hooked here - the original's code runs them,
 * as it did inside the interpreter.  Returns -1 when the va is not one of them,
 * else 0 done / 1 the original's fatal exit (the memory is not meaningful). */
static uint32_t fd_champ_svc(void *ud, uint32_t va, uint32_t eax, uint32_t ebx,
                             uint32_t ecx, uint32_t edx)
{
    fe *f = (fe *)ud;
    switch (va) {
    case 0x00401F8F: return fd_svc(ud, va, eax, ebx, ecx, edx, 0);
    case 0x00480461: launch_note(f, f->vm, ebx); return 0;
    case 0x00440F60: case 0x004713BA:
        /* the 3D engine's world clear / set-up (it fills the engine's own tables: a 64 K shuffled
         * word table at 0x00A0BF54, the trig table at 0x0057C378, the object lists), reached only when
         * [0x0070AB50] == 1 - which the original's LAUNCHER (0x00480461: [0x70AB50] = EAX) stores, and
         * which this port's launcher hook (launch_note) does not, so it is 0 and this is never called
         * (measured 2026-11-10: the image's only writer of the word is 0x00480463).  Run in the interpreter
         * it STOPS at its first 16-bit store (0x0041E3E9), i.e. it has never had an effect here; nothing the
         * port reads lives in the engine's tables (the menus' 3D models are the port's own, menu3d.c).  So
         * it is nothing, as it is inside the finish routines (FIN_NOP, the user's decision of 2026-10-20). */
        return 0;
    default: return 0;
    }
}

static int fe_champ_call(fe *f, uint32_t va)
{
    fec_services sv;
    int rc;
    if (va != 0x0046F785 && va != 0x0046E580) return -1;
    sv.svc = fd_champ_svc;
    sv.ud = f;
    rc = va == 0x0046F785 ? fec_46f785(f->vm, &sv) : fec_46e580(f->vm);
    if (rc != 0 && !f->vm_bad) {
        fprintf(stderr, "frontend: FUN_%08X stopped: the original's fatal exit %d\n", va, rc);
        f->vm_bad = 1;
    }
    return rc != 0 ? 1 : 0;
}

/* the four families' twins (fd_service / fd_stage /
 * fd_career / fd_engine), each gated against the original's code.  Every
 * platform service they reach is answered by the hook that stands in for it
 * in the VM (vm_invoke: nothing runs where nothing is hooked, which is what
 * the engine's set-up and teardown calls did under the interpreter too).  So
 * one callback serves all four. */
static void nat_call(void *ud, uint32_t va, uint32_t r[8])
{
    fe *f = (fe *)ud;
    /* the best-time rows: the host hooks them to nothing when hst.dat's block is not
     * loaded, else they run - as C since (fd_svcdraw), over this same layer */
    if (!vm_invoke(f->vm, va, r) && (va == 0x0047091Eu || va == 0x00470AFAu)) {
        fdsg_draw d;
        d.call = nat_call;
        d.ud = f;
        fdv_run_edi(va, f->vm, &d, r[VM_EDI]);
    }
}

static uint32_t nat_c_svc(void *ud, vm *v, uint32_t va, uint32_t r[8])
{
    (void)ud;
    /* nothing here runs in the interpreter.  The slot files (reads 0x004A7041 / 0x004A6E82, writers 0x004A709B /
     * 0x004A6EDC; CF = failure) are C over the file layer's services (fd_career); the car setup pickers' change handlers,
     * entered with EBX = 1 (0x0046857A / 0x004689A4 / 0x00468802 / 0x004686CC, the item is the cursor global the
     * caller set), are setup.c's.  The arcade's start 0x004A60A7 and the championship's 0x0049CB25 are the hooks
     * ch_slots_select installs while a load runs (vh_chcalled); the arcade's flow (a loop over some forty screens) is
     * NOT PORTED, so with no hook it does nothing, as an unhooked service does. */
    if (fdc_is_slot_file(va)) {
        uint32_t fl = fdc_slot_file(v, va, r);
        vm_set_cf(v, (fl & FDC_CF) != 0);
        return fl;
    }
    switch (va) {
    case 0x0046857Au: setup_picker_confirm(v, 14); return 0;
    case 0x004689A4u: setup_picker_confirm(v, 15); return 0;
    case 0x00468802u: setup_picker_confirm(v, 18); return 0;
    case 0x004686CCu: setup_picker_confirm(v, 19); return 0;
    default: break;
    }
    vm_invoke(v, va, r);
    return (vm_cf(v) ? FDC_CF : 0u) | (vm_zf(v) ? FDC_ZF : 0u);
}

static void nat_b_svc(void *ud, vm *v, uint32_t va, uint32_t r[8])
{
    (void)ud;
    vm_invoke(v, va, r);
}

static int fe_native(fe *f, uint32_t va, uint32_t regs[8], int *rc);

static int nat_e_svc(void *ud, uint32_t va, uint32_t r[8])
{
    fe *f = (fe *)ud;
    if (va == 0x00415A0Cu) {     /* the COMING SOON box: not a hook, a draw routine (a twin, else the original's) */
        int nrc;
        if (!fe_native(f, va, r, &nrc)) orig_call(f->vm, va, r);
        return 0;
    }
    vm_invoke(f->vm, va, r);
    return vm_cf(f->vm);
}

/* RALLY_NO_TWIN=0xADDR,0xADDR,..: those handlers run as the original's code in
 * the interpreter again, whatever twin answers them (the bisect that finds which
 * twin changed a picture; "*" turns them all off: the pixel comparison of).  Debug only; unset in the
 * shipped game. */
static int no_twin(uint32_t va)
{
#ifndef RALLY_INTERP
    (void)va;                  /* with no interpreter a twin cannot be turned off */
    return 0;
#else
    static int init;
    static uint32_t list[64];
    static int n, all;
    int k;
    if (!init) {
        const char *e = getenv("RALLY_NO_TWIN");
        init = 1;
        if (e && *e == '*') all = 1;                /* every twin off */
        while (e && *e && n < 64) {
            char *end;
            unsigned long x = strtoul(e, &end, 16);
            if (end == e) break;
            list[n++] = (uint32_t)x;
            e = *end ? end + 1 : end;
        }
    }
    if (all) return 1;
    for (k = 0; k < n; k++) if (list[k] == va) return 1;
    return 0;
#endif
}

/* 1 when a twin answered (*rc: 0 done, -1 the original would have stopped);
 * regs may be NULL (no register in, as vm_run has none) */
static int fe_native(fe *f, uint32_t va, uint32_t regs[8], int *rc)
{
    vm *v = f->vm;
    uint32_t z[8] = { 0 };
    uint32_t *r = regs ? regs : z;
    fdsg_draw d;
    fds_fn ns;
    fdc_fn nc;
    fde_fn ne;
    fdb_fn nb;
    int k, c;

    if (no_twin(va)) return 0;
    if ((ns = fds_lookup(va)) != NULL) { *rc = ns(v, r) ? -1 : 0; return 1; }
    d.call = nat_call;
    d.ud = f;
    c = fdv_run(va, v, &d);                    /* the service area's post-draw */
    if (c != FDSG_NOT_MINE) { *rc = c == FDSG_OK ? 0 : -1; return 1; }
    for (k = 0; k < fdsg_edi_count(); k++)
        if (fdsg_edi_va(k) == va) {
            *rc = fdsg_run_edi(va, v, &d, r[VM_EDI]) == FDSG_OK ? 0 : -1;
            return 1;
        }
    c = fdsg_run(va, v, &d);
    if (c != FDSG_NOT_MINE) { *rc = c == FDSG_OK ? 0 : -1; return 1; }
    c = fdq_run(va, v, &d, r);                 /* CAR SPECIFICATIONS */
    if (c != FDSG_NOT_MINE) { *rc = c == FDSG_OK ? 0 : -1; return 1; }
    if ((nb = fdb_lookup(va)) != NULL) {
        fdb_host hb;
        hb.svc = nat_b_svc;
        hb.ud = f;
        *rc = nb(v, &hb, r) == FDS_OK ? 0 : -1;
        return 1;
    }
    if (va == 0x004680B9u) { *rc = fdc_4680b9(v, r[VM_EDI]) == FDC_FATAL ? -1 : 0; return 1; }
    if (fdc_is_typing(va)) { *rc = fdc_typing(v, va, r) == FDC_FATAL ? -1 : 0; return 1; }   
    if ((nc = fdc_lookup(va)) != NULL) { *rc = nc(v) == FDC_FATAL ? -1 : 0; return 1; }
    if ((ne = fde_lookup(va)) != NULL) {
        fde_services sv;
        sv.svc = nat_e_svc;
        sv.ud = f;
        c = 0;
        *rc = ne(v, r, &c, &sv) == FDE_OK ? 0 : -1;
        if (c >= 0) vm_set_cf(v, c);
        return 1;
    }
    return 0;
}

/* Run original code: said once, and the screen carries on without it. */
static int vm_run(fe *f, uint32_t va, const char *what)
{
    static fel_draw fd = { fd_tok, fd_text, fd_svc, NULL };
    fel_fn native;
    if (!ensure_vm(f)) return -1;
    fd.ud = f;
    fel_set_draw(&fd);
    if (!no_twin(va) && (native = fel_lookup(va)) != NULL) {     /* this one is C now */
        native(f->vm);
        return 0;
    }
    if (va == 0x0046F785 || va == 0x0046E580)    /* and so are these */
        return fe_champ_call(f, va) == 0 ? 0 : -1;
    {
        int nrc;
        if (fe_native(f, va, NULL, &nrc)) {      /* the four families' twins */
            if (nrc != 0) {
                if (!f->vm_bad)
                    fprintf(stderr, "frontend: %s (0x%08X): the original would have stopped\n",
                            what, va);
                f->vm_bad = 1;
                return -1;
            }
            return 0;
        }
    }
    if (orig_call(f->vm, va, NULL) != 0) {
        if (!f->vm_bad)
            fprintf(stderr, "frontend: %s (0x%08X) could not run: %s\n", what, va,
                    orig_error(f->vm));
        f->vm_bad = 1;
        return -1;
    }
    return 0;
}

/*/95: every host call into the original's code goes through here, so a
 * handler that has a C twin (fe_logic.c) never reaches the interpreter, whoever
 * calls it and with whatever registers: fe_logic_check runs the original with
 * none, so a twin reads none.  Returns vm_call's: 0 done. */
static int fe_vmcall(fe *f, uint32_t va, uint32_t regs[8])
{
    static fel_draw fd = { fd_tok, fd_text, fd_svc, NULL };
    fel_fn native;
    fd.ud = f;
    fel_set_draw(&fd);
    if (!no_twin(va) && (native = fel_lookup(va)) != NULL) {
        native(f->vm);
        return 0;
    }
    if (va == 0x0046F785 || va == 0x0046E580)    
        return fe_champ_call(f, va);
    {
        int nrc;
        if (fe_native(f, va, regs, &nrc)) return nrc;    /* the four families' twins */
    }
    return orig_call(f->vm, va, regs);
}

/* The fields the menus keep natively, into the player record the original's
 * code reads and writes (record [0x708DE8 + [0x7082F8]*4]). */
static void su_sync_player(fe *f)
{
    uint32_t P = setup_player(f->vm);
    vm_wr32(f->vm, P + 0x08, (uint32_t)f->p_car);
    vm_wr32(f->vm, P + 0x30, (uint32_t)f->p_gear);
    vm_wr32(f->vm, P + 0x90, (uint32_t)f->p_90);
}

/* FUN_0046F209, the new career, run as the original wrote it: the four
 * players' wear and flags cleared and - unless [0x708038] is 3, time trial's
 * - the setup sliders set to tyre 3, brake balance 50, height 70, stiffness
 * 30, steering = +0x280, gearing 2.  (Its native port is not worth writing
 * yet: nothing outside the setup screen reads what it clears.) */
static void new_career(fe *f, int mode)
{
    if (!ensure_vm(f)) return;
    vm_wr32(f->vm, 0x00708038, (uint32_t)mode);
    vm_wr32(f->vm, 0x007082F8, 0);
    su_sync_player(f);
    vm_run(f, 0x0046F209, "FUN_0046F209 (the new career)");
    vm_wr32(f->vm, 0x007082F8, 0);   /* its loop leaves the player index at 4; the callers store 0 */
}

/* The sliders the launcher will copy (0x004809FB..) for this player. */
static void su_read_sliders(fe *f)
{
    if (!ensure_vm(f)) return;
    setup_get_sliders(f->vm, &f->su_sl);
    f->su_valid = 1;
}

/* FUN_00489D64 and the screen's own entry (0x0046EEA9): the state before the
 * first frame. */
/* the service area's picture, [0x70C52E + car * 4] (0x0046DBA8) */
static int setup_art(fe *f, uint32_t car, char *err, size_t n)
{
    const char *path = rdstr(f, rd(f, A_SERVART + 4u * car));
    uint8_t *buf;
    size_t len;
    unsigned w, h;
    int rc;
    if (load_file(f, path, &buf, &len, err, n)) return -1;
    rc = png_decode(buf, len, f->bg, W * H, &w, &h, err, n);
    free(buf);
    return rc ? -1 : 0;
}

static int setup_enter(fe *f, char *err, size_t n)
{
    uint32_t P, car;
    if (!ensure_vm(f)) { snprintf(err, n, "out of memory"); return -1; }
    if (f->mode == 1) {
        /* FUN_0049D26A: the championship's own budget - the service
         * halt's (+0x14 of its itinerary entry) plus 0x384, which ch_service
         * has added - and its own career (no new one here) */
        P = setup_player(f->vm);
        car = vm_rd32(f->vm, P + 0x08);
        setup_begin(f->vm, vm_rd32(f->vm, 0x007082A4));
        printf("frontend: the championship's service area, budget %u s\n",
               (unsigned)vm_rd32(f->vm, 0x007082A4));   /* rally-menus reads it */
    } else {
        vm_wr32(f->vm, 0x007082BC, 3);                   /* the menu's mode    */
        vm_wr32(f->vm, 0x007082F0, 1);                   /* one player         */
        vm_wr32(f->vm, 0x007082F4, 0x14);
        vm_wr32(f->vm, 0x007082A0, 1);
        new_career(f, 3);                                /* 0x00486EA8         */
        P = setup_player(f->vm);
        car = vm_rd32(f->vm, P + 0x08);
        setup_begin(f->vm, SU_TT_BUDGET);                /* 0x00489D65         */
    }
    setup_costs(f->vm);                                  /* 0x00489DB1         */
    setup_panels_from_player(f->vm);                     /* 0x0046D371         */
    vm_wr32(f->vm, 0x0070CA48, 1);                       /* 0x0046F02D         */
    if (f->snd) snd_cue_car_setup(f->snd);               /* 0x0046EF6F: 9      */
    return setup_art(f, car, err, n);
}

/* RESULTS: the pre-draw handler FUN_0048229E draws everything but the title:
 * the stage, the time, the top speed, the player and the car's picture. */
static void results_draw(fe *f)
{
    vm_wr32(f->vm, 0x00708230, (uint32_t)f->dir);
    vm_wr32(f->vm, 0x00708238, (uint32_t)f->rally);
    vm_wr32(f->vm, 0x00708234, (uint32_t)f->stage);
    vm_wr32(f->vm, 0x007082F8, 0);
    vm_run(f, 0x0048229E, "RESULTS' draw");
    gfx2d_set_clip(&f->surf, 0, 0, W - 1, H - 1);
}

static void setup_draw(fe *f)
{
    vm_wr32(f->vm, 0x00707E91, (uint32_t)f->cursor);
    /* FUN_0047A8E9, the frame's blink: [0x708310] is 1 for the first 256 ms of
     * each 512 after the last key (0x0047A918), unless [0x708014] is 1; the
     * cursor's frame is drawn only while it is */
    vm_wr32(f->vm, 0x00708310,
            (f->itemflag != 1 && !((f->now - f->key_ms) & 0x100u)) ? 1u : 0u);
    /* RALLY_SVC_USED, a DIAGNOSTIC, not the original's: the
     * seconds the championship's service has spent ([0x70CBF4], the sum of
     * the chosen repairs' times, FUN_0046868c), put in before the draw so
     * that the draw's own code (FUN_004A1A56: [0x70CC28]) and the flow's
     * (0x0049D36C: P+0x288 = [0x70CC28] x 100) make the penalty, without
     * a car worn enough to buy that much repair */
    if (f->mode == 1 && getenv("RALLY_SVC_USED"))
        vm_wr32(f->vm, 0x0070CBF4, (uint32_t)atoi(getenv("RALLY_SVC_USED")));
    /* the draw list, 0x0070B3A6: FUN_0048FD81 - the margins' translucent
     * quads; then the post-draw list, 0x0070B56E: FUN_004A1C11 */
    if (vm_run(f, 0x0048FD81, "the service area's quads") != 0) return;
    if (vm_run(f, 0x004A1C11, "the service area's draw") != 0) return;
    gfx2d_set_clip(&f->surf, 0, 0, W - 1, H - 1);
}

/* ---- the 3D models (menu3d.h) ----------- */

/* Each screen body's carousel kind ([0x70C9E0] before CALL 0x0048C1A2) and
 * projection ([0x5D6E44], [0x5D6E48], [0x5D6F14] before CALL 0x0046C45F), and
 * the draw-list entry its table names (0x0048BDCB for the carousels and car
 * select, 0x0048BD13 for REVIEW OPTIONS) - entered past its Z clear and
 * texture residency at the call it ends on.  kind -1: no draw list. */
static const struct {
    int32_t kind, ox, oy, zoom;
    uint32_t draw;
    int carousel;                 /* [0x707E58] left at 1 by the body */
} M3D[SC_COUNT] = {
    {  0,    7,  10, 0x100, 0x0048BDF1, 1 }, /* MAIN    0x00470FA2..0x00470FFC */
    {  1,    7,  10, 0x100, 0x0048BDF1, 1 }, /* SRAL    0x00471F3F..0x00471F72 */
    {  2,    7,  10, 0x100, 0x0048BDF1, 1 }, /* PLAYERS 0x0047D81C..0x0047D84F */
    { 13,    7, -50, 0x100, 0x0048BDF1, 0 }, /* CAR     0x00476F34..0x00476F8A */
    { -1,    0,   0,     0, 0,          0 }, /* SELECT STAGE: no draw list     */
    {  2,    7,  10, 0x100, 0x0048BDF1, 1 }, /* TT PLAYERS 0x00487CC9..0x00487D00 */
    { -1,    0,   0,     0, 0,          0 },
    { -1,    0,   0,     0, 0,          0 }, /* SERVICE AREA: 0x0048FD81, not a model */
    { -1,    0,   0,     0, 0,          0 }, /* RESULTS: no draw list          */
    { 15, -156,  39, 0x140, 0x0048BD34, 0 }, /* REVIEW  0x00481F82..0x00481FCE */
    /* the arcade */
    {  5,    7,  10, 0x100, 0x0048BDF1, 1 }, /* SELECT LEVEL 0x004839A2..0x004839D9 */
    { -1,    0,   0,     0, 0,          0 }, /* NEXT STAGE: no draw list       */
    { -1,    0,   0,     0, 0,          0 }, /* RESULTS                        */
    { -1,    0,   0,     0, 0,          0 }, /* RESULTS, lost                  */
    { -1,    0,   0,     0, 0,          0 }, /* BEST TIMES                     */
    { -1,    0,   0,     0, 0,          0 }, /* TOP SCORES                     */
    { 15, -156,  39, 0x140, 0x0048BD34, 0 }, /* continue 0x0048496B..0x004849B7 */
    { 16, -156,  39, 0x140, 0x0048BD34, 0 }, /* next level 0x004842FF..0x0048434B */
    { 16, -156,  39, 0x140, 0x0048BD34, 0 }, /* restart 0x0048463A..0x00484686 */
    { 14,    7, -50, 0x100, 0x0048BDF1, 0 }, /* bonus car 0x00477254..0x004772BE */
    /* the championship */
    {  2,    7,  10, 0x100, 0x0048BDF1, 1 }, /* NEW SIMULATION 0x00472813..0x0047285A */
    { -1,    0,   0,     0, 0,          0 }, /* the welcome picture            */
    { -1,    0,   0,     0, 0,          0 }, /* WELCOME: no draw list          */
    { -1,    0,   0,     0, 0,          0 }, /* LEG DETAILS                    */
    { -1,    0,   0,     0, 0,          0 }, /* NEXT STAGE                     */
    { -1,    0,   0,     0, 0,          0 }, /* the results ...                */
    { -1,    0,   0,     0, 0,          0 },
    { -1,    0,   0,     0, 0,          0 },
    { -1,    0,   0,     0, 0,          0 },
    { -1,    0,   0,     0, 0,          0 },
    { -1,    0,   0,     0, 0,          0 },
    { -1,    0,   0,     0, 0,          0 }, /* BEST TIMES                     */
    { -1,    0,   0,     0, 0,          0 },
    { -1,    0,   0,     0, 0,          0 },
    { 10, -156,  39, 0x140, 0x0048BD34, 0 }, /* REVIEW 0x00479682..0x004796D3  */
    { 19, -156,  39, 0x140, 0x0048BD34, 0 }, /* REVIEW 0x0047D13E..0x0047D18F  */
    { 10, -156,  39, 0x140, 0x0048BD34, 0 }, /* REVIEW 0x0047CDF6..0x0047CE47  */
    { 20, -156,  39, 0x140, 0x0048BD34, 0 }, /* REVIEW 0x0047D4D9..0x0047D52A  */
    { -1,    0,   0,     0, 0,          0 }, /* RETIRE                         */
    { -1,    0,   0,     0, 0,          0 }, /* LOAD SETTINGS           */
    { -1,    0,   0,     0, 0,          0 }, /* SAVE SETTINGS                  */
    { -1,    0,   0,     0, 0,          0 }, /* LOAD CHAMPIONSHIP       */
    { -1,    0,   0,     0, 0,          0 }, /* SAVE CHAMPIONSHIP              */
    /* the draw list 0x0070B302 / 0x0070B30A, 0x0048BD13 */
    {  7, -156,  39, 0x140, 0x0048BD34, 0 }, /* OPTIONS 0x00473429..0x0047347F */
    {  8, -156,  41, 0x140, 0x0048BD34, 0 }, /* PLAYER DETAILS 0x004736EF..0x00473745 */
    { -1,    0,   0,     0, 0,          0 }, /* PLAYER 1 DETAILS: no draw list */
    { -1,    0,   0,     0, 0,          0 }, /* AUDIO SETUP                    */
    { -1,    0,   0,     0, 0,          0 }, /* ADVANCED AUDIO SETUP           */
    { -1,    0,   0,     0, 0,          0 }, /* VISUAL PREFERENCES             */
    { -1,    0,   0,     0, 0,          0 }, /* REDEFINE KEYBOARD              */
    { -1,    0,   0,     0, 0,          0 }, /* SETUP CONTROLLER               */
    { -1,    0,   0,     0, 0,          0 }, /* program controller             */
    { -1,    0,   0,     0, 0,          0 }, /* BEST TIMES (time trial) */
    { -1,    0,   0,     0, 0,          0 }, /* the INSPECTION AREA      */
    { -1,    0,   0,     0, 0,          0 }, /* VIEW STAGE               */
    { -1,    0,   0,     0, 0,          0 }, /* VIEW STAGE (time trial)  */
    /* INFORMATION's body 0x00489E3A..0x00489EAB: the same projection
     * as OPTIONS', kind 9, and [0x707E58] left at 1 (OPTIONS' 0x00473475
     * clears it; INFORMATION's body has no such store); draw list 0x70B312 */
    {  9, -156,  39, 0x140, 0x0048BD34, 1 }, /* INFORMATION                    */
    { -1,    0,   0,     0, 0,          0 }, /* BEST TIMES: no draw list       */
    { -1,    0,   0,     0, 0,          0 }, /* TOP SCORES                     */
    { -1,    0,   0,     0, 0,          0 }, /* STAGE INFORMATION              */
    /* VISUAL OPTIONS 0x0047C655: [0x707E68] / [0x707E70] are empty lists */
    { -1,    0,   0,     0, 0,          0 }, /* VISUAL OPTIONS                 */
    /* CAR SPECIFICATIONS 0x0047696D stores [0x5D6E44] = -160, [0x5D6E48] =
     * -100, [0x5D6F14] = 0x100 and kind 11; its draw list 0x0070B29A -> 0x0048BE40
     * runs only on page 0, and ends at 0x0048BE68 (one car, 0x0048C0D2) */
    { 11, -160, -100, 0x100, 0x0048BE68, 0 }, /* CAR SPECIFICATIONS             */
    /* SLIDESHOW 0x00476C7A: its draw list 0x70B2A2 is empty - no 3D */
    { -1,    0,   0,     0, 0,          0 }, /* SLIDESHOW                      */
    /* SAVE VIDEO REPLAY 0x0047B83E: [0x707E70] / [0x707E64] hold 2-D lists only */
    { -1,    0,   0,     0, 0,          0 }, /* SAVE VIDEO REPLAY              */
    { -1,    0,   0,     0, 0,          0 }  /* VIEW VIDEO REPLAY              */
};

/* 0x0048C4AC: the model draw, with the globals 0x0048C39A / 0x0048BD44 set */
static void vh_model(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    objlit_place p;
    (void)r;
    if (!f->m3d || !f->win_w) return;
    p.x = (int32_t)vm_rd32(v, 0x005E7EACu);
    p.y = (int32_t)vm_rd32(v, 0x005E7EB0u);
    p.z = (int32_t)vm_rd32(v, 0x005E7EB4u);
    p.scale16 = (int32_t)vm_rd32(v, 0x005E7EC8u);
    p.pitch = vm_rd32(v, 0x005E7ECCu);
    p.yaw = vm_rd32(v, 0x005E7ED0u);
    menu3d_draw(f->m3d, (int)vm_rd32(v, 0x005D422Cu), &p, f->win_w, f->win_h);
}

static int has3d(const fe *f)
{
    return f->m3d && !f->vm_bad && M3D[f->sc].kind >= 0;
}

/* The body's part of a screen entry: the kind, the slot, the projection. */
static void m3d_enter(fe *f)
{
    uint32_t regs[8];
    if (!has3d(f)) return;
    memset(regs, 0, sizeof regs);
    if (f->sc == SC_CAR || f->sc == SC_ARCCAR)
        regs[VM_ESI] = f->carlist;                           /* 0x0049C57A */
    vm_wr32(f->vm, 0x0070C9E0u, (uint32_t)M3D[f->sc].kind);
    if (fe_vmcall(f, 0x0048C1A2u, regs) != 0) {
        fprintf(stderr, "frontend: the carousel kind (0x0048C1A2) could not run: %s\n",
                orig_error(f->vm));
        f->vm_bad = 1;
        return;
    }
    /* the slot: cursor - 1 (0x0047100B), the car's index (0x0046D591), or
     * for REVIEW OPTIONS the cursor itself (0x00481FC9; 0x0048BD44 reads
     * [0x707E91] instead) */
    vm_wr32(f->vm, 0x00707E91u, (uint32_t)f->cursor);
    vm_wr32(f->vm, 0x0070C9E8u, f->sc == SC_CSPECS ? vm_rd32(f->vm, 0x007082FCu)   /* 0x004769CE */
                              : (f->sc == SC_CAR || f->sc == SC_ARCCAR)
                              ? (uint32_t)f->car_index
                              : (f->sc == SC_REVIEW || f->sc == SC_ARCCONT ||
                                 f->sc == SC_ARCNEXTLV || f->sc == SC_ARCRESTART)
                              ? (uint32_t)f->cursor
                              : (uint32_t)(f->cursor - 1));
    vm_wr32(f->vm, 0x00707E58u, (uint32_t)M3D[f->sc].carousel);
    vm_wr32(f->vm, 0x0070C9F0u, 0);
    menu3d_view(f->m3d, M3D[f->sc].ox, M3D[f->sc].oy, M3D[f->sc].zoom);
    f->scr_ms = f->now;                                      /* [0x00512730] */
}

/* RIGHT 0x00470DB1 / 0x00478FF9 (dirn 1) and LEFT 0x00470E39 / 0x00478F98
 * (dirn -1): the carousel's half of the arrow handlers. */
static void m3d_step(fe *f, int dirn)
{
    vm *v = f->vm;
    uint32_t cnt, slot, anim;
    if (!has3d(f)) return;
    cnt = vm_rd32(v, 0x0070C9E4u);
    slot = vm_rd32(v, 0x0070C9E8u);
    anim = vm_rd32(v, 0x0070C5CCu);
    if (dirn > 0) {
        if (cnt != 1 && anim == 1) {
            slot++;
            if ((int32_t)slot >= (int32_t)cnt) slot = 0;
        }
        vm_wr32(v, 0x0070C5CCu, 1);
        vm_wr32(v, 0x0070C9ECu, 0);
    } else {
        if (cnt != 1 && anim != 1) {
            slot--;
            if ((int32_t)slot < 0) slot = cnt - 1;
        }
        vm_wr32(v, 0x0070C5CCu, 0xFFFFFFFFu);
        vm_wr32(v, 0x0070C9ECu, 0xFFFFFFFFu);
    }
    vm_wr32(v, 0x0070C9E8u, slot);
}

/* The pre-draw 0x004664C9 (the frame time, then FUN_0048C114's slide), on
 * the screen loop's clock [0x005C6568] = GetTickCount - [0x00512730]. */
static void m3d_frame(fe *f)
{
    if (!has3d(f)) return;
    vm_wr32(f->vm, 0x005C6568u, f->now - f->scr_ms);
    vm_wr32(f->vm, 0x00707E91u, (uint32_t)f->cursor);
    if (fe_vmcall(f, 0x004664C9u, NULL) != 0) {
        fprintf(stderr, "frontend: 0x004664C9 could not run: %s\n", orig_error(f->vm));
        f->vm_bad = 1;
    }
}

void fe_draw_3d(fe *f, int win_w, int win_h)
{
    if (!has3d(f)) return;
    /* CAR SPECIFICATIONS' draw list (0x0048BE40) draws only on page 0 */
    if (f->sc == SC_CSPECS && vm_rd32(f->vm, 0x00708304u) != 0) return;
    f->win_w = win_w;
    f->win_h = win_h;
    menu3d_stats_reset(f->m3d);
    menu3d_begin(f->m3d);
    if (fe_vmcall(f, M3D[f->sc].draw, NULL) != 0) {
        fprintf(stderr, "frontend: the draw list (0x%08X) could not run: %s\n",
                M3D[f->sc].draw, orig_error(f->vm));
        f->vm_bad = 1;
    }
    menu3d_end(f->m3d);
    f->win_w = 0;
}

int fe_has_3d(const fe *f)
{
    return has3d(f);
}

void fe_report_3d(const fe *f, FILE *out)
{
    const menu3d_rec *r;
    int i, n;
    if (!has3d(f)) return;
    n = menu3d_records(f->m3d, &r);
    for (i = 0; i < n; i++)
        fprintf(out, "menu3d: draw %d type %d model %u at %d %d %d scale %d yaw %u pitch %u"
                " | polys %lu gated %lu lit %lu white %lu reflect %lu tex_none %lu"
                " | screen x %.1f..%.1f y %.1f..%.1f\n",
                i, r[i].type, r[i].model, r[i].place.x, r[i].place.y, r[i].place.z,
                r[i].place.scale16, r[i].place.yaw, r[i].place.pitch,
                r[i].st.polys, r[i].st.gated, r[i].st.lit, r[i].st.white,
                r[i].st.reflect, r[i].st.tex_none,
                r[i].st.min_sx, r[i].st.max_sx, r[i].st.min_sy, r[i].st.max_sy);
}

/* ---- screens ------------------------------------------------------------ */

static void m3d_enter(fe *f);
static void rpl_enter(fe *f);                                     
static int opt_enter(fe *f, int sc, char *err, size_t n);         
static int opt_index(int sc);
static int opt_screen(int sc);                                    
static void opt_arrow(fe *f, uint32_t h);
static int enter(fe *f, int sc, char *err, size_t n)
{
    static const uint32_t art[SC_COUNT] = { S_ART_MAIN, S_ART_SRAL, S_ART_PLAYERS,
        S_ART_CAR, 0, S_ART_PLAYERS, 0, 0, S_ART_RESULTS, S_ART_REVIEW,
        S_ART_ARCLEVEL, 0, S_ART_ARCRES, S_ART_ARCOVER, S_ART_ARCBEST, S_ART_ARCTOP,
        S_ART_ARCCONT, S_ART_ARCNEXTLV, S_ART_ARCRESTART, S_ART_ARCCAR,
        S_ART_CHNEW, 0, 0, S_ART_CHLEG, 0, S_ART_CHCLASS, S_ART_CHOVER,
        S_ART_CHRCLASS, S_ART_CHRALLY, S_ART_CHCCLASS, S_ART_CHCHAMP, S_ART_CHBEST,
        S_ART_CHBESTCH, S_ART_CHBESTOV, S_ART_CHREVIEW, S_ART_CHREVIEW2,
        S_ART_CHREVIEWR, S_ART_CHREVIEWF, S_ART_RETIRE, 0, 0, S_ART_LOADSV,
        S_ART_SAVESV, 0, 0, 0, 0, 0, 0, 0, 0, 0, S_ART_TTBEST, 0, 0,
        [SC_RPLSAVE] = S_ART_RPLSAVE, [SC_RPLLOAD] = S_ART_RPLLOAD };
    static const int kind[SC_COUNT] = { 0, 1, 2, -1, -1, 2, -1, -1, -1, -1,
        5, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1,
        [SC_RPLSAVE] = -1, [SC_RPLLOAD] = -1 };
    int rc;
    f->sc = sc;
    f->carousel_kind = kind[sc];
    f->cursor = f->saved[sc] ? f->saved[sc] : 1;
    cursor_visible(f);                                           
    if (sc >= SC_CHCLASS && sc <= SC_CHCHAMP) {
        /* each results body's first lines (0x00462F23..): its first
         * item says "back" (0x138) while [0x70CA44] is 1 - a REVIEW's
         * leaderboard - and "continue" (0x139) otherwise */
        f->tab[sc].it[0].tok = vm_rd32(f->vm, 0x0070CA44u) == 1 ? 0x138 : 0x139;
    }
    if (sc == SC_CHSPLASH && f->a8) {
        /* 0x0047B48A: the A8's - a8_welc.png with titlebar.png over it,
         * no narration, no 3,000 ms (any key goes on; ESC is RETIRE) */
        f->splash_ms = f->now;
        rc = load_art_over(f, rdstr(f, S_ART_A8WELC), rdstr(f, 0x006E46BCu), 0, err, n);
    } else if (sc == SC_CHSPLASH) {
        /* 0x0047B3E5: rally0.wav, then mob_welc.png with titlebar.png over it */
        if (f->snd) snd_speech(f->snd, 2, rdstr(f, S_CHRALLY0), f->snd->set.speech_menu);
        f->splash_ms = f->now;
        rc = load_art_over(f, rdstr(f, S_ART_CHSPLASH), rdstr(f, 0x006E46BCu), 0, err, n);
    } else if (sc == SC_CHWELCOME) {
        /* FUN_004650F5: the rally's picture ([0x70BBA2 + rally * 4] while
         * [0x708038] is 2, the A8's at 0x70BBBA otherwise), titlebar.png over
         * it; then 0x0047B4D2, the rally's narration */
        uint32_t r = vm_rd32(f->vm, A_CHRALLY);
        uint32_t tab = vm_rd32(f->vm, 0x00708038u) == 2 ? A_CHWELART : 0x0070BBBAu;
        rc = load_art_over(f, rdstr(f, rd(f, tab + 4u * r)), rdstr(f, 0x006E46BCu), 0,
                           err, n);
        vm_run(f, 0x0047B4D2, "0x0047B4D2 (the rally's narration)");
    } else if (sc == SC_CHNEXT) {
        /* FUN_00470180: [0x708068] = 3, the map (0x00465011), then 0x0047B4FA,
         * the stage's narration list */
        vm_wr32(f->vm, 0x00708068u, 3);
        rc = load_stage_art(f, err, n);
        vm_run(f, 0x0047B4FA, "0x0047B4FA (the stage's narration)");
    } else if (sc == SC_VIEWSTAGE) {
        /* FUN_0047BDFA: NEXT STAGE's loop (FUN_00470180) over its
         * own table.  The stage is kept aside ([0x7082C8] / [0x7082CC], and
         * the port's copy for the exit); 0x0047C0F5 counts the stages to the
         * next service area into [0x7082D0], 0x0046DC0C / 0x0046DBE8 set the
         * selector; then the map and 0x0047B4FA, the stage's narration */
        vm *v = f->vm;
        f->vs_stage[0] = vm_rd32(v, A_CHSTAGE);
        f->vs_stage[1] = vm_rd32(v, 0x007082C4u);
        vm_wr32(v, 0x007082C8u, f->vs_stage[0]);
        vm_wr32(v, 0x007082CCu, f->vs_stage[1]);
        vm_run(f, 0x0047C0F5, "0x0047C0F5 (the stages to the service area)");
        {
            /* EDI the table: item 1's arrows on (0x0046DC0C), or off and
             * its font dimmed (0x0046DBE8) with no other stage to show */
            uint32_t regs[8];
            memset(regs, 0, sizeof regs);
            regs[VM_EDI] = T_VIEWSTAGE;
            if (fe_vmcall(f, 0x0046DC0Cu, regs) == 0 && !vm_rd32(v, 0x007082D0u)) {
                memset(regs, 0, sizeof regs);
                regs[VM_EDI] = T_VIEWSTAGE;
                fe_vmcall(f, 0x0046DBE8u, regs);
            }
            load_table_from(f, &f->tab[sc], T_VIEWSTAGE, 1);
        }
        vm_wr32(v, 0x00708068u, 3);
        rc = load_stage_art(f, err, n);
        vm_run(f, 0x0047B4FA, "0x0047B4FA (the stage's narration)");
    } else if (sc == SC_TTVIEWSTAGE) {
        /* FUN_0048AEB9: the same loop over its own one-item table
         * (0x00707598, no arrows), [0x708068] = 1 - the single stage's map
         * (0x00465011) - and no narration; the stage is the service area's */
        vm_wr32(f->vm, 0x00708068u, 1);
        rc = load_stage_art(f, err, n);
    } else if (sc == SC_ARCNEXT) {
        /* FUN_004857FD: [0x707BC0] = 0, [0x708068] = 2 - the map is the
         * arcade itinerary's (0x00465048) */
        vm_wr32(f->vm, 0x00707BC0u, 0);
        vm_wr32(f->vm, 0x00708068u, 2);
        rc = load_stage_art(f, err, n);
    } else if (sc == SC_STAGE || sc == SC_TTSTAGE) {
        pre_stage(f);
        rc = load_stage_art(f, err, n);
    } else if (sc == SC_CHINSPECT) {
        /* 0x00479CB7: [0x70AB6E] = 0; then 0x00411573 + 0x00405E6D - the
         * picture alone, 0x0047B0C2 without its frame sprite */
        vm_wr32(f->vm, 0x0070AB6Eu, 0);
        rc = load_art_over(f, rdstr(f, S_ART_CHINSPECT), NULL, 0, err, n);
        if (rc) {
            /* A STAND-IN: service.png is in no
             * install we have, and the original's load fails to the guru
             * 0x80712345 (0x00479D4D -> 0x004D5900).  The port draws the
             * notice over OVERALL STAGE RESULTS' picture instead. */
            printf("frontend: %s - the INSPECTION AREA's picture is a STAND-IN\n", err);
            rc = load_art_over(f, rdstr(f, S_ART_CHOVER), NULL, 0, err, n);
        }
    } else if (sc == SC_SETUP) {
        rc = setup_enter(f, err, n);
    } else if (sc == SC_SULOAD || sc == SC_SUSAVE) {
        rc = su_slots_enter(f);
    } else {
        rc = load_art(f, rdstr(f, art[sc]), err, n);
    }
    if (sc == SC_CAR || sc == SC_ARCCAR) pre_car(f);
    if (sc == SC_CHLOAD || sc == SC_CHSAVE) ch_slots_enter(f);     
    if (sc == SC_RPLSAVE || sc == SC_RPLLOAD) rpl_enter(f);        /* / */
    m3d_enter(f);
    return rc;
}

/* ----: the three finish routines, the scene left out -------------
 *
 * 0x00484149 (a won arcade level), 0x004A5009 (a rally's end) and 0x0049D15B
 * (the championship's end) run WHOLE in the interpreter - so their own code
 * decides whether a scene comes (always for the arcade; the rally's and the
 * championship's only for 0x004A4FDE's placed player, or the winner) and
 * makes their writes ([0x70D068] .., the A8 unlock) - with the scene itself,
 * 0x004264D7, left out: the podium and the fireworks are after the release
 * (the user, 2026-10-20).  What is HEARD is theirs: the
 * tail after the scene stops the music (0x0040F639, twice) and plays track
 * 3 (0x0040F5A3); a routine that skips its scene makes neither call.  The
 * scene's set-up and teardown calls are not run (they load and free the 3D
 * world the menus do not have). */
static const uint32_t FIN_NOP[] = {
    0x004157F9, 0x0040EE51, 0x00441843, 0x0044187C, 0x0044188F, 0x00441832,
    0x0040456A, 0x00440F60, 0x004713BA
};

static void vh_fin_scene(vm *v, uint32_t r[8], void *ud)       /* 0x004264D7 */
{
    fe *f = (fe *)ud;
    (void)r;
    f->fin_scene = 1;
    if (f->scene_cb) {
        uint32_t k = vm_rd32(v, 0x0070D068u);
        f->scene_cb(f->scene_ud, (long)k, (long)(int32_t)vm_rd32(v, 0x0070D070u),
                    (long)(int32_t)vm_rd32(v, 0x0070D06Cu),
                    vm_rd32(v, 0x0070D074u) != 0);
    }
}

static void vh_fin_stop(vm *v, uint32_t r[8], void *ud)        /* 0x0040F639 */
{
    fe *f = (fe *)ud;
    (void)v; (void)r;
    if (f->snd) snd_music_stop(f->snd);
}

static void vh_fin_three(vm *v, uint32_t r[8], void *ud)       /* 0x0040F5A3 */
{
    fe *f = (fe *)ud;
    (void)v; (void)r;
    if (f->snd) snd_cue_after_race(f->snd);
}

static void finish_routine(fe *f, uint32_t va, const char *what)
{
    vm *v = f->vm;
    uint32_t regs[8];
    size_t i;
    int ok;

    for (i = 0; i < sizeof FIN_NOP / sizeof FIN_NOP[0]; i++)
        vm_hook(v, FIN_NOP[i], vh_nop, f);
    vm_hook(v, 0x004264D7, vh_fin_scene, f);
    vm_hook(v, 0x0040F639, vh_fin_stop, f);
    vm_hook(v, 0x0040F5A3, vh_fin_three, f);
    f->fin_scene = 0;
    memset(regs, 0, sizeof regs);
    {
        int nrc;
        ok = fe_native(f, va, regs, &nrc) ? nrc == 0 : orig_call(v, va, regs) == 0;
    }
    if (!ok)
        fprintf(stderr, "frontend: %s (0x%08X) could not run: %s\n", what, va,
                orig_error(v));
    for (i = 0; i < sizeof FIN_NOP / sizeof FIN_NOP[0]; i++)
        vm_unhook(v, FIN_NOP[i]);
    vm_unhook(v, 0x004264D7);
    vm_unhook(v, 0x0040F639);
    vm_unhook(v, 0x0040F5A3);
    if (!ok) return;
    if (f->fin_scene) {
        /* 0x00456F42: [shauntracknum] = 0x5FE9DC[[0x70D068]] */
        uint32_t k = vm_rd32(v, 0x0070D068u);
        uint32_t lv = k < 7u ? vm_rd32(v, 0x005FE9DCu + 4u * k) : 0xFFFFFFFFu;
        printf("frontend: %s: its scene, LEVEL%02u%s, %s; then track 3, as its "
               "tail\n", what, (unsigned)lv,
               vm_rd32(v, 0x0070D074u) ? " with the fireworks" : "",
               f->scene_cb ? "was shown" : "is left out (no scene callback)");
    } else
        printf("frontend: %s: no scene (no player placed)\n", what);
}

static void unported(fe *f, uint32_t va, const char *what)
{
    int i;
    char key[16];
    snprintf(key, sizeof key, "%08X", va);
    for (i = 0; i < f->nsaid; i++) if (!strcmp(f->said[i], key)) return;
    if (f->nsaid < 64) snprintf(f->said[f->nsaid++], sizeof f->said[0], "%s", key);
    printf("frontend: %s (handler 0x%08X) is not ported\n",
           what, va);
}

/* Car select's list (ESI), and 0x0048BBD6: the list's count - 1 */
static void set_carlist(fe *f, uint32_t list)
{
    f->carlist = list;
    for (f->car_max = 0; drd(f, list + 4u * (uint32_t)f->car_max) != 0xFFFFFFFFu &&
                         f->car_max < 64; f->car_max++) {}
    f->car_max--;
}

/* FUN_0046D562: the car list index of the player's car */
static void car_index_of(fe *f)
{
    int32_t i;
    f->car_index = 0;
    for (i = 0; i <= f->car_max; i++)
        if ((int32_t)rd(f, drd(f, f->carlist + 4u * (uint32_t)i)) == f->p_car) {
            f->car_index = i;
            return;
        }
}

/* The arrow callbacks (+34..+40) and the select handlers (+44), by address. */
static void call(fe *f, uint32_t h)
{
    /* STAGE INFORMATION's arrows share SELECT STAGE's handlers, and
     * run on the VM's selectors, not the port's */
    if (f->sc == SC_INFOSTAGE && h != H_NOP) { opt_arrow(f, h); return; }
    switch (h) {
    case H_NOP: break;
    case H_CAR_PREV:                     /* 0x00478FD2 */
        m3d_step(f, -1);                 /* 0x00478F99..0x00478FC8 */
        if (f->car_index == 0) f->car_index = f->car_max; else f->car_index--;
        pre_car(f);
        break;
    case H_CAR_NEXT:                     /* 0x0047903D */
        m3d_step(f, 1);                  /* 0x00478FFA..0x00479033 */
        if (f->car_index == f->car_max) f->car_index = 0; else f->car_index++;
        pre_car(f);
        break;
    case H_GEAR_NEXT:                    /* 0x00466576: 0..2, player+0x30 */
        if (++f->p_gear >= 3) f->p_gear = 0;
        pre_car(f);
        break;
    case H_GEAR_PREV:                    /* 0x004665C2 */
        if (--f->p_gear < 0) f->p_gear = 2;
        pre_car(f);
        break;
    case H_DIR:      f->dir ^= 1; break;                         /* 0x47E041 */
    case H_RALLY_NEXT: f->rally = f->rally >= 5 ? 0 : f->rally + 1; break;
    case H_RALLY_PREV: f->rally = f->rally < 1 ? 5 : f->rally - 1; break;
    case H_STAGE_NEXT: f->stage = f->stage >= 6 ? 1 : f->stage + 1; break;
    case H_STAGE_PREV: f->stage = f->stage < 2 ? 6 : f->stage - 1; break;
    /* state only, the original's code run - RETIRE's no / yes, the
     * results' pages (the class lists' 0x0049F3B4 / 0x0049F3DD, the 90-row
     * lists' 0x0049F40A / 0x0049F433) */
    case H_RETIRE_TOGGLE: case 0x0049F3B4: case 0x0049F3DD: case 0x0049F40A:
    case 0x0049F433:
    case 0x0046A04F: case 0x0046A064:    /* the slots' page, [0x707BB0] */
    case 0x0046A080: case 0x0046A098:    /* the replay slots' page, [0x707BBC] */
        vm_run(f, h, "a championship screen's arrow");
        break;
    case 0x0046C745: case 0x0046C77E: {  /* VIEW STAGE's < > */
        /* [0x7082D4] stepped in 0..[0x7082D0], [0x7082C0] = [0x7082C8] + it,
         * then the map (0x00465011, the port's load_stage_art) and the
         * narration 0x0047B4FA, which run in the interpreter */
        char err[256];
        vm_hook(f->vm, 0x00465011, vh_nop, f);
        vm_run(f, h, "VIEW STAGE's arrow");
        vm_unhook(f->vm, 0x00465011);
        if (load_stage_art(f, err, sizeof err))
            fprintf(stderr, "frontend: %s\n", err);
        return;
    }
    default:
        if (opt_screen(f->sc)) { opt_arrow(f, h); return; }      
        unported(f, h, "an arrow callback");
        return;
    }
    if ((f->sc == SC_STAGE || f->sc == SC_TTSTAGE) &&
        (h == H_DIR || h == H_RALLY_NEXT || h == H_RALLY_PREV ||
         h == H_STAGE_NEXT || h == H_STAGE_PREV)) {
        char err[256];
        pre_stage(f);
        if (load_stage_art(f, err, sizeof err))          /* 0x00465011 */
            fprintf(stderr, "frontend: %s\n", err);
    }
}

/* 0x0040EE7C on the menu clock */
static void fe_snd(fe *f, uint32_t mask)
{
    if (f->snd) snd_fe(f->snd, mask, f->now);
}

/* UP/DOWN/LEFT/RIGHT: 0x00470D0F / 0x00470D60 / 0x00470E39 / 0x00470DB1 -
 * move to the neighbour, or call the callback; the UP/DOWN mover steps past
 * an item whose +48 is not 0xFFFFFC1B (0x00470D45). */
static void move(fe *f, int dirn)
{
    fe_item *it = item(f, f->cursor);
    uint32_t nb;
    int guard = 0;
    if (!it) return;
    /* 0x00470DB2 / 0x00470E3A: the carousel steps first, when [0x707E58] */
    if (dirn >= 2 && has3d(f) && vm_rd32(f->vm, 0x00707E58u) == 1)
        m3d_step(f, dirn == 3 ? 1 : -1);
    nb = it->nb[dirn];
    /* MOVE whenever the item has a neighbour that way (!= 0x6F) -
     * a move or a callback alike; LEFT / RIGHT not while [0x707E3C] is 1
     * (0x00470D1E, 0x00470E0C / 0x00470E11) */
    if (nb != K_NONE && (dirn < 2 || vm_rd32(f->vm, 0x00707E3Cu) != 1))
        fe_snd(f, SND_MOVE);
    if (nb == K_CALL) { call(f, it->cb[dirn]); return; }
    if (nb == K_NONE || nb == 0) return;
    for (;;) {
        fe_item *to = item(f, (int)nb);
        if (!to) return;
        f->cursor = (int)nb;
        if (dirn > 1 || to->selectable == 0xFFFFFC1Bu || ++guard > 40) return;
        nb = to->nb[dirn];
        if (nb == K_CALL || nb == K_NONE) return;
    }
}

/* The setup screen's keys.  A picker being edited takes LEFT / RIGHT / RETURN
 * as FUN_004684F0's handlers read them (setup.h); otherwise the screen's own
 * moves and selects.  ESC leaves it as CONTINUE does: 0x00462C72 sets
 * [0x707ECD], which the loop at 0x0046F137 turns into the same exit - read
 * from the code, not yet seen in a picture. */
static int ch_go(fe *f, int pc, char *err, size_t n);
static int setup_leave(fe *f)
{
    f->saved[SC_SETUP] = f->cursor;
    setup_commit(f->vm);                                 /* 0x00468DAD         */
    if (f->snd) snd_music_stop(f->snd);                  /* 0x0046F1F8         */
    if (f->mode == 1) {                                  /* 0x0049D337  */
        char err[256];
        int r = ch_go(f, f->ch_pc, err, sizeof err);
        if (r < 0) { fprintf(stderr, "frontend: %s\n", err); return FE_QUIT; }
        return r == FE_LAUNCH ? FE_LAUNCH : FE_RUN;
    }
    su_read_sliders(f);
    f->launched = 1;
    return FE_LAUNCH;
}

static int setup_keys(fe *f, unsigned keys)
{
    vm *v = f->vm;
    if (vm_rd32(v, 0x0070CA08) == 1) {
        unsigned k = (keys & FE_LEFT) ? SU_KEY_LEFT : (keys & FE_RIGHT) ? SU_KEY_RIGHT :
                     (keys & FE_ENTER) ? SU_KEY_ENTER : 0;
        setup_edit_frame(v, k);
        return FE_RUN;
    }
    if (keys & FE_ESC) return setup_leave(f);
    if (keys & FE_ENTER) {
        int r;
        fe_item *it = item(f, f->cursor);
        fe_snd(f, SND_SELECT);                           /* 0x00462CC3         */
        if (it && it->sel == H_VIEWSTAGE) {
            /* 0x0046CFE3 outside time trial is 0x0047BDFA:
             * in time trial ([0x7082BC] 3) it is 0x0048AEB9 */
            char err[256];
            f->saved[SC_SETUP] = f->cursor;
            if (enter(f, vm_rd32(v, 0x007082BCu) == 3 ? SC_TTVIEWSTAGE : SC_VIEWSTAGE,
                      err, sizeof err))
                fprintf(stderr, "frontend: %s\n", err);
            return FE_RUN;
        }
        if (it && (it->sel == H_SULOAD || it->sel == H_SUSAVE)) {   
            char err[256];
            f->saved[SC_SETUP] = f->cursor;
            if (enter(f, it->sel == H_SULOAD ? SC_SULOAD : SC_SUSAVE, err, sizeof err))
                fprintf(stderr, "frontend: %s\n", err);
            return FE_RUN;
        }
        r = setup_select(v, f->cursor);
        if (r == 1) return setup_leave(f);
        if (r < 0) {
            fe_item *it = item(f, f->cursor);
            unported(f, it ? it->sel : 0, "this menu item");
        }
        return FE_RUN;
    }
    if (keys & FE_UP)    move(f, 0);
    else if (keys & FE_DOWN)  move(f, 1);
    else if (keys & FE_LEFT)  move(f, 2);
    else if (keys & FE_RIGHT) move(f, 3);
    return FE_RUN;
}

/* ----: LOAD / SAVE SETTINGS, the carset?? slots ---------------------
 *
 * FUN_0046B545 / FUN_0046B826: a screen over the service area's canvas (no
 * art of its own), page [0x707BB8]'s 30 slots in two columns, item 16 the
 * page cycle (0x0046BE3D), item 32 back (0x0046E300).  Every handler is the
 * original's code: the load 0x0046BB78 (the file through the port's file
 * layer, the six values into the pickers' globals and the player record, the
 * four pickers' own change handlers, then back), the save 0x0046BB2D (the
 * slot's name copied aside, the typing begun: [0x708010] = 1) and the typing
 * 0x0046BE55, once a frame while [0x708010] is 1 - RETURN writes the file
 * (0x004A6EDC), ESC puts the old name back.  The loops' own exits are
 * transcribed: after a SELECT the screen stays while the item's arg is 2
 * (every slot's) unless [0x707ECD] / [0x707ED5] / [0x7082EC] / [0x707EC9]
 * says go (0x0046B772..); the save screen leaves when the typing ends
 * (0x0046B9D9).  The slot names are the boot's (0x004111AA, 0x004A6E82). */

/* each screen's cursor dword - and the load screen's exit writes the SAVE
 * screen's (0x0046B7EE), as the original does */
static uint32_t su_curvar(const fe *f)
{
    return f->sc == SC_SULOAD ? 0x006F9FF0u : 0x006F9FECu;
}

static int su_slots_enter(fe *f)
{
    uint32_t c;
    vm_wr32(f->vm, 0x00707E50u, 0);                      /* 0x0046B546          */
    vm_wr32(f->vm, 0x0070AB6Eu, 0);
    vm_wr32(f->vm, A_SUNAMING, 0);
    c = vm_rd32(f->vm, su_curvar(f));
    f->cursor = c >= 1 && c <= 32 ? (int)c : 1;
    return 0;
}

/* 0x0047C095: VIEW STAGE's exit - the stage put back, the
 * narration stopped (0x0040F57A), the service area's loop again */
static int vs_leave(fe *f)
{
    char err[256];
    if (f->sc == SC_VIEWSTAGE) {
        vm_wr32(f->vm, A_CHSTAGE, f->vs_stage[0]);
        vm_wr32(f->vm, 0x007082C4u, f->vs_stage[1]);
        if (f->snd) snd_speech(f->snd, 0, NULL, 0);
    }                          /* time trial's (0x0048AEB9) has neither */
    vm_wr32(f->vm, 0x00707ECDu, 0);
    /* the map was drawn over the canvas: the service area's picture again */
    if (setup_art(f, vm_rd32(f->vm, setup_player(f->vm) + 0x08), err, sizeof err))
        fprintf(stderr, "frontend: %s\n", err);
    f->sc = SC_SETUP;
    f->carousel_kind = -1;
    f->cursor = f->saved[SC_SETUP];
    return FE_RUN;
}

/* 0x0046B7E8 / 0x0046BAEF: back into the service area's loop */
static int su_slots_leave(fe *f)
{
    vm_wr32(f->vm, 0x006F9FECu, (uint32_t)f->cursor);
    vm_wr32(f->vm, 0x00707ECDu, 0);
    vm_wr32(f->vm, 0x00707E50u, 1);
    f->sc = SC_SETUP;
    f->carousel_kind = -1;
    f->cursor = f->saved[SC_SETUP];
    return FE_RUN;
}

/* 0x0046B6E1 / 0x0046B9E8: a SELECT */
static int su_slots_select(fe *f)
{
    vm *v = f->vm;
    fe_item *it = item(f, f->cursor);
    if (!it) return FE_RUN;
    vm_wr32(v, su_curvar(f), (uint32_t)f->cursor);
    vm_wr32(v, 0x00707E91u, (uint32_t)f->cursor);
    vm_wr32(v, 0x007083A4u, it->arg54);                  /* 0x00470CCE          */
    f->su_ecx = (uint32_t)f->cursor;                     /* 0x0046B9F9 (su_typing) */
    vm_run(f, it->sel, "a slot screen's item");
    vm_wr32(v, 0x0070AB6Eu, 0);
    f->cursor = (int)vm_rd32(v, su_curvar(f));
    if (vm_rd32(v, 0x00707ED5u) == 1 || vm_rd32(v, 0x007082ECu) == 1 ||
        vm_rd32(v, 0x00707ECDu) == 1)
        return su_slots_leave(f);
    if (vm_rd32(v, 0x007083A4u) == 2 || vm_rd32(v, 0x00707EC9u) != 1) return FE_RUN;
    return su_slots_leave(f);
}

/* ----: SAVE VIDEO REPLAY, the replay slot screen ----------------------
 *
 * REVIEW OPTIONS' third row (0x0046D194 -> 0x0041C0C6 -> 0x0041C224 command 3):
 * with a recording made ([0x574548], set by the race's end) the slot picker
 * 0x0046C3E8 (EAX 1) -> 0x0047B83E, then 0x0041DF0A copies var\tmprpl to the
 * slot it returned.  The screen: savesv-style art (0x006E4939), twenty slot
 * names a page in two columns (0x0046C09C, the names at 0x0052245C: read at
 * boot from each file's header), item 21 the page (0x0046A080 / 0x0046A098,
 * its range text 0x0046D166), item 22 back.  A slot's SELECT (0x0046C377)
 * starts the typing of its name, 0x0046C169 each frame; RETURN keeps it and
 * ESC puts the old one back.  The loop's exit (0x0047BB51) returns [0x708378]:
 * RETURN stores 10 * page + item - 1 there (0x0047BA03), ESC while typing -1.
 *
 * THE ORIGINAL'S SLIPS, kept (read, not seen):
 *  the slot is 10 * page + item - 1, and the page is twenty slots wide: on
 *     page 1 the first name is slot 10, which page 0's eleventh name is
 *     already; slots 60..99 are never offered.  The saved file takes the NAME
 *     OF THE SLOT (0x0041DF5B), not the one just typed.
 *  BACK and ESC on the menu itself (not while typing) leave [0x708378]
 *     as it was - 0 until some screen stores -1 there - and the caller saves
 *     to whatever slot that is.  PATCHED here, by the user's decision: the
 *     entry stores -1 (rpl_enter), so BACK and ESC save nothing.
 * The save itself is rplrec_resave (the container's own copy, checked against
 * the game's two files). */
static void rpl_enter(fe *f)
{
    int load = f->sc == SC_RPLLOAD;                      /* 0x0047B54F reads [0x6FA198] */
    uint32_t c = vm_rd32(f->vm, load ? 0x006FA198u : 0x006FA000u);
    vm_wr32(f->vm, 0x0070AB6Eu, 0);
    vm_wr32(f->vm, A_SUNAMING, 0);
    /* PATCHED (the user, 2026-11-14): the original never stores the loop's result
     * on entry, so BACK / ESC return whatever [0x708378] held and the caller saves to it.
     * -1 is "no slot" (0x0041C283), as the typing's ESC stores. */
    if (!load) vm_wr32(f->vm, 0x00708378u, 0xFFFFFFFFu);
    f->cursor = c >= 1 && c <= 22 ? (int)c : 1;
    vm_draw(f, 0x0046D166, "the replay page's range");
}

/* 0x0041DF27 over the files: tmprpl to slot `slot`, its name from the table */
static void rpl_save(fe *f, int32_t slot)
{
    char src[1100], dst[1100], name[48];
    uint8_t tag[32];
    uint32_t k;
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    if (slot < 0 || slot > 99) return;       /* 100..199 are the install's own, shipped replays */
    if (!f->writes) {
        fprintf(stderr, "frontend: the replay is not saved (the saves are off)\n");
        return;
    }
    if (vfs_resolve(&f->fs, "var\\tmprpl", src, sizeof src) != 0) {
        fprintf(stderr, "frontend: no recording to save (%s)\n", src);
        return;
    }
    snprintf(name, sizeof name, "savedata\\replay\\replay%02d", (int)slot);
    vfs_resolve_dir(&f->fs, name, dst, sizeof dst);
    make_dirs(f, dst);
    for (k = 0; k < 32; k++) tag[k] = vm_rd8(f->vm, 0x0052245Cu + 0x20u * (uint32_t)slot + k);
    if (rplrec_resave(src, dst, tag, lt ? rplrec_stamp(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                                                         lt->tm_hour, lt->tm_min) : 0) == 0)
        printf("frontend: the replay is saved to slot %02d: %s\n", (int)slot, dst);
    else
        fprintf(stderr, "frontend: the replay could not be saved to %s\n", dst);
}

/* back to the screen a replay screen was chosen on: REVIEW OPTIONS or an OPTIONS one */
static int rpl_back_to(fe *f, char *err, size_t n)
{
    if (opt_screen(f->rpl_ret)) return opt_enter(f, f->rpl_ret, err, n);
    return enter(f, f->rpl_ret, err, n);
}

/* 0x0047BB1E..0x0047BB56: the loop's exit with [0x708378]; back to the REVIEW */
static int rpl_finish(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    int32_t slot = (int32_t)vm_rd32(v, 0x00708378u);
    vm_wr32(v, 0x006FA000u, (uint32_t)f->cursor);
    vm_wr32(v, 0x00707ECDu, 0);
    vm_wr32(v, 0x007083A8u, 0);
    vm_wr32(v, 0x0070AB6Eu, 0);
    if (slot != -1) rpl_save(f, slot);
    return rpl_back_to(f, err, n);
}

/* REVIEW OPTIONS' row: 0x0041C0C6 - with nothing recorded it does nothing */
static int rpl_begin(fe *f, char *err, size_t n)
{
    if (vm_rd32(f->vm, 0x00574548u) == 0) return 0;
    f->rpl_ret = f->sc;
    return enter(f, SC_RPLSAVE, err, n);
}

/* VIEW VIDEO REPLAY's slot screen is the save screen's twin: its slot items' SELECT is the
 * BACK handler (0x0046E300), the loop then returns BACK (item 22) as -1 and any other item as
 * 10 * page + item - 1 (0x0047B810..0x0047B83D) - the same page-1 slip: the slot is not
 * the one named under the cursor on pages 1..4.  ESC alone never sets the cancel word (0x0046E300's
 * own store is not run), so in the original it takes the slot under the cursor; PATCHED here,
 * ESC cancels.  A slot with no replay in it does nothing, silently, as 0x0041C2EC does. */
static int rpl_load_done(fe *f, int32_t slot, char *err, size_t n)
{
    vm *v = f->vm;
    vm_wr32(v, 0x006F9FFCu, (uint32_t)f->cursor);
    vm_wr32(v, 0x00707ECDu, 0);
    vm_wr32(v, 0x007083A8u, 0);
    vm_wr32(v, 0x0070AB6Eu, 0);
    if (slot >= 0 && slot <= 99) {
        char name[48], path[1100];
        snprintf(name, sizeof name, "savedata\\replay\\replay%02d", (int)slot);
        if (vfs_resolve(&f->fs, name, path, sizeof path) == 0 ||
            vfs_resolve(&f->inst, name, path, sizeof path) == 0) {
            snprintf(f->view_path, sizeof f->view_path, "%s", path);
            f->view_ret = f->rpl_ret;
            return FE_VIEW;
        }
        printf("frontend: slot %02d holds no replay\n", (int)slot);
    }
    return rpl_back_to(f, err, n);
}

/* REVIEW OPTIONS' view video replay, 0x0046D1A3: with a recording made ([0x574548]) it plays var\tmprpl
 * (command 1 of 0x0041C224); with none it plays ERROR (0x0040EE7C mask 8, after a 500 ms wait the
 * port does not keep) and does nothing */
static int rpl_view_begin(fe *f)
{
    char path[1100];
    if (vm_rd32(f->vm, 0x00574548u) == 0 || vfs_resolve(&f->fs, "var\\tmprpl", path, sizeof path) != 0) {
        printf("frontend: no recording to view (the race wrote none: [0x574548] is %u)\n",
               (unsigned)vm_rd32(f->vm, 0x00574548u));
        fe_snd(f, SND_ERROR);
        return FE_RUN;
    }
    snprintf(f->view_path, sizeof f->view_path, "%s", path);
    f->view_ret = f->sc;
    return FE_VIEW;
}

const char *fe_view_path(const fe *f)
{
    return f->view_path;
}

void fe_after_view(fe *f, uint32_t now_ms)
{
    char err[256];
    if (f->snd) {
        f->snd->now = now_ms;
        snd_mode_frontend(f->snd);
        snd_cue_after_race(f->snd);
        if (f->vm) vm_wr32(f->vm, 0x00511E88u, (uint32_t)f->snd->set.pos);
    }
    f->launched = 0;
    f->key_ms = now_ms;
    f->rpl_ret = f->view_ret;
    if (rpl_back_to(f, err, sizeof err) < 0) fprintf(stderr, "frontend: %s\n", err);
}

/* 0x0047BA17..: a SELECT on the screen */
static int rpl_select(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    fe_item *it = item(f, f->cursor);
    uint32_t h = it ? it->sel : 0;
    if (!it) return FE_RUN;
    if (f->sc == SC_RPLSAVE) vm_wr32(v, 0x006FA000u, (uint32_t)f->cursor);   /* the save loop's own dword */
    vm_wr32(v, 0x00707E91u, (uint32_t)f->cursor);
    vm_wr32(v, 0x007083A4u, it->arg54);
    if (h == H_BACK && f->sc == SC_RPLLOAD)              /* 0x0047B810: the loop's result */
        return rpl_load_done(f, f->cursor == 22 ? -1
                             : (int32_t)(vm_rd32(v, 0x00707BBCu) * 10u) + f->cursor - 1, err, n);
    if (h == H_BACK) return rpl_finish(f, err, n);       /* 0x0046E300: [0x707ECD] = 1 */
    if (h == 0x0046E2FEu) return FE_RUN;                 /* the page item: RET */
    f->su_ecx = (uint32_t)f->cursor;                     /* 0x0047BA28: ECX */
    vm_run(f, h, "a replay slot's item");
    return FE_RUN;
}

/* 0x0047B9C3: the typing frame, and what the loop does when it ends */
static int rpl_typing(fe *f, uint32_t regs[8], char *err, size_t n)
{
    vm *v = f->vm;
    regs[VM_ECX] = f->su_ecx;                            /* the SELECT's [0x707E91] */
    regs[VM_EDI] = T_RPLSAVE;                            /* 0x0047BA5A */
    if (fe_vmcall(f, 0x0046C169u, regs) != 0 && !f->vm_bad) {
        fprintf(stderr, "frontend: FUN_0046C169 (a replay slot's name typed) could not "
                        "run: %s\n", orig_error(v));
        f->vm_bad = 1;
    }
    f->dik = 0;
    vm_wr8(v, 0x004F1ACEu, 0);
    if (vm_rd32(v, A_SUNAMING) != 0) return FE_RUN;
    if (vm_rd32(v, 0x0070AB6Eu) == 1) {                  /* 0x0047B9D7: ESC */
        vm_wr32(v, 0x0070AB6Eu, 0);
        vm_wr32(v, 0x00708378u, 0xFFFFFFFFu);
    } else                                               /* 0x0047B9FE: RETURN */
        vm_wr32(v, 0x00708378u, vm_rd32(v, 0x00707BBCu) * 10u + vm_rd32(v, 0x00707E91u) - 1u);
    return rpl_finish(f, err, n);
}

/* 0x0046B9C2: the typing, once a frame - RETURN and ESC as their scan codes,
 * a typed key from fe_type */
static int su_typing(fe *f, unsigned keys)
{
    vm *v = f->vm;
    f->dik = (keys & FE_ENTER) ? 0x1C : (keys & FE_ESC) ? 0x01 : 0;
    if (!f->dik && f->ntyped) {
        f->dik = f->typed[0];
        vm_wr8(v, 0x004F1ACEu, f->typed_shift[0] ? 0x80 : 0);   /* LSHIFT's state */
        memmove(f->typed, f->typed + 1, sizeof f->typed - sizeof f->typed[0]);
        memmove(f->typed_shift, f->typed_shift + 1,
                sizeof f->typed_shift - sizeof f->typed_shift[0]);
        f->ntyped--;
    }
    vm_wr32(v, 0x00707E91u, (uint32_t)f->cursor);
    {
        /* the registers the loop holds at 0x0046B9CB - the pump and the frame
         * are PUSHAL-wrapped, so ECX is still the SELECT's [0x707E91]
         * (0x0046B9F9), and it matters: RETURN with nothing typed restores
         * the name and then 0x0046C150 counts its length into ECX inside a
         * PUSHAL / POPAL, so the count is lost and the cursor becomes that
         * ECX + 1 - the name is cut there (item 1 keeps two characters).
         * The original's own slip, kept. */
        uint32_t regs[8];
        int ch = f->sc == SC_CHSAVE;                     /* 0x0047B288   */
        memset(regs, 0, sizeof regs);
        if (f->sc == SC_P1DET) {
            /* 0x004648A0, once a frame while [0x708010] is 1
             * (0x00473B26); the screen stays when it ends */
            vm_wr32(v, 0x007082F8u, 0);
            if (fe_vmcall(f, 0x004648A0u, regs) != 0 && !f->vm_bad) {
                fprintf(stderr, "frontend: 0x004648A0 (the name typed) could not "
                                "run: %s\n", orig_error(v));
                f->vm_bad = 1;
            }
            f->dik = 0;
            vm_wr8(v, 0x004F1ACEu, 0);
            return FE_RUN;
        }
        if (f->sc == SC_RPLSAVE) {                       /* 0x0047B9C3 */
            char e2[256];
            int r2 = rpl_typing(f, regs, e2, sizeof e2);
            if (r2 < 0) { fprintf(stderr, "frontend: %s\n", e2); return FE_QUIT; }
            return r2;
        }
        regs[VM_EAX] = f->now - f->scr_ms;               /* 0x0046B9B2          */
        regs[VM_ECX] = f->su_ecx;
        regs[VM_EBX] = vm_rd32(v, ch ? 0x006F9FF8u : 0x006F9FECu);   /* 0x0046B841 */
        regs[VM_ESI] = ch ? 0x0070B46Eu : 0x0070B47Au;
        regs[VM_EDI] = ch ? 0x00704300u : 0x007052E0u;
        if (fe_vmcall(f, ch ? 0x0046A2E2u : 0x0046BE55u, regs) != 0 && !f->vm_bad) {
            fprintf(stderr, "frontend: FUN_0046BE55 (a slot's name typed) could not "
                            "run: %s\n", orig_error(v));
            f->vm_bad = 1;
        }
    }
    f->dik = 0;
    vm_wr8(v, 0x004F1ACEu, 0);
    if (vm_rd32(v, A_SUNAMING) == 0 && f->sc == SC_SUSAVE) {   /* 0x0046B9D9 */
        vm_wr32(v, 0x007083A4u, 2);
        return su_slots_leave(f);
    }
    return FE_RUN;
}

/* ----: LOAD CHAMPIONSHIP and save rally, the svgame?? slots --------
 *
 * FUN_0047ADF5 (the main menu's item 2) and FUN_0047B0F7 (REVIEW OPTIONS'
 * save rally): loadsv.png / savesv.png, page [0x707BB0]'s ten slots as items
 * 1..10 (their text runtime strings str#7..16), item 11 the page (str#4,
 * LEFT / RIGHT 0x0046A04F / 0x0046A064), item 12 back.  The pre-draws, every
 * frame: 0x00469EBF (the page's numbers into the strings; also the entry's
 * own list, 0x0070B76E), 0x0046A0B7 (the ten names from 0x005206DC), and
 * 0x0046A4CC (the slot under the cursor read, and its details or "empty").
 * The load's select 0x0046B312 runs whole - the slot unpacked over the
 * championship's globals, the players and the roster, the corrupt-save
 * check, the entry stepped, [0x7082D8] = 1 - with FUN_0049CB25 hooked: the
 * port's championship takes over there (ch_begin).  The save's select
 * 0x0046A1CE begins the typing, 0x0046A2E2 each frame (su_typing), and the
 * screen stays when it ends (0x0047B28D). */
static void ch_slots_enter(fe *f)
{
    uint32_t c = vm_rd32(f->vm, f->sc == SC_CHLOAD ? 0x006F9FF4u : 0x006F9FF8u);
    vm_wr32(f->vm, 0x0070AB6Eu, 0);
    vm_wr32(f->vm, A_SUNAMING, 0);
    f->cursor = c >= 1 && c <= 12 ? (int)c : 1;
    vm_draw(f, 0x00469EBF, "the slots' numbers");           /* 0x0070B76E */
}

static void vh_chcalled(vm *v, uint32_t r[8], void *ud)
{
    (void)r;
    ((fe *)ud)->ch_loaded = vm_last_call(v);
}

/* 0x0040EE7C, a frontend sound request (EAX the mask), while the load runs */
static void vh_fesnd(vm *v, uint32_t r[8], void *ud)
{
    (void)v;
    fe_snd((fe *)ud, r[VM_EAX]);
}

static int ch_begin(fe *f, char *err, size_t n);
static int ch_slots_leave(fe *f, char *err, size_t n);
/* 0x0047AF7D / 0x0047B29C: a SELECT on either screen */
static int ch_slots_select(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    fe_item *it = item(f, f->cursor);
    uint32_t h = it ? it->sel : 0, cv = f->sc == SC_CHLOAD ? 0x006F9FF4u : 0x006F9FF8u;
    if (!it) return FE_RUN;
    vm_wr32(v, cv, (uint32_t)f->cursor);
    vm_wr32(v, 0x00707E91u, (uint32_t)f->cursor);
    vm_wr32(v, 0x007083A4u, it->arg54);
    if (h == H_BACK) return ch_slots_leave(f, err, n);
    if (h == 0x0046E2FE) return FE_RUN;                  /* the page item: RET */
    f->su_ecx = (uint32_t)f->cursor;                     /* 0x0047B2AD          */
    f->ch_loaded = 0;
    if (f->sc == SC_CHLOAD) {
        vm_hook(v, 0x0049CB25, vh_chcalled, f);
        vm_hook(v, 0x004A60A7, vh_chcalled, f);
        vm_hook(v, 0x0040EE7C, vh_fesnd, f);
    }
    vm_run(f, h, "a championship slot's item");
    if (f->sc == SC_CHLOAD) {
        vm_unhook(v, 0x0049CB25);
        vm_unhook(v, 0x004A60A7);
        vm_unhook(v, 0x0040EE7C);
    }
    vm_wr32(v, 0x0070AB6Eu, 0);
    f->cursor = (int)vm_rd32(v, cv);
    if (f->ch_loaded == 0x0049CB25u) {
        /* the loaded players: the port's copies of player 1's car */
        f->p_car = (int32_t)vm_rd32(v, 0x007083B8u + 0x08);
        f->p_gear = (int32_t)vm_rd32(v, 0x007083B8u + 0x30);
        f->p_90 = (int32_t)vm_rd32(v, 0x007083B8u + 0x90);
        vm_wr32(v, 0x00707ECDu, 0);
        f->mode = 1;
        f->a8 = 0;
        set_carlist(f, A_CHCARS);
        return ch_begin(f, err, n);
    }
    if (f->ch_loaded == 0x004A60A7u) {
        /* the same hand-over for the A8's flow - its car list, mode
         * 0x13, the picture shown (ch_begin) */
        f->p_car = (int32_t)vm_rd32(v, 0x007083B8u + 0x08);
        f->p_gear = (int32_t)vm_rd32(v, 0x007083B8u + 0x30);
        f->p_90 = (int32_t)vm_rd32(v, 0x007083B8u + 0x90);
        vm_wr32(v, 0x00707ECDu, 0);
        f->mode = 1;
        f->a8 = 1;
        set_carlist(f, A_A8CARS);
        return ch_begin(f, err, n);
    }
    if (vm_rd32(v, 0x00707ED5u) == 1 || vm_rd32(v, 0x007082ECu) == 1 ||
        vm_rd32(v, 0x00707ECDu) == 1)
        return ch_slots_leave(f, err, n);
    return FE_RUN;
}

/* 0x0047B084 / 0x0047B3A3: the cursor kept, back where the screen came from */
static int ch_slots_leave(fe *f, char *err, size_t n)
{
    vm_wr32(f->vm, f->sc == SC_CHLOAD ? 0x006F9FF4u : 0x006F9FF8u, (uint32_t)f->cursor);
    vm_wr32(f->vm, 0x00707ECDu, 0);
    vm_wr32(f->vm, 0x0070AB6Eu, 0);
    return enter(f, f->sc == SC_CHLOAD ? SC_MAIN : f->chsave_ret, err, n);
}

void fe_type(fe *f, int dik, int shift)
{
    if (f->ntyped >= (int)(sizeof f->typed / sizeof f->typed[0])) return;
    f->typed[f->ntyped] = (uint8_t)dik;
    f->typed_shift[f->ntyped++] = (uint8_t)(shift != 0);
}

int fe_typing(const fe *f)
{
    return (f->sc == SC_SUSAVE || f->sc == SC_CHSAVE || f->sc == SC_P1DET || f->sc == SC_RPLSAVE) && f->vm &&
           vm_rd32(f->vm, A_SUNAMING) == 1;     /* PLAYER 1 DETAILS' name */
}

/* ---- the arcade ----------------
 *
 * FUN_00483E20 is one loop that calls a screen body and waits for it to
 * return; the port's menus are a frame loop, so the loop is cut at each
 * screen and resumed by that screen's exit.  Every state the flow keeps is
 * the original's own global in the interpreter ([0x70AB54] the level,
 * [0x70AB58] the stage, [0x70AB5C] lost), and every helper that only
 * computes is the original's code, run there (vm_run). */

static uint32_t arc_v(const fe *f, uint32_t va) { return vm_rd32(f->vm, va); }

/* 0x00486DE1: the arcade itinerary's entry for the level and the stage */
static uint32_t arc_entry(const fe *f)
{
    return rd(f, A_ARCITIN + 4u * arc_v(f, A_ARCLEVEL)) + 0xA4u * arc_v(f, A_ARCSTAGE);
}

/* A screen's pre- or post-draw handler, run: straight-line draw code. */
static void vm_draw(fe *f, uint32_t va, const char *what)
{
    vm_wr32(f->vm, 0x00707E91u, (uint32_t)f->cursor);
    vm_run(f, va, what);
    gfx2d_set_clip(&f->surf, 0, 0, W - 1, H - 1);
}

/* Run a post-draw that may call the box 0x00415A0C; if it drew one, the box's
 * rectangle ({x, y, w, h} at 0x00523364, zeroed first so a stale one is not
 * taken) also goes on the post-draw layer: the original draws its post-draw
 * list after the 3D draw list, and the port draws the models over the canvas
 **/
static void post_box(fe *f, uint32_t va, const char *what)
{
    int x, y, x0, y0, x1, y1;
    vm_wr32(f->vm, 0x0052336Cu, 0);
    vm_wr32(f->vm, 0x00523370u, 0);
    vm_draw(f, va, what);
    x0 = (int)vm_rd32(f->vm, 0x00523364u);
    y0 = (int)vm_rd32(f->vm, 0x00523368u);
    x1 = x0 + (int)vm_rd32(f->vm, 0x0052336Cu);
    y1 = y0 + (int)vm_rd32(f->vm, 0x00523370u);
    if (x1 <= x0 || y1 <= y0) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (!f->post_on) memset(f->post, 0, sizeof f->post);
    f->post_on = 1;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) f->post[y * W + x] = f->px[y * W + x] | 0xFF000000u;
}

void fe_set_scene_cb(fe *f, fe_scene_fn fn, void *ud)
{
    f->scene_cb = fn;
    f->scene_ud = ud;
}

void fe_set_quit_confirm(fe *f, int on)
{
    f->quit_confirm = on;
}

/* 0x00483EA7 / 0x0047CDCB / 0x00476F13: car select, from the arcade's list
 * - every car with cheat bit 1 */
static int arc_car_select(fe *f, char *err, size_t n)
{
    set_carlist(f, (arc_v(f, A_CHEATS) & 2u) ? A_ALLCARS : A_ARCCARS);
    car_index_of(f);
    return enter(f, SC_CAR, err, n);
}

/* FUN_00483E20 to car select: a new arcade game at the chosen level */
static int arc_begin(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    vm_run(f, 0x004A5249, "FUN_004A5249");             /* players' +0x284 = -1 */
    vm_wr32(v, rd(f, 0x00708DE8u) + 0xD0u, 1);         /* 0x00483E34          */
    vm_wr32(v, A_ARCFAIL, 0);
    vm_run(f, 0x0046C8A9, "FUN_0046C8A9");             /* OPTIONS' items off  */
    vm_wr32(v, 0x0070AB10u, 0);
    vm_wr32(v, 0x0070AB00u, 0);
    vm_wr32(v, 0x0070AB20u, 0);
    vm_wr32(v, A_ARCPOS, 0);
    vm_wr32(v, 0x007082F0u, 1);                         /* one player          */
    vm_wr32(v, 0x007082F4u, 0x1D);
    vm_wr32(v, 0x00707EC9u, 0);
    vm_wr32(v, A_ARCSTAGE, 1);
    vm_wr32(v, 0x007082F8u, 0);
    return arc_car_select(f, err, n);
}

/* 0x00483ED5: each stage begins at NEXT STAGE */
static int arc_next_stage(fe *f, char *err, size_t n)
{
    vm_wr32(f->vm, A_ARCFAIL, 0);
    return enter(f, SC_ARCNEXT, err, n);
}

/* 0x00483EF1..0x00483F37: NEXT STAGE has gone; the launch, gmode 2 */
static int arc_launch(fe *f)
{
    vm *v = f->vm;
    if (vm_rd32(v, 0x00708048u))                       /* hst.dat's block D   */
        vm_run(f, 0x00486DDA, "FUN_00486DDA (the stage's best time)");
    vm_wr32(v, 0x0070CF60u, arc_v(f, A_ARCSTAGE));      /* shaunstagenumber    */
    vm_wr32(v, 0x0070CF64u, arc_v(f, A_ARCLEVEL));      /* shaunrallynumber    */
    /* 0x00483F0A: direction 1, or 0 with cheat bit 0x12 */
    f->crsdirec = (arc_v(f, A_CHEATS) & (1u << 0x12)) ? 0 : 1;
    vm_wr32(v, 0x0070CC58u, (uint32_t)f->crsdirec);
    vm_run(f, 0x00482F5F, "FUN_00482F5F (the new level's counters)");
    f->track = (int32_t)vm_rd32(v, 0x0070CC3Cu);       /* 0x00485E73's store  */
    if (f->vm) su_sync_player(f);
    su_read_sliders(f);
    f->launched = 1;
    return FE_LAUNCH;
}

/* 0x0048402C, FUN_00486D55: the level is over - won only when the race's
 * arcadepo is 1 */
static int arc_level_end(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    uint32_t lv = arc_v(f, A_ARCLEVEL);
    if (arc_v(f, A_ARCPOS) != 1) {
        if (f->snd) snd_cue(f->snd, 7, 4, f->snd->set.music_menu);   /* 0x0040F5FD */
        vm_wr32(v, A_ARCFAIL, 1);
        return enter(f, SC_ARCOVER, err, n);           /* 0x00484F40          */
    }
    if (f->snd) snd_cue(f->snd, 6, 0, f->snd->set.music_menu);       /* 0x0040F5EB */
    finish_routine(f, 0x00484149, "the arcade's finish routine");    /* 0x00486D7A */
    if (vm_rd8(v, A_ARCWON + lv) == 0) {
        vm_wr8(v, A_ARCWON + lv, 1);                    /* 0x00486D8D          */
        set_carlist(f, A_BONUSCARS);                    /* 0x00486D94          */
        f->car_index = 0;                               /* [0x7082FC] = 0      */
        car_index_of(f);                                /* 0x0047729B          */
        return enter(f, SC_ARCCAR, err, n);
    }
    save_dat(f);                                        /* 0x00486DA9          */
    return -2;                                         /* on to 0x00484031    */
}

/* 0x00484031: after the level's end - restart it, the next, or the menu */
static int arc_after_level(fe *f, char *err, size_t n)
{
    if (arc_v(f, A_ARCFAIL) == 1) return enter(f, SC_ARCRESTART, err, n);
    if (arc_v(f, A_ARCLEVEL) == 5) return enter(f, SC_MAIN, err, n);  /* 0x00484124 */
    return enter(f, SC_ARCNEXTLV, err, n);
}

static int arc_end(fe *f, char *err, size_t n)
{
    int r = arc_level_end(f, err, n);
    return r == -2 ? arc_after_level(f, err, n) : r;
}

/* 0x00483FEF: the stage count, then the next stage or the level's end */
static int arc_after_best(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    uint32_t st = arc_v(f, A_ARCSTAGE) + 1;
    vm_wr32(v, A_ARCSTAGE, st);
    if ((int32_t)st <= 6) {                             /* 0x00484099          */
        if (arc_v(f, A_ARCFAIL) == 1) return enter(f, SC_ARCRESTART, err, n);
        return enter(f, SC_ARCCONT, err, n);            /* 0x0048494C          */
    }
    /* the TOP SCORES table is hst.dat's block C - read, or seeded at the
     * main menu's entry; the test stays for a heap that failed */
    if (vm_rd32(v, 0x00708044u))
        vm_run(f, 0x00484244, "FUN_00484244 (the level's score against the table)");
    if (arc_v(f, A_NEWBEST) == 1 && vm_rd32(v, 0x00708044u)) {
        vm_run(f, 0x00486CEA, "FUN_00486CEA");
        vm_run(f, 0x00486030, "FUN_00486030 (TOP SCORES' new entry)");
        return enter(f, SC_ARCTOP, err, n);
    }
    return arc_end(f, err, n);
}

/* RESULTS has gone: 0x00483F9D..0x00483FE9 */
static int arc_after_results(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    uint32_t regs[8];
    memset(regs, 0, sizeof regs);
    regs[VM_EDI] = arc_entry(f);
    if (fe_vmcall(f, 0x00484207, regs) != 0)              /* the top speed record */
        fprintf(stderr, "frontend: FUN_00484207 could not run: %s\n", orig_error(v));
    /* BEST TIMES is hst.dat's block D: no file, no table, no record */
    if (arc_v(f, A_ARCFAIL) != 1 && vm_rd32(v, 0x00708048u)) {
        vm_run(f, 0x00484274, "FUN_00484274 (the stage's time against the table)");
        if (arc_v(f, A_NEWBEST) == 1) {
            vm_run(f, 0x00486D12, "FUN_00486D12");
            vm_run(f, 0x00485F67, "FUN_00485F67 (BEST TIMES' new entry)");
            return enter(f, SC_ARCBEST, err, n);
        }
    }
    return arc_after_best(f, err, n);
}

/* time trial's RESULTS has gone, FUN_00486E1B 0x00486EFD..0x00486F4B.
 * EDI = the stage's itinerary entry, [[0x6E7E85 + dir * 4] + rally * 4] +
 * stage * 0xA4; 0x00489D20 keeps the stage's top speed (+0x2C, the
 * player's +0x94, with his +0x04 / +0x08 / name); 0x004887B0 sets
 * [0x708280] when the player's time (+0x88) is no worse than the table's
 * tenth (hst.dat block B or F by direction, [0x70804C] / [0x708050] +
 * entry+0x3C * 0x618 + 0x57C, its +0x14); then 0x0048883E clears every
 * row's "new" flag (+0x40), 0x00488898 writes the player into the tenth
 * row - his time, +0x90, +0x04, +0x08, the cheat word [0x70AA78],
 * FUN_0049F9B3 of the stage's map, the NAME copied from his own +0x18
 * (31 bytes; no typing) - and bubbles it up (0x00488979), and BEST TIMES
 * (0x00488B70) shows the table.  Every one of them is run, not ported. */
static int tt_after_results(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    uint32_t regs[8];
    vm_wr32(v, 0x00708230u, (uint32_t)f->dir);
    vm_wr32(v, 0x00708238u, (uint32_t)f->rally);
    vm_wr32(v, 0x00708234u, (uint32_t)f->stage);
    vm_wr32(v, 0x007082F8u, 0);
    memset(regs, 0, sizeof regs);
    regs[VM_EDI] = vm_rd32(v, vm_rd32(v, A_ITIN + 4u * (uint32_t)f->dir)
                              + 4u * (uint32_t)f->rally)
                 + 0xA4u * (uint32_t)f->stage;
    if (fe_vmcall(f, 0x00489D20, regs) != 0)
        fprintf(stderr, "frontend: FUN_00489D20 could not run: %s\n", orig_error(v));
    vm_wr32(v, A_NEWBEST, 0);
    vm_run(f, 0x004887B0, "FUN_004887B0 (the time against the table)");
    if (vm_rd32(v, A_NEWBEST) == 1) {
        vm_run(f, 0x0048883E, "FUN_0048883E (the rows' new flags cleared)");
        vm_run(f, 0x00488898, "FUN_00488898 (the player's row, bubbled up)");
        return enter(f, SC_TTBEST, err, n);
    }
    return enter(f, SC_REVIEW, err, n);
}

/* 0x00484066 / 0x004840B9: start next level, restart level */
static int arc_again(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    vm_wr32(v, 0x0070AB20u, 0);
    vm_wr32(v, 0x0070AB00u, 0);
    vm_wr32(v, 0x0070AB10u, 0);
    vm_wr32(v, A_ARCSTAGE, 1);
    if (f->sc == SC_ARCNEXTLV) {
        vm_wr32(v, A_ARCLEVEL, arc_v(f, A_ARCLEVEL) + 1);
        vm_wr32(v, 0x007082F8u, 0);                     /* 0x00483E9D          */
        return arc_car_select(f, err, n);
    }
    vm_wr32(v, rd(f, 0x00708DE8u) + 0xD0u, 1);         /* 0x004840EF          */
    vm_wr32(v, A_ARCFAIL, 0);
    return arc_next_stage(f, err, n);
}

/* What leaving an arcade screen does - SELECT and ESC alike unless named.
 * `h` is the item's handler, 0 for ESC. */
static int arc_leave(fe *f, uint32_t h, char *err, size_t n)
{
    switch (f->sc) {
    case SC_ARCNEXT:   return arc_launch(f);           /* ESC too: 0x00483EE4 */
    case SC_ARCRES:    return arc_after_results(f, err, n);
    case SC_ARCBEST:   return arc_after_best(f, err, n);
    case SC_ARCTOP:    return arc_end(f, err, n);
    case SC_ARCOVER:   return arc_after_level(f, err, n);
    case SC_ARCCAR:
        /* 0x00482BCE: the car joins the arcade's list; 0x00482BF1: it leaves
         * the bonus list */
        vm_wr32(f->vm, 0x007082FCu, (uint32_t)f->car_index);
        vm_run(f, 0x00482BCE, "FUN_00482BCE");
        vm_run(f, 0x00482BF1, "FUN_00482BF1");
        save_dat(f);                                    /* 0x00486DA9 */
        return arc_after_level(f, err, n);
    case SC_ARCCONT:
        /* 0x00484108: on to NEXT STAGE unless return to main menu */
        if (h == H_REVIEW_MAIN) return enter(f, SC_MAIN, err, n);
        return arc_next_stage(f, err, n);
    case SC_ARCNEXTLV:
    case SC_ARCRESTART:
        /* 0x004840A7 / 0x0048404C: [0x707ED1], only from the first item */
        if (h == H_ARCAGAIN) return arc_again(f, err, n);
        return enter(f, SC_MAIN, err, n);
    }
    return 0;
}

static int arc_screen(int sc)
{
    return sc >= SC_ARCNEXT && sc <= SC_ARCCAR;
}

/* SELECT LEVEL's items 1..6: 0x00483C9E, 0x00483CBD + 0x47 * (k - 2) */
static int arc_select_level(fe *f, uint32_t h, char *err, size_t n)
{
    int k = h == H_ARCLV1 ? 1 : (int)((h - 0x00483CBDu) / 0x47u) + 2;
    if (k < 1 || k > 6) return 0;
    /* a level after the first: cheat bit 0, or the one before it won */
    if (k > 1 && !(arc_v(f, A_CHEATS) & 1u) &&
        vm_rd8(f->vm, A_ARCWON + (uint32_t)(k - 2)) != 1) {
        fe_snd(f, SND_ERROR);                           /* 0x00483CDA          */
        return 0;
    }
    vm_wr32(f->vm, A_ARCLEVEL, (uint32_t)(k - 1));
    vm_wr32(f->vm, A_ARCSTAGE, 0);
    vm_run(f, 0x00464AE0, "FUN_00464AE0 (the units)");
    f->mode = 2;
    return arc_begin(f, err, n);
}

/* ---- the championship ----------
 *
 * START RALLY's 2nd slot (0x004727FE) is NEW SIMULATION, a carousel of the
 * player counts; one player (0x0049E278) is car select from the
 * championship's list and then FUN_0049CB25, the Mobil 1 championship: one
 * loop over the itinerary 0x006E7EE9 (a rally's start, its service halts,
 * its stages, its end) that calls a screen body and waits for it - the
 * arcade's shape (section 19), and cut the same way: ch_go runs from one of
 * its labels to the next screen and the screen's exit resumes it.  Every
 * state it keeps is the original's global in the interpreter; every helper
 * that only computes is the original's code run there - the rivals' times
 * (0x0046D5B5, and 0x0046F785's shuffle and retirements), the results
 * lists, the sorts, the positions, the standings.  A helper that ends in a
 * screen runs with that screen's body hooked (vh_body); the class loops
 * that call one screen per class are ch_loop. */

enum {                                    /* FUN_0049CB25's labels          */
    CH_SPLASH_DONE,     /* 0x0049CBC5: the welcome picture has gone         */
    CH_RALLY,           /* 0x0049CBE1: a rally begins                       */
    CH_ENTRY,           /* 0x0049CC1D: the next itinerary entry             */
    CH_LEG_DONE,        /* 0x0049CDDF: NEXT STAGE                           */
    CH_NEXT_DONE,       /* 0x0049CE0B: the service area, FUN_0049D26A       */
    CH_SVC_DONE,        /* 0x0049D332: the car setup screen has gone        */
    CH_SVC_END,         /* 0x0049D395                                       */
    CH_RACE,            /* 0x0049CEEB: FUN_0046F785, the race               */
    CH_POSTRACE,        /* 0x0049CEF0                                       */
    CH_CLASS,           /* FUN_0049DB6D's loop: CLASS STAGE RESULTS         */
    CH_OVER_DONE,       /* 0x0049CF69                                       */
    CH_RCLASS,          /* FUN_0049EF26's loop, then 0x0049F81D             */
    CH_D016,            /* 0x0049D016: the best times                       */
    CH_BEST_DONE,       /* 0x0049D05F: what comes next                      */
    CH_REVIEW,          /* 0x0049D0DE                                       */
    CH_NEXTSTAGE,       /* 0x0049D10E                                       */
    CH_RET,             /* 0x0049CE44: out of the rally                     */
    CH_EOR,             /* 0x0049CC8D: the rally's end                      */
    CH_CCLASS,          /* FUN_004A0691's loop: CLASS CHAMPIONSHIP          */
    CH_CHAMP_DONE,      /* 0x0049CCE8                                       */
    CH_REVIEWR_DONE,    /* 0x0049CD0C: the next rally                       */
    CH_END,             /* 0x0049D129: the championship's end               */
    CH_BESTCH,          /* FUN_004A41E1's loop                              */
    CH_BESTOV,          /* FUN_004A4D92                                     */
    CH_FINAL,           /* 0x0049D14F: the last REVIEW OPTIONS              */
    CH_LB_DONE,         /* a REVIEW leaderboard's screen has gone           */
    CH_EXIT,            /* 0x0049D154: back to the main menu                */
    /* the INSPECTION AREA 0x00479CB6 is modal inside two loops, so
     * each is cut around it */
    CH_SVC_INSP,        /* 0x0049D35D: its CONTINUE after the service       */
    CH_SVC_PEN,         /* 0x0049D36C: the overrun penalty                  */
    CH_INSP,            /* FUN_004A412F's loop, 0x0049D09B: the next player */
    CH_INSP_DONE        /* 0x004A41A1: its CONTINUE after a stage           */
};
#define CH_NONE (-100)                    /* a loop with no screen left     */

/* What RETIRE's yes does, by where the flow is (each screen's caller tests
 * [0x707BC0] on its return): leave the championship (WELCOME, the end of a
 * rally, every REVIEW - 0x0049D154), retire from this rally before its race
 * (LEG DETAILS, NEXT STAGE - 0x0049CDC1: [0x707BC4] = 1, FUN_0047D46C, then
 * the race, which is skipped) or after it (the results - 0x0049CF15: the
 * same, then 0x0049CE44). */
enum { PH_EXIT, PH_PRE, PH_POST };

static void ch_inc(fe *f, uint32_t va) { vm_wr32(f->vm, va, vm_rd32(f->vm, va) + 1u); }

/* FUN_0046EBBC: the itinerary entry of [0x7082C4] / [0x7082C0] + ahead -
 * skip [0x7082C4] rallies (each ends at a kind-4 entry), then that many
 * 0xA4-byte entries */
static uint32_t ch_entry(const fe *f, int ahead)
{
    uint32_t e = A_CHITIN, r = vm_rd32(f->vm, A_CHRALLY), c = 0;
    int guard = 0;
    if (r != 0)
        for (;;) {
            if (vm_rd32(f->vm, e) == 4 && ++c == r) { e += 0xA4u; break; }
            e += 0xA4u;
            if (++guard > 200) break;
        }
    return e + 0xA4u * (vm_rd32(f->vm, A_CHSTAGE) + (uint32_t)ahead);
}

/* FUN_0046D95B: the kind of the entry after this one (also in [0x708358]) */
static uint32_t ch_kind_next(fe *f)
{
    vm_run(f, 0x0046D95B, "FUN_0046D95B (the next entry)");
    return vm_rd32(f->vm, 0x00708358u);
}

static int ch_go(fe *f, int pc, char *err, size_t n);
static int ch_lb_done(fe *f, char *err, size_t n);
static int ch_screen_of(uint32_t body)
{
    size_t k;
    for (k = 0; k < sizeof CH_BODY / sizeof CH_BODY[0]; k++)
        if (CH_BODY[k][0] == body) return (int)CH_BODY[k][1];
    return -1;
}

/* Run a helper that prepares a screen and calls its body (hooked); enter the
 * screen it asked for, the flow resuming at `resume` when it goes. */
static int ch_show(fe *f, uint32_t helper, uint32_t ebx, int resume, char *err, size_t n)
{
    uint32_t regs[8];
    int sc;
    memset(regs, 0, sizeof regs);
    regs[VM_EBX] = ebx;
    f->ch_req = 0;
    if (fe_vmcall(f, helper, regs) != 0)
        fprintf(stderr, "frontend: 0x%08X could not run: %s\n", helper, orig_error(f->vm));
    sc = ch_screen_of(f->ch_req);
    if (sc <= 0) {
        if (f->ch_req == 0x00479CB6u)
            unported(f, 0x00479CB6u, "the INSPECTION AREA from a helper the flow does "
                                     "not cut");
        return ch_go(f, resume, err, n);
    }
    f->ch_pc = resume;
    return enter(f, sc, err, n);
}

/* The class loops (FUN_0049DB6D, FUN_0049EF26, FUN_004A0691): for each class
 * EBX from the loop's start to [0x708260], when a row of the list has that
 * class and is player 1, 2 or 3 (+4 in 1..3 - not 4: the original's), the
 * class's positions (ECX -> [0x7082E8]) and the screen. */
typedef struct {
    uint32_t list, rec, cls;   /* [list]: rows of `rec` bytes, the class at +cls */
    uint32_t count, show;      /* the positions helper, the screen's helper     */
} ch_loop_t;
static const ch_loop_t CL_STAGE = { 0x00708054, 0x9C, 0x1C, 0x0049DCDC, 0x0049DC01 };
static const ch_loop_t CL_RALLY = { 0x00708054, 0x9C, 0x1C, 0x0049DCDC, 0x0049EF98 };
static const ch_loop_t CL_CHAMP = { 0x00708058, 0x78, 0x10, 0x004A0712, 0x004A074A };

static int ch_loop(fe *f, const ch_loop_t *L, int self, char *err, size_t n)
{
    vm *v = f->vm;
    while (f->ch_k < (int)vm_rd32(v, 0x00708260u)) {
        uint32_t k = (uint32_t)f->ch_k++, e = vm_rd32(v, L->list), regs[8];
        int found = 0, guard = 0;
        for (; vm_rd32(v, e) != 0xFFFFFFFFu && guard < 4000; e += L->rec, guard++) {
            uint32_t p = vm_rd32(v, e + 4);
            if (vm_rd32(v, e + L->cls) == k && p >= 1 && p <= 3) found = 1;
        }
        vm_wr32(v, 0x00707ED1u, (uint32_t)found);
        if (!found) continue;
        memset(regs, 0, sizeof regs);
        regs[VM_EBX] = k;
        if (fe_vmcall(f, L->count, regs) != 0)
            fprintf(stderr, "frontend: 0x%08X could not run: %s\n", L->count, orig_error(v));
        if (!fdb_lookup(L->count))      /* a twin hands its ECX back in regs */
            orig_regs(v, regs);
        vm_wr32(v, 0x007082E8u, regs[VM_ECX]);
        return ch_show(f, L->show, k, self, err, n);
    }
    return CH_NONE;
}

/* START RALLY's 3rd slot, 0x004A557A, is NEW SIMULATION again - the
 * same screen, art and 3D carousel (0x0047285A..), over its OWN table
 * 0x006FC06C, which differs from 0x006FBE3C in the one-player item's select
 * (0x004A586B), the three player counts above (0x004A592C .. 0x004A5C10,
 * deferred) and the cursor's dword ([0x6FA274], not [0x6FA0D4]).  The port
 * keeps the one screen and swaps the table (and the remembered cursor). */
static void a8_open(fe *f)
{
    int t;
    if (f->a8_tab) return;
    f->a8_tab = 1;
    load_table(f, &f->tab[SC_CHNEW], T_A8NEW);
    t = f->saved[SC_CHNEW]; f->saved[SC_CHNEW] = f->a8_saved; f->a8_saved = t;
}

/* NEW SIMULATION left for START RALLY, or the championship over: the Mobil 1's
 * table again */
static void a8_close(fe *f)
{
    int t;
    f->a8 = 0;
    if (!f->a8_tab) return;
    f->a8_tab = 0;
    load_table(f, &f->tab[SC_CHNEW], T_CHNEW);
    t = f->saved[SC_CHNEW]; f->saved[SC_CHNEW] = f->a8_saved; f->a8_saved = t;
}

/* 0x0049D154 and the caller's tail (0x0049E310): back to the main menu.  The
 * A8's caller (0x004A5916 -> 0x004A5920) clears only [0x7082EC]; the Mobil 1's
 * also [0x707EC9], [0x707ED5] and [0x707ED1].  The port clears all three for
 * both: [0x707ED5] the main menu's own body would clear (0x00470FC5), and the
 * other two nothing in the port reads (a DEPARTURE for the A8, harmless by
 * reading - the START RALLY loop leaves on [0x7082EC] either way). */
static int ch_exit(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    vm_run(f, 0x0046C83A, "FUN_0046C83A");
    vm_wr32(v, 0x00707EC9u, 0);
    vm_wr32(v, 0x00707ED5u, 0);
    vm_wr32(v, 0x00707ED1u, 0);
    vm_wr32(v, 0x007082ECu, 1);
    a8_close(f);
    if (f->snd) snd_speech(f->snd, 0, NULL, 0);
    f->mode = 0;
    return enter(f, SC_MAIN, err, n);
}

/* FUN_0049D15B, whole (the finish_routine): the championship's winner
 * opens the A8 slot - [0x70CC34] = [0x70D074] = 1 when the first player row
 * of the standings is in first place (FUN_004A07BA, EBX 0x63; FUN_004A078B,
 * EBX 0, when [0x708038] is 0x13) - then the finish routine, its scene left
 * out. */
static void ch_unlock(fe *f)
{
    finish_routine(f, 0x0049D15B, "the championship's finish routine");
}

/* FUN_004A41E1's loop (BEST CHAMPIONSHIP SCORES, per class) and FUN_004A4D92
 * (BEST OVERALL SCORES): a player's score against hst.dat's block A
 * ([0x70803C], class * 0x258 + 0x21C, the lowest at +0x38) */
static int ch_best_scores(fe *f, int overall, char *err, size_t n)
{
    vm *v = f->vm;
    uint32_t tab = vm_rd32(v, 0x0070803Cu);
    if (!tab) return CH_NONE;                   /* no var\hst.dat: no table, no record */
    while (overall || f->ch_k < (int)vm_rd32(v, 0x00708260u)) {
        uint32_t cls = overall ? 4u : (uint32_t)f->ch_k++, pl;
        vm_wr32(v, 0x00708288u, 0);
        for (pl = 0; pl < vm_rd32(v, 0x007082F0u); pl++) {
            uint32_t P;
            vm_wr32(v, 0x007082F8u, pl);
            P = vm_rd32(v, 0x00708DE8u + 4u * pl);
            if (!overall && vm_rd32(v, P + 0x90) != cls) continue;
            if ((int32_t)vm_rd32(v, tab + cls * 0x258u + 0x21Cu + 0x38u) >
                (int32_t)vm_rd32(v, P + (overall ? 0xCCu : 0xB0u))) continue;
            vm_run(f, overall ? 0x004A42F5 : 0x004A4277, "the score's entry");
            vm_wr32(v, 0x00708288u, 1);
        }
        if (vm_rd32(v, 0x00708288u) == 1) {
            if (!overall) vm_wr32(v, 0x0070825Cu, cls);
            f->ch_pc = overall ? CH_FINAL : CH_BESTCH;
            return enter(f, overall ? SC_CHBESTOV : SC_CHBESTCH, err, n);
        }
        if (overall) break;
    }
    return CH_NONE;
}

static int ch_go(fe *f, int pc, char *err, size_t n)
{
    vm *v = f->vm;
    uint32_t e, P;
    int r, guard = 0;
    for (;;) {
        if (++guard > 400) { snprintf(err, n, "the championship's flow ran away"); return -1; }
        switch (pc) {
        case CH_SPLASH_DONE:                                  /* 0x0049CBC5 */
            if (f->snd) snd_speech(f->snd, 0, NULL, 0);       /* 0x0040F57A */
            vm_wr32(v, 0x007082D8u, 0);
            pc = CH_RALLY;
            break;
        case CH_RALLY:                                        /* 0x0049CBE1 */
            vm_wr32(v, 0x007083A8u, 0);
            vm_wr32(v, 0x007082A4u, 0);
            vm_wr32(v, 0x007082A0u, 0);
            vm_wr32(v, 0x00708290u, 0);
            vm_wr32(v, 0x00708294u, 1);
            vm_wr32(v, 0x00707BC4u, 0);
            pc = CH_ENTRY;
            break;
        case CH_ENTRY:                                        /* 0x0049CC1D */
            vm_wr32(v, 0x0070CA88u, 0);
            e = ch_entry(f, 0);
            switch (vm_rd32(v, e)) {
            case 3:                                           /* the end      */
                vm_wr32(v, 0x00708290u, 1);
                pc = CH_EOR;
                break;
            case 5:                                           /* a rally's start */
                vm_wr32(v, 0x0070828Cu, vm_rd32(v, e + 0x38));
                ch_inc(f, A_CHSTAGE);
                vm_wr32(v, 0x0070829Cu, 1);
                if (f->a8)       /* 0x004A61DA: the A8's own copy */
                    vm_run(f, 0x004A65C9, "FUN_004A65C9 (the A8's new career)");
                else
                    vm_run(f, 0x0046F209, "FUN_0046F209 (the new career)");
                f->ch_phase = PH_EXIT;
                f->ch_pc = CH_ENTRY;
                return enter(f, SC_CHWELCOME, err, n);
            case 4:                                           /* a rally's end */
                pc = CH_EOR;
                break;
            case 1:                                           /* a service halt */
                vm_wr32(v, 0x007082A0u, 1);
                vm_wr32(v, 0x007082A4u, vm_rd32(v, e + 0x14));   /* the budget  */
                vm_wr32(v, 0x007082A8u, vm_rd32(v, e + 0x0C));   /* the time of day */
                vm_wr32(v, 0x007082ACu, 1);
                vm_wr32(v, 0x007082B4u, 1);
                ch_inc(f, 0x00707BC8u);
                ch_inc(f, A_CHSTAGE);
                pc = CH_ENTRY;
                break;
            case 0:                                           /* a stage      */
                f->ch_phase = PH_PRE;
                f->ch_pc = CH_LEG_DONE;
                return enter(f, SC_CHLEG, err, n);
            default:
                snprintf(err, n, "the itinerary entry 0x%08X has kind %u (guru 0x004D5C9D)",
                         e, vm_rd32(v, e));
                return -1;
            }
            break;
        case CH_LEG_DONE:
            f->ch_pc = CH_NEXT_DONE;
            return enter(f, SC_CHNEXT, err, n);
        case CH_NEXT_DONE:                                    /* FUN_0049D26A */
            if (f->snd) snd_speech(f->snd, 0, NULL, 0);       /* 0x0047043C */
            vm_wr32(v, 0x0070CC28u, 0);
            vm_wr32(v, 0x007082A4u, vm_rd32(v, 0x007082A4u) + 0x384u);
            vm_wr32(v, 0x00708294u, 1);
            vm_wr32(v, 0x007082F8u, 0);
            P = vm_rd32(v, 0x00708DE8u);
            if (vm_rd32(v, P + 0xD0) == 1 && vm_rd32(v, 0x007082A0u) == 1) {
                /* 0x0049D846 / 0x0049D981, the service area's picture
                 * sliding in with waits of 1,000 and 250 ms: not kept */
                vm_wr32(v, 0x0070CBF4u, 0);
                vm_wr32(v, 0x0070CAA0u, 0);
                vm_run(f, 0x0047A87F, "FUN_0047A87F");
                vm_run(f, 0x0046DA16, "FUN_0046DA16");
                f->ch_pc = CH_SVC_DONE;
                return enter(f, SC_SETUP, err, n);            /* 0x0049D324   */
            }
            pc = CH_SVC_END;
            break;
        case CH_SVC_DONE:                                     /* 0x0049D337   */
            vm_wr32(v, 0x007082F8u, 0);
            vm_run(f, 0x0049D5A9, "FUN_0049D5A9 (the car's state)");
            P = vm_rd32(v, 0x00708DE8u);
            if (vm_rd32(v, P + 0xD0) == 2) {
                vm_run(f, 0x0049D3BA, "FUN_0049D3BA");
                f->ch_phase = PH_PRE;     /* RETIRE's yes: 0x0049D364 -> 0x0049CE19 */
                f->ch_pc = CH_SVC_INSP;
                printf("frontend: the INSPECTION AREA (after the service)\n");
                return enter(f, SC_CHINSPECT, err, n);        /* 0x0049D358   */
            }
            pc = CH_SVC_PEN;
            break;
        case CH_SVC_INSP:                                     /* 0x0049D35D   */
            ch_inc(f, 0x007082E0u);                           /* 0x0049D366   */
            pc = CH_SVC_PEN;
            break;
        case CH_SVC_PEN:                                      /* 0x0049D36C   */
            P = vm_rd32(v, 0x00708DE8u);
            vm_wr32(v, P + 0x288, vm_rd32(v, 0x0070CC28u) * 100u);
            pc = CH_SVC_END;
            break;
        case CH_SVC_END:                                      /* 0x0049D395   */
            if (vm_rd32(v, 0x007082E0u) == vm_rd32(v, 0x007082F0u))
                vm_wr32(v, 0x00708294u, 0);
            vm_wr32(v, 0x007082A0u, 0);
            pc = vm_rd32(v, 0x00708294u) == 1 ? CH_RACE : CH_RET;   /* 0x0049CE37 */
            break;
        case CH_RACE:                                         /* FUN_0046F785 */
            f->ch_launch = 0;
            vm_wr32(v, 0x0070CF94u, 0);       /* the launcher's gof: the race's, later */
            if (fe_vmcall(f, 0x0046F785, NULL) != 0)
                fprintf(stderr, "frontend: FUN_0046F785 could not run: %s\n", orig_error(v));
            if (f->ch_launch) {
                f->track = (int32_t)vm_rd32(v, 0x0070CC3Cu);   /* 0x0046F888 */
                f->crsdirec = (int32_t)vm_rd32(v, 0x0070CC58u);
                f->ch_pc = CH_POSTRACE;
                su_read_sliders(f);
                f->launched = 1;
                return FE_LAUNCH;
            }
            pc = CH_POSTRACE;                 /* retired: no race (0x0046F841) */
            break;
        case CH_POSTRACE:                                     /* 0x0049CEF0   */
            if (vm_rd32(v, 0x00708294u) == 0) { pc = CH_RET; break; }
            vm_run(f, 0x0049E231, "FUN_0049E231 (the totals)");
            vm_run(f, 0x0049E7F9, "FUN_0049E7F9 (the stage results)");
            vm_run(f, 0x004775CB, "FUN_004775CB (by stage time)");
            f->ch_k = vm_rd32(v, 0x0070CAA8u) == 2 ? 1 : 0;
            f->ch_phase = PH_POST;
            pc = CH_CLASS;
            break;
        case CH_CLASS:                                        /* FUN_0049DB6D */
            if ((r = ch_loop(f, &CL_STAGE, CH_CLASS, err, n)) != CH_NONE) return r;
            if (f->a8) { pc = CH_OVER_DONE; break; }   /* 0x004A6408: no OVERALL STAGE RESULTS */
            vm_run(f, 0x004A1683, "FUN_004A1683");
            vm_run(f, 0x0049DD17, "FUN_0049DD17 (the positions)");
            return ch_show(f, 0x0049DC6C, 0, CH_OVER_DONE, err, n);
        case CH_OVER_DONE:                                    /* 0x0049CF69   */
            if ((int32_t)vm_rd32(v, 0x007082B8u) >= 1) {
                uint32_t k = ch_kind_next(f);
                if (k == 3 || k == 4) vm_wr32(v, 0x0070CA88u, 1);
                vm_run(f, 0x0049E7F9, "FUN_0049E7F9 (the stage results)");
                vm_run(f, 0x004776DE, "FUN_004776DE (by total)");
                f->ch_k = 0;
                f->ch_after = CH_D016;
                pc = CH_RCLASS;
                break;
            }
            pc = CH_D016;
            break;
        case CH_RCLASS:                                       /* FUN_0049EF26 */
            if ((r = ch_loop(f, &CL_RALLY, CH_RCLASS, err, n)) != CH_NONE) return r;
            if (f->a8) { pc = f->ch_after; break; }    /* 0x004A6467 / 0x004A63BD: no overall standings */
            vm_run(f, 0x004A1683, "FUN_004A1683");
            vm_run(f, 0x0049DD17, "FUN_0049DD17 (the positions)");
            return ch_show(f, 0x0049F81D, 0, f->ch_after, err, n);
        case CH_D016:                                         /* 0x0049D016   */
            vm_run(f, 0x0049FB06, "FUN_0049FB06 (the top speed)");
            vm_wr32(v, 0x00708288u, 0);
            /* the stage's best times: hst.dat's block D, [0x708048] - with
             * no file there is no table and no record (as the arcade's) */
            if (vm_rd32(v, 0x00708048u)) vm_run(f, 0x0049F85B, "FUN_0049F85B (the best times)");
            if (vm_rd32(v, 0x00708288u) == 1) {
                f->ch_pc = CH_BEST_DONE;
                return enter(f, SC_CHBEST, err, n);           /* 0x0049D029   */
            }
            pc = CH_BEST_DONE;
            break;
        case CH_BEST_DONE: {                                  /* 0x0049D05F   */
            uint32_t k = ch_kind_next(f);
            if (k == 3) { pc = CH_NEXTSTAGE; break; }
            if (k == 4) { pc = CH_EOR; break; }
            if (k == 1) {
                vm_wr32(v, 0x007082ACu, 1);
                vm_wr32(v, 0x007082B4u, 1);
                pc = CH_REVIEW;
                break;
            }
            vm_wr32(v, 0x007082B4u, 0);
            /* FUN_004A412F, the cars' state (0x0049D09B), in C: its loop
             * calls the INSPECTION AREA in the middle */
            vm_wr32(v, 0x00708294u, 1);                       /* 0x004A4130   */
            f->ch_k = 0;
            pc = CH_INSP;
            break;
        }
        case CH_INSP:                                         /* 0x004A413F   */
            while (f->ch_k < (int)vm_rd32(v, 0x007082F0u)) {
                vm_wr32(v, 0x007082F8u, (uint32_t)f->ch_k++);
                P = vm_rd32(v, 0x00708DE8u + 4u * vm_rd32(v, 0x007082F8u));
                if (vm_rd32(v, P + 0xD0) != 1) continue;
                setup_costs(v);                                   /* 0x004A3047, C (setup_check) */
                vm_run(f, 0x0049D5D9, "FUN_0049D5D9 (the car's state)");
                vm_run(f, 0x0049D3BA, "FUN_0049D3BA");
                if (vm_rd32(v, P + 0xD0) != 2) continue;
                ch_inc(f, 0x007082E0u);                       /* 0x004A4182   */
                /* 0x0049D846, the 1,000 ms wait, 0x0049D981: the slides,
                 * not kept (vh_nop) */
                f->ch_phase = PH_POST;    /* RETIRE's yes: 0x004A41A8 -> 0x0049D0A9 */
                f->ch_pc = CH_INSP_DONE;
                printf("frontend: the INSPECTION AREA (after the stage)\n");
                return enter(f, SC_CHINSPECT, err, n);        /* 0x004A419C   */
            }
            if (vm_rd32(v, 0x007082E0u) == vm_rd32(v, 0x007082F0u))   /* 0x004A41C6 */
                vm_wr32(v, 0x00708294u, 0);
            pc = vm_rd32(v, 0x00708294u) == 0 ? CH_RET : CH_REVIEW;   /* 0x0049D0D1 */
            break;
        case CH_INSP_DONE:                                    /* 0x004A41AA   */
            /* 0x0049DA2C, the failed car's slide, and 1,250 ms: not kept */
            pc = CH_INSP;
            break;
        case CH_REVIEW:                                       /* 0x0049D0DE   */
            f->ch_phase = PH_EXIT;
            f->ch_pc = CH_NEXTSTAGE;
            return enter(f, vm_rd32(v, 0x007082B4u) == 1 ? SC_CHREVIEW : SC_CHREVIEW2,
                         err, n);
        case CH_NEXTSTAGE:                                    /* 0x0049D10E   */
            ch_inc(f, A_CHSTAGE);
            ch_inc(f, 0x007082B8u);
            if (!f->a8) vm_wr32(v, 0x00707BC8u, 0);           /* 0x0049D11A; the A8's 0x004A6590 has none */
            pc = CH_ENTRY;
            break;
        case CH_RET:                                          /* 0x0049CE44   */
            vm_wr32(v, 0x0070CA88u, 1);
            vm_wr32(v, 0x00707EC9u, 0);
            vm_wr32(v, 0x00707ECDu, 0);
            vm_wr32(v, 0x0070AB6Eu, 0);
            vm_wr32(v, 0x00708294u, 0);
            vm_run(f, 0x0046E580, "FUN_0046E580 (the rest of the rally)");
            vm_run(f, 0x0049E7F9, "FUN_0049E7F9 (the stage results)");
            vm_run(f, 0x004776DE, "FUN_004776DE (by total)");
            f->ch_k = 0;
            f->ch_after = CH_EOR;
            f->ch_phase = PH_EXIT;
            pc = CH_RCLASS;
            break;
        case CH_EOR:                                          /* 0x0049CC8D   */
            vm_run(f, 0x004A0115, "FUN_004A0115 (the standings)");
            vm_run(f, 0x004A02EF, "FUN_004A02EF");
            vm_run(f, 0x004A045F, "FUN_004A045F");
            vm_run(f, 0x004A02EF, "FUN_004A02EF");
            vm_run(f, 0x004A1776, "FUN_004A1776");
            f->ch_k = (vm_rd32(v, 0x0070CAA8u) != 2 && vm_rd32(v, 0x00708038u) == 0x13) ? 0 : 1;
            f->ch_phase = PH_EXIT;
            pc = CH_CCLASS;
            break;
        case CH_CCLASS:                                       /* FUN_004A0691 */
            if ((r = ch_loop(f, &CL_CHAMP, CH_CCLASS, err, n)) != CH_NONE) return r;
            if (f->a8) { pc = CH_CHAMP_DONE; break; }         /* 0x004A6221: no OVERALL CHAMPIONSHIP */
            vm_run(f, 0x004A02EF, "FUN_004A02EF");
            vm_run(f, 0x004A05A8, "FUN_004A05A8");
            vm_run(f, 0x004A169E, "FUN_004A169E");
            vm_run(f, 0x004A02EF, "FUN_004A02EF");
            vm_run(f, 0x004A18E6, "FUN_004A18E6");
            vm_run(f, 0x004A1707, "FUN_004A1707 (the positions)");
            return ch_show(f, 0x004A16B6, 0, CH_CHAMP_DONE, err, n);
        case CH_CHAMP_DONE:                                   /* 0x0049CCE8   */
            if (vm_rd32(v, 0x00708290u) == 1) { pc = CH_END; break; }
            /* FUN_004A5009, the rally's finish routine, its scene left out */
            finish_routine(f, 0x004A5009, "the rally's finish routine");
            f->ch_pc = CH_REVIEWR_DONE;
            return enter(f, SC_CHREVIEWR, err, n);            /* 0x0049CD07   */
        case CH_REVIEWR_DONE:                                 /* 0x0049CD0C   */
            vm_wr32(v, 0x00707BC4u, 0);
            vm_wr32(v, 0x00708294u, 1);
            ch_inc(f, A_CHRALLY);
            vm_wr32(v, A_CHSTAGE, 0);
            if (f->a8)                                        /* 0x004A6296 */
                vm_run(f, 0x004A65C9, "FUN_004A65C9 (the A8's new career)");
            else
                vm_run(f, 0x0046F209, "FUN_0046F209 (the new career)");
            pc = CH_RALLY;
            break;
        case CH_END:                                          /* 0x0049D129   */
            ch_unlock(f);
            if (vm_rd32(v, 0x0070803Cu)) vm_run(f, 0x004A4D6D, "FUN_004A4D6D");
            f->ch_k = 0;
            pc = CH_BESTCH;
            break;
        case CH_BESTCH:                                       /* FUN_004A41E1 */
            if ((r = ch_best_scores(f, 0, err, n)) != CH_NONE) return r;
            pc = f->a8 ? CH_EXIT : CH_BESTOV;     /* 0x004A65B9: the A8 ends here, no BEST OVERALL, no REVIEW */
            break;
        case CH_BESTOV:                                       /* FUN_004A4D92 */
            if ((r = ch_best_scores(f, 1, err, n)) != CH_NONE) return r;
            pc = CH_FINAL;
            break;
        case CH_FINAL:                                        /* 0x0049D14F   */
            f->ch_phase = PH_EXIT;
            f->ch_pc = CH_EXIT;
            return enter(f, SC_CHREVIEWF, err, n);
        case CH_EXIT:
            return ch_exit(f, err, n);
        case CH_LB_DONE:
            return ch_lb_done(f, err, n);
        default:
            snprintf(err, n, "the championship's flow: no label %d", pc);
            return -1;
        }
    }
}

/* NEW SIMULATION's one player, 0x0049E278: the player record's race fields
 * cleared (0x00482F5F, twice), the menu's words, then car select from the
 * championship's list (0x0049E2E8) */
static int ch_new(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    uint32_t regs[8];
    memset(regs, 0, sizeof regs);
    vm_wr32(v, 0x007082F8u, 0);
    if (fe_vmcall(f, 0x00482F5F, regs) != 0)
        fprintf(stderr, "frontend: FUN_00482F5F could not run: %s\n", orig_error(v));
    vm_wr32(v, 0x0070AB6Eu, 0);
    vm_wr32(v, 0x007082BCu, 4);
    vm_wr32(v, 0x00708E08u, 0xF);
    vm_wr32(v, 0x007082F0u, 1);
    vm_wr32(v, A_CHSTAGE, 0);
    vm_wr32(v, A_CHRALLY, 0);
    vm_wr32(v, 0x007082B8u, 0);
    vm_wr32(v, 0x007082F8u, 0);
    if (fe_vmcall(f, 0x00482F5F, regs) != 0)
        fprintf(stderr, "frontend: FUN_00482F5F could not run: %s\n", orig_error(v));
    f->mode = 1;
    f->a8 = 0;
    set_carlist(f, A_CHCARS);
    car_index_of(f);
    return enter(f, SC_CAR, err, n);
}

/* The A8's one player, 0x004A586B: [0x7082BC] = 5, [0x7082F0] = 1, and
 * before the car select - unlike the Mobil 1's - the new career's
 * FUN_0046F209, 0x0046CFFE and 0x0047A87F (unless a load is under way), the
 * rally and entry cleared, then car select from the A8's own list (0x70C022,
 * six cars); its accept is the same 0x004775A4 and [0x7082F4] = 9, then
 * FUN_004A60A7 (ch_begin) */
static int a8_new(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    uint32_t regs[8];
    memset(regs, 0, sizeof regs);
    f->a8 = 1;
    vm_wr32(v, 0x007082BCu, 5);
    vm_wr32(v, 0x0070AB6Eu, 0);
    vm_wr32(v, 0x007082F0u, 1);
    if (vm_rd32(v, 0x007082D8u) != 1) {
        vm_run(f, 0x0046F209, "FUN_0046F209 (the new career)");
        vm_run(f, 0x0046CFFE, "FUN_0046CFFE");
        vm_run(f, 0x0047A87F, "FUN_0047A87F");
    }
    vm_wr32(v, 0x007082D8u, 0);
    vm_wr32(v, 0x007082C0u, 0);
    vm_wr32(v, 0x007082C4u, 0);
    vm_wr32(v, 0x007082B8u, 0);
    vm_wr32(v, 0x007082F8u, 0);
    if (fe_vmcall(f, 0x00482F5F, regs) != 0)
        fprintf(stderr, "frontend: FUN_00482F5F could not run: %s\n", orig_error(v));
    f->mode = 1;
    set_carlist(f, A_A8CARS);
    car_index_of(f);
    return enter(f, SC_CAR, err, n);
}

/* FUN_0049CB25's prologue up to the welcome picture - called after car
 * select's accept (0x0049E301, which first sets [0x7082F4] = 9) or,
 * by LOAD CHAMPIONSHIP's select (0x0046B52D) with [0x7082D8] = 1 */
static int ch_begin(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    int loaded = vm_rd32(v, 0x007082D8u) == 1;
    /* the A8's 0x004A60A7 has no "[0x7082B8] = 2 for a loaded one" (0x0049CB2F) */
    if (loaded && !f->a8) vm_wr32(v, 0x007082B8u, 2);               /* a loaded one */
    vm_wr32(v, 0x00707BC4u, 0);
    if (vm_rd32(v, 0x007082D8u) != 1) {
        vm_run(f, 0x0046F209, "FUN_0046F209 (the new career)");
        vm_run(f, 0x0046D70C, "FUN_0046D70C (the rivals shuffled)");
        vm_run(f, 0x0046DA2D, "FUN_0046DA2D");
        vm_run(f, 0x0046CFFE, "FUN_0046CFFE");
        vm_run(f, 0x0047A87F, "FUN_0047A87F");
        vm_wr32(v, 0x00707BC8u, 0);
        vm_run(f, 0x004A5249, "FUN_004A5249");
    }
    vm_run(f, 0x00464AE0, "FUN_00464AE0 (the units)");
    vm_run(f, 0x0046C8A9, "FUN_0046C8A9");
    vm_wr32(v, A_CHRETIRE, 0);
    vm_wr32(v, 0x00707ECDu, 0);
    vm_wr32(v, 0x00707EC9u, 0);
    if (f->a8) {
        /* 0x004A60E3 clears the load flag BEFORE the picture, so a loaded A8
         * championship shows it too (0x0047B48A has no test of it); the mode
         * is 0x13 and [0x70CAA8] 1 (0x004A6115 / 0x004A611F) */
        vm_wr32(v, 0x007082D8u, 0);
        vm_wr32(v, 0x00708038u, 0x13);
        vm_wr32(v, 0x0070CAA8u, 1);
        vm_wr32(v, 0x007082F8u, 0);
        f->ch_phase = PH_EXIT;
        f->ch_pc = CH_SPLASH_DONE;
        return enter(f, SC_CHSPLASH, err, n);           /* 0x0047B48A          */
    }
    vm_wr32(v, 0x00708038u, 2);                         /* the Mobil 1's mode  */
    vm_wr32(v, 0x0070CAA8u, 2);
    vm_wr32(v, 0x007082F8u, 0);
    if (vm_rd32(v, 0x007082D8u) == 1) return ch_go(f, CH_SPLASH_DONE, err, n);
    f->ch_phase = PH_EXIT;
    f->ch_pc = CH_SPLASH_DONE;
    return enter(f, SC_CHSPLASH, err, n);               /* 0x0047B3D7          */
}

/* ESC on a championship screen: [0x70AB6E] = 1 (0x00462D65), and the
 * screen's body calls 0x0046C6E7 - RETIRE, "are you sure?" */
static int ch_retire(fe *f, char *err, size_t n)
{
    f->ret_sc = f->sc;
    vm_wr32(f->vm, A_CHRETIRE, 0);                      /* 0x0046C6E7          */
    return enter(f, SC_RETIRE, err, n);
}

/* RETIRE has gone: yes ([0x707BC0] = 1) leaves as the phase says; no, or
 * ESC, goes back to the screen, whose body starts it again */
static int ch_retired(fe *f, int esc, char *err, size_t n)
{
    vm *v = f->vm;
    if (f->ret_sc == SC_CHINSPECT)                      /* champ-race reads it */
        printf("frontend: the INSPECTION AREA's RETIRE?: [0x707BC0] = %u%s\n",
               vm_rd32(v, A_CHRETIRE), esc ? " (ESC)" : "");
    if (esc || vm_rd32(v, A_CHRETIRE) != 1) {
        vm_wr32(v, A_CHRETIRE, 0);
        vm_wr32(v, 0x00707ED5u, 0);                     /* REVIEW's top, 0x00479678 */
        return enter(f, f->ret_sc, err, n);
    }
    if (f->ch_phase == PH_EXIT) return ch_exit(f, err, n);
    if (f->ret_sc == SC_CHINSPECT && f->ch_phase == PH_PRE)
        vm_wr32(v, 0x007082A0u, 0);                     /* 0x0049D3AE   */
    vm_wr32(v, 0x00707BC4u, 1);                         /* 0x0049CDC1          */
    vm_run(f, 0x0047D46C, "FUN_0047D46C (every player out)");
    if (f->ret_sc == SC_CHINSPECT)                      /* champ-race reads it */
        printf("frontend: the INSPECTION AREA's RETIRE? yes: [0x707BC4] = %u, out of the rally\n",
               vm_rd32(v, 0x00707BC4u));
    vm_wr32(v, A_CHRETIRE, 0);
    return ch_go(f, f->ch_phase == PH_PRE ? CH_RACE : CH_RET, err, n);
}

/* The port's (not the original's): the main menu's quit asks first, on the
 * original's own RETIRE screen, "are you sure?" with its no / yes - titled
 * EXIT GAME (tokExitT, which ENGLISH.TXT keeps for a quit screen the original
 * never opens: tok_text) - a remaster guard against a stray back press.  The
 * answer is the screen's [0x707BC0], as a championship's is, but the
 * championship's code (ch_retired) does not see it. */
static int quit_ask(fe *f, char *err, size_t n)
{
    f->quit_screen = 1;
    vm_wr32(f->vm, A_CHRETIRE, 0);                      /* 0x0046C6E7          */
    return enter(f, SC_RETIRE, err, n);
}

/* yes ([0x707BC0] = 1) quits as the original's exit does; no, or ESC, is the
 * main menu again, its cursor where it was */
static int quit_answered(fe *f, int esc, char *err, size_t n)
{
    int yes = !esc && vm_rd32(f->vm, A_CHRETIRE) == 1;
    f->quit_screen = 0;
    vm_wr32(f->vm, A_CHRETIRE, 0);
    if (yes) {
        save_dat(f);                                    /* 0x00414FDA */
        return FE_QUIT;
    }
    return enter(f, SC_MAIN, err, n);
}

/* REVIEW OPTIONS' leaderboards: "rally leaderboard" (0x004A50BC) and
 * "championship leaders" (0x004A5153), each a results screen and back */
static int ch_leaderboard(fe *f, uint32_t h, char *err, size_t n)
{
    vm *v = f->vm;
    f->ch_lb = f->sc;
    f->ch_lb_pc = f->ch_pc;                             /* REVIEW's own resume */
    f->ch_pc = CH_LB_DONE;
    if (h == H_CH_RALLYLB) {
        vm_wr32(v, 0x0070CA44u, 1);
        vm_run(f, 0x0049E7F9, "FUN_0049E7F9 (the stage results)");
        vm_run(f, 0x004776DE, "FUN_004776DE (by total)");
        vm_run(f, 0x004A1683, "FUN_004A1683");
        vm_run(f, 0x0049DD17, "FUN_0049DD17 (the positions)");
        return ch_show(f, 0x0049F81D, 0, CH_LB_DONE, err, n);
    }
    vm_run(f, 0x004A0115, "FUN_004A0115 (the standings)");
    vm_run(f, 0x004A02EF, "FUN_004A02EF");
    vm_run(f, 0x004A18E6, "FUN_004A18E6");
    vm_run(f, 0x004A1707, "FUN_004A1707 (the positions)");
    vm_wr32(v, 0x0070CA9Cu, 1);
    return ch_show(f, 0x004A16B6, 0, CH_LB_DONE, err, n);
}

static int ch_lb_done(fe *f, char *err, size_t n)
{
    vm_wr32(f->vm, 0x0070CA44u, 0);
    vm_wr32(f->vm, 0x0070CA9Cu, 0);
    f->ch_pc = f->ch_lb_pc;
    return enter(f, f->ch_lb, err, n);
}

static int ch_screen(int sc)
{
    return (sc >= SC_CHSPLASH && sc <= SC_RETIRE) || sc == SC_CHINSPECT;
}

/* SELECT and ESC on the championship's screens */
static int ch_leave(fe *f, uint32_t h, int esc, char *err, size_t n)
{
    if (f->sc == SC_RETIRE && f->quit_screen) return quit_answered(f, esc, err, n);
    if (f->sc == SC_RETIRE) return ch_retired(f, esc, err, n);
    if (esc) return ch_retire(f, err, n);
    if (f->ch_pc == CH_LB_DONE && h == H_BACK) return ch_lb_done(f, err, n);
    switch (f->sc) {
    case SC_CHREVIEW: case SC_CHREVIEW2: case SC_CHREVIEWR: case SC_CHREVIEWF:
        if (h == H_CH_RALLYLB || h == H_CH_CHAMPLB) return ch_leaderboard(f, h, err, n);
        if (h == H_REVIEW_MAIN) {                       /* [0x707ED5] = 1: RETIRE */
            vm_wr32(f->vm, 0x00707ED5u, 1);
            return ch_retire(f, err, n);
        }
        if (h == H_CHSAVE) {                            /* save rally */
            f->chsave_ret = f->sc;
            return enter(f, SC_CHSAVE, err, n);
        }
        if (h != H_BACK) { unported(f, h, "this menu item"); return 0; }
        break;
    default:
        if (h != H_BACK) return 0;
    }
    if (f->snd && f->sc == SC_CHWELCOME) snd_speech(f->snd, 0, NULL, 0);   /* 0x0046EB0A */
    return ch_go(f, f->ch_pc, err, n);
}

/* ----: OPTIONS and what it opens ------------------------------------
 *
 * The main menu's item 4 (0x00473428) and the screens under it, read from
 * the bodies.  Every body is the same loop:
 * a few stores, the art (FUN_0047B0C2), the cursor from its own dword, an
 * entry list run once (0x00464E44, ESI), then each frame the pump with the
 * pre-draw list and the generic key handler 0x00462C28; a SELECT whose item
 * arg (+0x54) is 2 stays, BACK / ESC ([0x707ECD]) returns to the body that
 * called it, which starts again from its top.  The port keeps the flow (the
 * transitions below) and RUNS everything else as the original's code: every
 * arrow callback and select handler (EDI = the item / the table, as
 * 0x00470DB1 and 0x00470CCE pass them), every pre-draw, every entry list.
 * The state is the original's globals and the player record, in the
 * interpreter - var\ral.dat saves them (0x004A69BB), and the launch reads
 * them (fe_launch_get). */
static const struct {
    int      sc;
    uint32_t body;          /* the screen's body: the handler that opens it   */
    uint32_t art;           /* LEA EAX,[art] before CALL 0x0047B0C2           */
    uint32_t cur;           /* the cursor's own dword                         */
    uint32_t pre[2];        /* the pre-draw list ([0x707E64])                 */
    uint32_t entry;         /* the entry list (0x00464E44), run once          */
} OPT[] = {
    { SC_OPTIONS,  0x00473428, 0x006E47FA, 0x006FA018, { 0, 0 }, 0 },
    { SC_PDET,     0x004736EE, 0x006E47D7, 0x006FA01C, { 0, 0 }, 0 },
    { SC_P1DET,    0x004739AA, 0x006E474B, 0x006FA020, { 0x0046C4F6, 0 }, 0 },
    { SC_AUDIO,    0x00472B05, 0x006E4B15, 0x006FA03C, { 0x00472DB4, 0 }, 0 },
    { SC_VISPREF,  0x00476547, 0x006E4861, 0x006FA08C, { 0x004767E2, 0 }, 0 },
    { SC_REDEF,    0x004759DF, 0x006E4D63, 0x006FA0BC, { 0x0047949B, 0x0047906A }, 0 },
    { SC_CTRL,     0x00474464, 0x006E4C3B, 0x006FA09C, { 0x004671BD, 0 }, 0x00467C2E },
    { SC_CTRLPROG, 0x0047470E, 0x006E4C60, 0x006FA0A0, { 0x00467E9A, 0x00467592 }, 0 },
    /* INFORMATION (0x00489E3A) and its tables.  Pre-draw and entry
     * lists are the ones each body loads into [0x707E64] / ESI: BEST TIMES
     * 0x70B02A / 0x70B62E, STAGE INFORMATION 0x70B01A / 0x70B652, TOP SCORES
     * 0x70B032 / 0x70B636.  STAGE INFORMATION has no art of its own: its map
     * is 0x00465011's (load_stage_art) */
    { SC_INFO,       0x00489E3A, 0x006E481C, 0x006FA030, { 0, 0 }, 0 },
    { SC_INFOTIMES,  0x0048A165, 0x006E49ED, 0x006FA068, { 0x0048A411, 0 }, 0x00465ED7 },
    { SC_INFOSCORES, 0x0048ABF3, 0x006E4A11, 0x006FA06C, { 0x00469D88, 0x00469DA9 }, 0x00469D88 },
    { SC_INFOSTAGE,  0x0048A8F5, 0,          0x006FA05C, { 0x0047DB0E, 0 }, 0x00466062 },
    /* VISUAL OPTIONS, the body 0x0047C655: art 0x6E4885, cursor
     * [0x6FA090], pre-draw list 0x70AFD6 (0x0046CA47 only), no entry list */
    { SC_VISOPT,     0x0047C655, 0x006E4885, 0x006FA090, { 0x0046CA47, 0 }, 0 },
    /* CAR SPECIFICATIONS, the body 0x0047696D: no art of its own (the
     * page's, per car: 0x004650C4), cursor [0x6FA048], pre-draw list 0x70AFFE
     * (0x004664C9 is the carousel's clock, m3d_frame's), post-draw 0x00478F77 */
    { SC_CSPECS,     0x0047696D, 0,          0x006FA048, { 0x004667D6, 0x004667FC }, 0 },
    /* SLIDESHOW, the body 0x00476C7A: the car's pictures (0x0046512D, the host's
     * art hooks), cursor [0x6FA04C], pre-draw list 0x70B00E (the car's name 0x004667D6 and
     * the picture row's 0x0046681F), empty draw and post-draw lists */
    { SC_SLIDES,     0x00476C7A, 0,          0x006FA04C, { 0x004667D6, 0x0046681F }, 0 }
};
#define N_OPT ((int)(sizeof OPT / sizeof OPT[0]))
#define H_OPTIONS 0x00473428u            /* the main menu's item 4            */

static int opt_index(int sc)
{
    int k;
    for (k = 0; k < N_OPT; k++) if (OPT[k].sc == sc) return k;
    return -1;
}

static int opt_screen(int sc)
{
    return opt_index(sc) >= 0;
}

/* the item record of the cursor's item, as 0x0046E362 finds it */
static uint32_t opt_item_va(const fe *f, int id)
{
    return f->tab[f->sc].va + 4u + 0x5Cu * (uint32_t)(id - 1);
}

/* Original code with the registers the engine hands it. */
static void opt_call(fe *f, uint32_t va, uint32_t edi, uint32_t ebx, uint32_t ebp,
                     const char *what)
{
    uint32_t r[8];
    memset(r, 0, sizeof r);
    r[VM_EDI] = edi;
    r[VM_EBX] = ebx;
    r[VM_EBP] = ebp;
    vm_wr32(f->vm, 0x00707E91u, (uint32_t)f->cursor);
    vm_wr32(f->vm, 0x00707EF5u, f->tab[f->sc].va);   /* the screen's table: the
                                                        item lookup 0x0046E362 */
    if (fe_vmcall(f, va, r) != 0 && !f->vm_bad) {
        fprintf(stderr, "frontend: %s (0x%08X) could not run: %s\n", what, va,
                orig_error(f->vm));
        f->vm_bad = 1;
    }
    gfx2d_set_clip(&f->surf, 0, 0, W - 1, H - 1);
}

/* What AUDIO SETUP's sliders wrote, into the sound layer's copy of the
 * globals: the menu copies the requests read,
 * the two music volumes and the in-game music switch.  The overall volume
 * and the music volume reach it through their own hooks, as commands. */
static void opt_sound_sync(fe *f)
{
    snd *s = f->snd;
    vm *v = f->vm;
    if (!s || !v) return;
    s->set.music_menu     = (int32_t)vm_rd32(v, 0x00511E8Cu);
    s->set.music_race     = (int32_t)vm_rd32(v, 0x00511E90u);
    s->set.music_race_set = (int32_t)vm_rd32(v, 0x00707FF4u);
    s->set.fx_menu        = (int32_t)vm_rd32(v, 0x00707FF8u);
    s->set.fx_race        = (int32_t)vm_rd32(v, 0x00707FFCu);
    s->set.speech_menu    = (int32_t)vm_rd32(v, 0x00708000u);
    s->set.codriver       = (int32_t)vm_rd32(v, 0x00708004u);
    s->set.engine         = (int32_t)vm_rd32(v, 0x00708008u);
    s->set.race_music_on  = (int32_t)vm_rd32(v, 0x0070CC68u);
}

/* After any of the tree's code ran: the items it rewrote, the sound */
static void opt_after(fe *f)
{
    load_table_from(f, &f->tab[f->sc], sc_table[f->sc], 1);
    opt_sound_sync(f);
}

/* ---- the platform under the tree's code (hooked while it is open) ---- */

/* 0x00409A8B: command 8, the overall volume (EAX) */
static void vh_overall(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    (void)v;
    if (f->snd) snd_overall(f->snd, (int32_t)r[VM_EAX]);
}

/* 0x0040F651: the music stream's volume (EAX), when it differs */
static void vh_musvol(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    (void)v;
    if (f->snd) snd_music_volume(f->snd, (int32_t)r[VM_EAX]);
}

static void vh_fesnd(vm *v, uint32_t r[8], void *ud);

/* 0x00410E0C, timeGetTime: the menu clock */
static void vh_clock(vm *v, uint32_t r[8], void *ud)
{
    (void)v;
    r[VM_EAX] = ((fe *)ud)->now;
}

/* 0x00409290 -> 0x00408897: the device's four words (the scheme block's
 * +0x28 X, +0x2C / +0x30 the pedal axes, +0x34 the buttons) - the port's
 * pad, as car_input_pad builds that block */
static void vh_devwords(vm *v, uint32_t r[8], void *ud)
{
    fe *f = (fe *)ud;
    car_dev d;
    (void)v;
    car_input_pad(&d);
    /* PORT'S: the button that chose a row (A, START) is still down when the
     * row starts waiting for a press, and would be read as the answer.  The
     * original chose rows on the keyboard, so it never met this: nothing is
     * read until the pad has been at rest. */
    if (!f->ctl_armed) {
        if (d.buttons) d.buttons = 0;
        else f->ctl_armed = 1;
    }
    r[VM_ECX] = (uint32_t)d.x;
    r[VM_EDX] = (uint32_t)d.ydown;
    r[VM_EAX] = (uint32_t)d.yup;
    r[VM_EBX] = d.buttons;
}

/* The device list SETUP CONTROLLER cycles through - [0x4F28A0] devices,
 * 0x50-byte records from 0x4F28A4 (+0x14 the axes, +0x1C force feedback,
 * +0x30 the name) - is DirectInput's enumeration, which the port does not
 * have (the platform is a remaster): THE PORT'S, one device, its pad, with
 * six axes and no force feedback, and the pad's DIJOYSTATE (0x4F19F0) each
 * frame for the axis tests 0x0040929E makes.  A stand-in, in
 */
static void opt_devices(fe *f)
{
    static const char name[] = "GAME CONTROLLER";
    vm *v = f->vm;
    uint32_t k;
    car_dev d;
    vm_wr32(v, 0x004F28A0u, 1);
    vm_wr32(v, 0x004F28A4u + 0x14u, 0x3Fu);
    vm_wr32(v, 0x004F28A4u + 0x1Cu, 0);
    for (k = 0; k < sizeof name; k++) vm_wr8(v, 0x004F28A4u + 0x30u + k, (uint8_t)name[k]);
    car_input_pad(&d);
    vm_wr32(v, 0x004F19F0u, (uint32_t)d.x);                 /* lX                 */
    vm_wr32(v, 0x004F19F4u, 0);                             /* lY                 */
    vm_wr32(v, 0x004F19F8u, (uint32_t)d.yup);               /* lZ: the throttle's */
    vm_wr32(v, 0x004F19FCu, (uint32_t)d.ydown);             /* lRx: the brake's   */
    vm_wr32(v, 0x004F1A00u, 0);
    vm_wr32(v, 0x004F1A04u, 0);
}

static void opt_hooks(fe *f, int on)
{
    if (on == f->opt_hooked) return;
    f->opt_hooked = on;
    if (on) {
        vm_hook(f->vm, 0x00409A8B, vh_overall, f);
        vm_hook(f->vm, 0x0040F651, vh_musvol, f);
        vm_hook(f->vm, 0x0040EE7C, vh_fesnd, f);
        vm_hook(f->vm, 0x00410E0C, vh_clock, f);
        vm_hook(f->vm, 0x00409250, vh_nop, f);   /* the device configured, */
        vm_hook(f->vm, 0x00409285, vh_nop, f);   /*   released: DirectInput's */
        vm_hook(f->vm, 0x00409290, vh_devwords, f);
        vm_hook(f->vm, 0x0047B0C2, vh_art, f);   /* CAR SPECIFICATIONS' page art */
        vm_hook(f->vm, 0x00402CA3, vh_exists, f);        /* SLIDESHOW's pictures */
        vm_hook(f->vm, 0x004115E1, vh_slide, f);
        vm_hook(f->vm, 0x00411573, vh_slide_alone, f);
        vm_hook(f->vm, 0x00405E6D, vh_nop, f);
    } else {
        vm_unhook(f->vm, 0x0047B0C2);
        vm_unhook(f->vm, 0x00402CA3);
        vm_unhook(f->vm, 0x004115E1);
        vm_unhook(f->vm, 0x00411573);
        vm_unhook(f->vm, 0x00405E6D);
        vm_unhook(f->vm, 0x00409A8B);
        vm_unhook(f->vm, 0x0040F651);
        vm_unhook(f->vm, 0x0040EE7C);
        vm_unhook(f->vm, 0x00410E0C);
        vm_unhook(f->vm, 0x00409250);
        vm_unhook(f->vm, 0x00409285);
        vm_unhook(f->vm, 0x00409290);
    }
}

/* A body's entry, transcribed */
static int opt_enter(fe *f, int sc, char *err, size_t n)
{
    vm *v = f->vm;
    int k = opt_index(sc), rc;
    uint32_t P;
    if (!ensure_vm(f)) { snprintf(err, n, "out of memory"); return -1; }
    opt_hooks(f, 1);
    opt_devices(f);                  /* DirectInput's list: the boot's, read by
                                        PLAYER 1 DETAILS (0x0046C4F9) too */
    f->sc = sc;
    f->carousel_kind = -1;
    P = setup_player(v);
    switch (sc) {
    case SC_P1DET:                                   /* 0x004739AB..0x004739BF */
        vm_wr32(v, 0x007082ECu, 0);
        vm_wr32(v, 0x007082F8u, 0);
        break;
    case SC_AUDIO:                                   /* 0x00472B06..0x00472B12 */
        vm_wr32(v, 0x006FA03Cu, vm_rd32(v, 0x006FA1D8u));
        vm_wr32(v, 0x00707E3Cu, 1);                  /* LEFT / RIGHT silent */
        break;
    case SC_INFO:                                    /* 0x00489E6B..0x00489E91:
                                                        the 3D clip -40..760,
                                                        as ints and floats */
        vm_wr32(v, 0x005D7070u, 0xFFFFFFD8u);
        vm_wr32(v, 0x005D7074u, 0x000002F8u);
        vm_wr32(v, 0x005D7080u, 0xC2200000u);
        vm_wr32(v, 0x005D7084u, 0x443E0000u);
        break;
    case SC_INFOTIMES:                               /* 0x0048A166..0x0048A16C */
        vm_wr32(v, 0x006FA068u, vm_rd32(v, 0x006FA204u));
        break;
    case SC_INFOSCORES:                              /* 0x0048ABF4..0x0048ABFA */
        vm_wr32(v, 0x006FA06Cu, vm_rd32(v, 0x006FA208u));
        break;
    case SC_INFOSTAGE:                               /* 0x0048A8F6..0x0048A93C:
                                                        the single-stage map's
                                                        selectors, first entry */
        vm_wr32(v, 0x00708068u, 1);
        vm_wr32(v, 0x00708230u, 0);
        vm_wr32(v, 0x00708234u, 1);
        vm_wr32(v, 0x00708238u, 0);
        vm_wr32(v, 0x0070AB54u, 0);
        vm_wr32(v, 0x0070AB58u, 1);
        vm_wr32(v, 0x007082C4u, 0);
        vm_wr32(v, 0x007082C0u, 0);
        break;
    case SC_VISPREF:                                 /* 0x00476548           */
        vm_wr32(v, 0x006FA08Cu, vm_rd32(v, 0x006FA228u));
        break;
    case SC_CSPECS:                                  /* 0x00476978: the first page */
        vm_wr32(v, 0x00708304u, 0);
        break;
    case SC_REDEF: {                                 /* 0x004759E0..0x00475A02:
                                                        the keys into the edit
                                                        copy 0x707EFD */
        uint32_t i;
        P = vm_rd32(v, 0x00708DE8u);
        for (i = 0; i < 12; i++) vm_wr8(v, 0x00707EFDu + i, vm_rd8(v, P + 0x1Cu + i));
        vm_wr32(v, 0x007082F8u, 0);
        break;
    }
    case SC_CTRLPROG: {                              /* 0x0047470F: the axes'
                                                        rest state */
        uint32_t r[8], k;
        memset(r, 0, sizeof r);
        opt_devices(f);
        fe_vmcall(f, 0x0040929Eu, r);
        for (k = 0; k < 11; k++)                 /* the log shows changes */
            f->ctl_map[k] = vm_rd32(v, P + 0x50u + 4u * k);
        f->ctl_armed = 0;
    }   /* fall through */
    case SC_CTRL:
        opt_devices(f);
        /*/4 (the user, 2026-11-05): the pad is always live and
         * the device row is hidden, so a player whose device +0x2C is 0 (the
         * install's ral.dat: all four) gets the port's one device, the pad.
         * At 0 the pre-draw 0x004671BD greys every row, and with the picker
         * hidden nothing could turn it on.  The race ignores +0x2C either way. */
        if (vm_rd32(v, vm_rd32(v, 0x00708DE8u) + 0x2Cu) == 0)
            vm_wr32(v, vm_rd32(v, 0x00708DE8u) + 0x2Cu, 1);
        vm_run(f, 0x004759BE, "0x004759BE (the player's device)");
        break;
    }
    (void)P;
    vm_wr32(v, 0x0070AB6Eu, 0);
    vm_wr32(v, 0x00707EF5u, sc_table[sc]);      /* LEA EDI,[T] / MOV [0x707EF5] */
    f->tab[sc].va = sc_table[sc];
    f->cursor = (int)vm_rd32(v, OPT[k].cur);
    if (f->cursor < 1) f->cursor = 1;
    vm_wr32(v, 0x00707E91u, (uint32_t)f->cursor);
    if (OPT[k].entry) vm_run(f, OPT[k].entry, "a screen's entry list");
    vm_wr32(v, 0x0070800Cu, 0x6F);
    vm_wr32(v, 0x00707ECDu, 0);
    vm_wr32(v, 0x00708240u, 1);
    if (sc == SC_CSPECS) {               /* 0x004769A4..0x00476A74: the kind-11 list of
                                            cars first (its records are what the page
                                            reads), then this car's page art (0x004650C4)
                                            and its narration (0x00466869) */
        m3d_enter(f);
        rc = 0;
        vm_run(f, 0x004650C4, "CAR SPECIFICATIONS' art");
        vm_run(f, 0x00466869, "CAR SPECIFICATIONS' narration");
    } else if (sc == SC_SLIDES) {        /* 0x00476D12: the car's first picture, EAX = 1 (the
                                            select's own: the item's word at +0x54, 1); the
                                            narration goes on from CAR SPECIFICATIONS' */
        uint32_t regs[8];
        memset(regs, 0, sizeof regs);
        regs[VM_EAX] = 1;
        rc = 0;
        if (fe_vmcall(f, 0x0046512Du, regs) != 0) {
            fprintf(stderr, "frontend: SLIDESHOW's picture (0x0046512D) could not run: %s\n", orig_error(f->vm));
            f->vm_bad = 1;
        }
    } else if (sc == SC_INFOSTAGE) {     /* 0x00465011, the map at the selectors */
        f->dir = (int)vm_rd32(v, 0x00708230u);
        f->stage = (int)vm_rd32(v, 0x00708234u);
        f->rally = (int)vm_rd32(v, 0x00708238u);
        rc = load_stage_art(f, err, n);
    } else
        rc = load_art(f, rdstr(f, OPT[k].art), err, n);
    load_table_from(f, &f->tab[sc], sc_table[sc], 1);
    cursor_visible(f);                                           
    vm_wr32(v, 0x00707E91u, (uint32_t)f->cursor);
    if (sc != SC_CSPECS) m3d_enter(f);
    return rc;
}

static int opt_child(fe *f, int sc, char *err, size_t n)
{
    /* the caller's cursor into its own dword first (0x00473B4E..0x00473B54,
     * before 0x00470CCE runs the select): BACK finds it there */
    vm_wr32(f->vm, OPT[opt_index(f->sc)].cur, (uint32_t)f->cursor);
    f->opt_parent[sc] = f->sc;
    return opt_enter(f, sc, err, n);
}

/* BACK or ESC: the body's own exit (its cursor saved), then the body that
 * called it, from its top */
static int opt_leave(fe *f, char *err, size_t n)
{
    vm *v = f->vm;
    int k = opt_index(f->sc), to = f->opt_parent[f->sc];
    vm_wr32(v, OPT[k].cur, (uint32_t)f->cursor);
    vm_wr32(v, 0x00707ECDu, 0);
    vm_wr32(v, 0x007083A8u, 0);
    if (f->sc == SC_AUDIO) vm_wr32(v, 0x00707E3Cu, 0);          /* 0x00472DA8 */
    if (f->sc == SC_CSPECS && f->snd) snd_speech(f->snd, 0, NULL, 0);   /* 0x00476C74: CALL 0x0040F57A */
    if (f->sc == SC_CTRL || f->sc == SC_CTRLPROG)               /* 0x00474707 */
        vm_run(f, 0x00468114, "0x00468114 (the device released)");
    if (opt_screen(to)) return opt_enter(f, to, err, n);
    opt_hooks(f, 0);
    return enter(f, SC_MAIN, err, n);
}

/* A SELECT on one of the tree's screens */
/* OPTIONS' video replay, 0x0046D18E: the slot picker in LOAD mode (command 2 of 0x0041C224),
 * the options screen's cursor kept as opt_child keeps it, then the slot's viewer */
static int rpl_load_begin(fe *f, char *err, size_t n)
{
    vm_wr32(f->vm, OPT[opt_index(f->sc)].cur, (uint32_t)f->cursor);
    f->rpl_ret = f->sc;
    return enter(f, SC_RPLLOAD, err, n);
}

static int opt_select(fe *f, uint32_t h, char *err, size_t n)
{
    int k;
    fe_item *it = item(f, f->cursor);
    if (h == H_BACK) return opt_leave(f, err, n);
    for (k = 0; k < N_OPT; k++)
        if (OPT[k].body == h) return opt_child(f, OPT[k].sc, err, n);
    switch (h) {
    case H_RPLOPT: return rpl_load_begin(f, err, n);     
    }
    vm_wr32(f->vm, 0x007083A4u, it ? it->arg54 : 0);             /* 0x00470CE4 */
    opt_call(f, h, f->tab[f->sc].va, 0, opt_item_va(f, f->cursor), "an OPTIONS item");
    opt_after(f);
    if (fe_awaits_key(f)) f->ntyped = 0;          /* only keys pressed from now */
    if (vm_rd32(f->vm, 0x00707ECDu) == 1) return opt_leave(f, err, n);
    return 0;
}

/* An arrow's callback (0x00470DB1 & co.: EDI the item, EBX 0xFFFFFC1A) */
static void opt_arrow(fe *f, uint32_t h)
{
    if (f->sc == SC_INFOSTAGE) {
        /* STAGE INFORMATION's arrows are SELECT STAGE's (0x0047E041..): they
         * step [0x708230] / [0x708238] / [0x708234] and end on the map load
         * 0x00465011, which is the port's load_stage_art (as VIEW STAGE's) */
        char err[256];
        vm_hook(f->vm, 0x00465011, vh_nop, f);
        opt_call(f, h, opt_item_va(f, f->cursor), K_CALL, 0, "STAGE INFORMATION's arrow");
        vm_unhook(f->vm, 0x00465011);
        f->dir = (int)vm_rd32(f->vm, 0x00708230u);
        f->stage = (int)vm_rd32(f->vm, 0x00708234u);
        f->rally = (int)vm_rd32(f->vm, 0x00708238u);
        if (load_stage_art(f, err, sizeof err))
            fprintf(stderr, "frontend: %s\n", err);
        opt_after(f);
        return;
    }
    /* VISUAL OPTIONS' resolution arrows (0x0046CC32 / 0x0046CC46) step
     * [0x4F009C] through the DirectDraw mode list, whose count [0x4F00B8] is 0
     * in the port: LEFT would store -1 and the pre-draw would index
     * 0x4F00BC - 16, RIGHT toggles [0x707FB0], which ral.dat saves.  Inert
     * until the port's own resolution row replaces it. */
    if (f->sc == SC_VISOPT && (h == 0x0046CC32u || h == 0x0046CC46u) &&
        vm_rd32(f->vm, 0x004F00B8u) == 0)
        return;
    opt_call(f, h, opt_item_va(f, f->cursor), K_CALL, 0, "an OPTIONS arrow");
    opt_after(f);
}

/* The pre-draw list, each frame (0x0047134D) - after the items, before the
 * 3D draw list */
static void opt_frame(fe *f)
{
    int k = opt_index(f->sc), i;
    if (f->sc == SC_P1DET || f->sc == SC_REDEF) vm_wr32(f->vm, 0x007082F8u, 0);
    if (f->sc == SC_CTRL || f->sc == SC_CTRLPROG) opt_devices(f);
    if (f->sc == SC_CTRLPROG && vm_rd32(f->vm, 0x00707B64u) != 1)
        f->ctl_armed = 0;                         /* not waiting: re-arm on the
                                                     next row's wait */
    for (i = 0; i < 2; i++)
        if (OPT[k].pre[i]) {
            uint32_t r[8];
            memset(r, 0, sizeof r);
            r[VM_EDI] = f->tab[f->sc].va;
            vm_wr32(f->vm, 0x00707E91u, (uint32_t)f->cursor);
            vm_wr32(f->vm, 0x00707EF5u, f->tab[f->sc].va);
            if (fe_vmcall(f, OPT[k].pre[i], r) != 0 && !f->vm_bad) {
                fprintf(stderr, "frontend: %s's pre-draw (0x%08X) could not run: %s\n",
                        sc_name[f->sc], OPT[k].pre[i], orig_error(f->vm));
                f->vm_bad = 1;
            }
            gfx2d_set_clip(&f->surf, 0, 0, W - 1, H - 1);
        }
    if (f->sc == SC_CTRLPROG) {                  /* the mapping, for the log */
        uint32_t P = setup_player(f->vm);
        for (i = 0; i < 11; i++) {
            uint32_t m = vm_rd32(f->vm, P + 0x50u + 4u * (uint32_t)i);
            if (m != f->ctl_map[i])
                printf("frontend: program controller: mapping %d = 0x%X (was "
                       "0x%X)\n", i, (unsigned)m, (unsigned)f->ctl_map[i]);
            f->ctl_map[i] = m;
        }
    }
    f->dik = 0;
    opt_after(f);
}

/* A key the screen's own code waits for, through the mailbox 0x00407CDC,
 * before the key handler sees it: REDEFINE KEYBOARD's (0x0047949B, while
 * [0x708014] is 1) and program controller's (0x00467E9A, SPACE cancels,
 * while [0x707B64] is 1 - 0x00462C35 then ignores every key) */
/* ----: the census of what can reach the interpreter -------------------------------------------------
 * Every call into the original's code in this file goes through vm_run / vm_draw / post_box / fe_vmcall /
 * opt_call / finish_routine (and four vm_call sites).  `fe_has_native` is the lookup-only mirror of the
 * dispatch those make (fel_lookup, then the champion twins, then fe_native's families): 1 when a twin
 * answers the address, so the interpreter never sees it.  `fe_vm_census` prints, for each address in
 * `list` (one per line: 0xADDRESS and a label, from tools/vm_census.py: the constants at the call sites)
 * and for every address the TABLES hand those funnels (the 3D draw lists, OPTIONS' pre-draw and entry
 * lists, the class loops' helpers, the items' handlers and arrows of the screens whose items run the
 * original's code), TWIN or NONE.  A NONE is read by hand: a hook (nothing runs), a no-op, or the
 * original's code run in the interpreter. */
static int fe_has_native(fe *f, uint32_t va)
{
    static fel_draw fd = { fd_tok, fd_text, fd_svc, NULL };
    int k;
    if (!ensure_vm(f)) return 0;
    fd.ud = f;
    fel_set_draw(&fd);
    if (fel_lookup(va) || fds_lookup(va) || fdb_lookup(va) || fdc_lookup(va) || fde_lookup(va)) return 1;
    if (va == 0x0046F785u || va == 0x0046E580u || va == 0x004680B9u || fdc_is_typing(va)) return 1;
    for (k = 0; k < fdv_count(); k++) if (fdv_va(k) == va) return 1;
    for (k = 0; k < fdv_edi_count(); k++) if (fdv_edi_va(k) == va) return 1;
    for (k = 0; k < fdsg_count(); k++) if (fdsg_va(k) == va) return 1;
    for (k = 0; k < fdsg_edi_count(); k++) if (fdsg_edi_va(k) == va) return 1;
    for (k = 0; k < fdq_count(); k++) if (fdq_va(k) == va) return 1;      
    return 0;
}

static int census_row(fe *f, FILE *out, uint32_t va, const char *what)
{
    int has = fe_has_native(f, va);
    fprintf(out, "%s 0x%08X %s\n", has ? "TWIN" : "NONE", va, what);
    return has;
}

int fe_vm_census(fe *f, const char *list, FILE *out)
{
    int sc, i, k, none = 0, total = 0;
    char what[96];
    if (!ensure_vm(f)) return -1;
    if (list) {
        FILE *in = fopen(list, "r");
        char line[256];
        if (!in) { fprintf(out, "census: cannot read %s\n", list); return -1; }
        while (fgets(line, sizeof line, in)) {
            unsigned long va;
            char *e;
            if (line[0] != '0') continue;
            va = strtoul(line, &e, 16);
            while (*e == ' ') e++;
            e[strcspn(e, "\r\n")] = 0;
            total++;
            none += !census_row(f, out, (uint32_t)va, e);
        }
        fclose(in);
    }
    for (sc = 0; sc < SC_COUNT; sc++)
        if (M3D[sc].draw) { snprintf(what, sizeof what, "table: the 3D draw list of %s", sc_name[sc]); total++; none += !census_row(f, out, M3D[sc].draw, what); }
    for (k = 0; k < N_OPT; k++) {
        for (i = 0; i < 2; i++)
            if (OPT[k].pre[i]) { snprintf(what, sizeof what, "table: %s's pre-draw %d", sc_name[OPT[k].sc], i); total++; none += !census_row(f, out, OPT[k].pre[i], what); }
        if (OPT[k].entry) { snprintf(what, sizeof what, "table: %s's entry list", sc_name[OPT[k].sc]); total++; none += !census_row(f, out, OPT[k].entry, what); }
    }
    {
        static const ch_loop_t *const CL[3] = { &CL_STAGE, &CL_RALLY, &CL_CHAMP };
        for (k = 0; k < 3; k++) {
            snprintf(what, sizeof what, "table: class loop %d's positions", k); total++; none += !census_row(f, out, CL[k]->count, what);
            snprintf(what, sizeof what, "table: class loop %d's screen", k); total++; none += !census_row(f, out, CL[k]->show, what);
        }
    }
    /* the screens whose items' handlers and arrows the host runs as the original's code: OPTIONS' family (opt_call) and the slot screens */
    for (sc = 0; sc < SC_COUNT; sc++) {
        int run = opt_screen(sc) || sc == SC_SULOAD || sc == SC_SUSAVE || sc == SC_CHLOAD || sc == SC_CHSAVE ||
                  sc == SC_RPLSAVE || sc == SC_RPLLOAD;
        if (!run) continue;
        for (i = 0; i < f->tab[sc].n; i++) {
            const fe_item *it = &f->tab[sc].it[i];
            uint32_t a[5];
            a[0] = it->sel; for (k = 0; k < 4; k++) a[k + 1] = it->cb[k];
            for (k = 0; k < 5; k++) {
                if (a[k] < 0x00401000u || a[k] >= 0x004D0000u) continue;
                snprintf(what, sizeof what, "table: %s item %d %s", sc_name[sc], i + 1, k ? "arrow" : "select");
                total++; none += !census_row(f, out, a[k], what);
            }
        }
    }
    fprintf(out, "census: %d addresses, %d with no twin\n", total, none);
    return none;
}

int fe_awaits_key(const fe *f)
{
    if (!f->vm) return 0;
    if (f->sc == SC_REDEF) return vm_rd32(f->vm, 0x00708014u) == 1;
    if (f->sc == SC_CTRLPROG) return vm_rd32(f->vm, 0x00707B64u) == 1;
    return 0;
}

/* the deferred entries - every player count but one (single's and
 * time trial's SELECT NUMBER OF PLAYERS) and NETWORKED RALLY */
static int deferred(const fe *f, uint32_t h)
{
    switch (f->sc) {
    case SC_MAIN:      return h == 0x0047223Cu;                 /* 0x6FBA8C */
    case SC_PLAYERS:   return h == 0x0047E54Eu || h == 0x0047E72Fu ||
                              h == 0x0047E93Du || h == 0x0047EC07u;
    case SC_TTPLAYERS: return h == 0x00486FB2u || h == 0x0048721Fu ||
                              h == 0x0048752Du || h == 0x004878F1u;
    case SC_CHNEW:     return h == 0x0049E33Au || h == 0x0049E439u ||   
                              h == 0x0049E538u || h == 0x0049E678u ||
                              h == 0x004A592Cu || h == 0x004A5A0Du ||   /* the A8's */
                              h == 0x004A5AEEu || h == 0x004A5C10u;
    case SC_PDET:      return h == 0x00473C78u || h == 0x00473F28u ||   
                              h == 0x004741AFu;          /* PLAYER 2..4 DETAILS */
    }
    return 0;
}

static int stage_screen(const fe *f)
{
    return f->mode == 3 ? SC_TTSTAGE : SC_STAGE;
}

static int select_item(fe *f, char *err, size_t n)
{
    fe_item *it = item(f, f->cursor);
    uint32_t h = it ? it->sel : 0;
    f->saved[f->sc] = f->cursor;
    if (deferred(f, h)) {                    /* as a locked level does */
        fe_snd(f, SND_ERROR);
        f->soon_on = 1;
        f->soon_sc = f->sc;
        f->soon_cur = f->cursor;
        return 0;
    }
    if (opt_screen(f->sc)) return opt_select(f, h, err, n);   
    if (h == H_RPLSAVE && f->sc != SC_RPLSAVE) return rpl_begin(f, err, n);   /* any REVIEW OPTIONS */
    if (h == H_RPLVIEW && f->sc != SC_RPLLOAD) return rpl_view_begin(f);       
    switch (f->sc) {
    case SC_SULOAD: case SC_SUSAVE:                      
        return su_slots_select(f);
    case SC_VIEWSTAGE: case SC_TTVIEWSTAGE:              /* 0x0046E300 */
        return vs_leave(f);
    case SC_CHLOAD: case SC_CHSAVE:                      
        return ch_slots_select(f, err, n);
    case SC_RPLSAVE: case SC_RPLLOAD:                    /* / */
        return rpl_select(f, err, n);
    case SC_MAIN:
        if (h == H_SRAL) return enter(f, SC_SRAL, err, n);
        if (h == H_CHLOAD) return enter(f, SC_CHLOAD, err, n);     
        if (h == H_OPTIONS) {                                       
            f->opt_parent[SC_OPTIONS] = SC_MAIN;
            return opt_enter(f, SC_OPTIONS, err, n);
        }
        if (h == H_BACK) {                                  /* exit game */
            if (f->quit_confirm) return quit_ask(f, err, n);
            save_dat(f);                                    /* 0x00414FDA */
            return FE_QUIT;
        }
        break;
    case SC_SRAL:
        if (h == H_PLAYERS)   { f->mode = 0; set_carlist(f, A_CARLIST);
                                return enter(f, SC_PLAYERS, err, n); }
        if (h == H_TTPLAYERS) { f->mode = 3; set_carlist(f, A_CARLIST);
                                return enter(f, SC_TTPLAYERS, err, n); }
        if (h == H_ARCADE) {
            /* 0x0048398D: [0x70AB6E] = [0x7082EC] = 0, then SELECT LEVEL */
            vm_wr32(f->vm, 0x0070AB6Eu, 0);
            vm_wr32(f->vm, 0x007082ECu, 0);
            return enter(f, SC_ARCLEVEL, err, n);
        }
        if (h == H_CHNEW) {
            /* 0x004727FE: [0x7082EC] = [0x70AB6E] = 0, NEW SIMULATION */
            vm_wr32(f->vm, 0x007082ECu, 0);
            vm_wr32(f->vm, 0x0070AB6Eu, 0);
            return enter(f, SC_CHNEW, err, n);
        }
        if (h == H_A8) {
            /* 0x004A528F: locked unless [0x70CC34] == 1 - ERROR, then
             * Sleep(250) (0x00410DF4, not kept: the port's loop does not
            * block); the box is the post-draw's (0x004A6652).  Unlocked it
             * calls 0x004A557A: NEW SIMULATION over its own table,
             * [0x7082EC] = [0x70AB6E] = 0 as 0x004727FE has them */
            if (vm_rd32(f->vm, A_A8LOCK) != 1) { fe_snd(f, SND_ERROR); return 0; }
            vm_wr32(f->vm, 0x007082ECu, 0);
            vm_wr32(f->vm, 0x0070AB6Eu, 0);
            a8_open(f);
            return enter(f, SC_CHNEW, err, n);
        }
        if (h == H_BACK) return enter(f, SC_MAIN, err, n);
        break;
    case SC_PLAYERS:
        if (h == H_SINGLE_1P) {
            /* 0x0047E40F: the new career (FUN_0046F209), one player, the
             * default car list, then car select at the player's own car */
            new_career(f, 0);
            car_index_of(f);
            return enter(f, SC_CAR, err, n);
        }
        if (h == H_BACK) return enter(f, SC_SRAL, err, n);
        break;
    case SC_TTPLAYERS:
        if (h == H_TT_1P) {
            /* 0x00486E1B: [0x7082BC] = 3, [0x708038] = 3, the new career, the
             * car list, car select */
            new_career(f, 3);
            car_index_of(f);
            return enter(f, SC_CAR, err, n);
        }
        if (h == H_BACK) return enter(f, SC_SRAL, err, n);
        break;
    case SC_CAR:
        if (h == H_BACK) {
            /* the accept: FUN_004775A4, player+0x08 and +0x90 from the entry */
            uint32_t ent = drd(f, f->carlist + 4u * (uint32_t)f->car_index);
            f->p_car = (int32_t)rd(f, ent);
            f->p_90 = (int32_t)rd(f, ent + 8);
            if (f->vm) su_sync_player(f);
            if (f->mode == 2) return arc_next_stage(f, err, n);   /* 0x00483ED0 */
            if (f->mode == 1) {                                   /* 0x0049E301 */
                vm_wr32(f->vm, 0x007082F4u, 9);
                return ch_begin(f, err, n);       /* FUN_0049CB25 */
            }
            return enter(f, stage_screen(f), err, n);
        }
        break;
    case SC_CHNEW:
        if (h == H_CH_1P) return ch_new(f, err, n);
        if (h == H_A8_1P) return a8_new(f, err, n);          
        if (h == H_BACK) { a8_close(f); return enter(f, SC_SRAL, err, n); }
        break;
    case SC_STAGE:
        if (h == H_BACK) {                         /* the launch, 0x0047E4CA */
            pre_stage(f);
            if (f->vm) su_sync_player(f);
            su_read_sliders(f);
            f->launched = 1;
            return FE_LAUNCH;
        }
        break;
    case SC_TTSTAGE:
        if (h == H_BACK) {                         /* 0x00486E89 -> 0x00489D64 */
            pre_stage(f);
            return enter(f, SC_SETUP, err, n);
        }
        break;
    case SC_RESULTS:
        if (h == H_BACK)
            return f->mode == 3 && f->vm ? tt_after_results(f, err, n)   
                                         : enter(f, SC_REVIEW, err, n);
        break;
    case SC_TTBEST:
        if (h == H_BACK) return enter(f, SC_REVIEW, err, n);  /* 0x00486F54 */
        break;
    case SC_ARCLEVEL:
        if (h == H_BACK) return enter(f, SC_SRAL, err, n);           /* item 7 */
        if (h >= H_ARCLV1 && h <= 0x00483DD9u) return arc_select_level(f, h, err, n);
        break;
    case SC_ARCNEXT: case SC_ARCRES: case SC_ARCOVER: case SC_ARCBEST:
    case SC_ARCTOP: case SC_ARCCAR:
        if (h == H_BACK) return arc_leave(f, h, err, n);
        break;
    case SC_ARCCONT: case SC_ARCNEXTLV: case SC_ARCRESTART:
        if (h == H_BACK || h == H_ARCAGAIN || h == H_REVIEW_MAIN)
            return arc_leave(f, h, err, n);
        break;
    case SC_REVIEW:
        if (h == H_BACK) return enter(f, stage_screen(f), err, n);   /* "select another stage" */
        if (h == H_REVIEW_MAIN) return enter(f, SC_MAIN, err, n);
        break;
    default:
        if (ch_screen(f->sc)) return ch_leave(f, h, 0, err, n);     
    }
    unported(f, h, "this menu item");
    return 0;
}

/* ESC: [0x707ECD] = [0x7083A8] = 1 (0x00462C72) - the screen's own loop
 * decides where that goes.  The single flow's exit (0x0047E51F) sets
 * [0x7082EC] = 1, which unwinds to the main menu. */
static int back(fe *f, char *err, size_t n)
{
    f->saved[f->sc] = f->cursor;
    if (opt_screen(f->sc)) return opt_leave(f, err, n);          
    switch (f->sc) {
    case SC_SULOAD: case SC_SUSAVE: return su_slots_leave(f);   
    case SC_VIEWSTAGE: case SC_TTVIEWSTAGE: return vs_leave(f); 
    case SC_CHLOAD: case SC_CHSAVE: return ch_slots_leave(f, err, n);   
    case SC_RPLSAVE:   return rpl_finish(f, err, n);                    /* 0x00462C72 */
    case SC_RPLLOAD:   return rpl_load_done(f, -1, err, n);             /* ESC cancels */
    case SC_MAIN:      if (f->quit_confirm) return quit_ask(f, err, n);
                       save_dat(f);                 /* 0x00414FDA */
                       return FE_QUIT;
    case SC_SRAL:      return enter(f, SC_MAIN, err, n);
    case SC_PLAYERS:   return enter(f, SC_SRAL, err, n);
    case SC_TTPLAYERS: return enter(f, SC_SRAL, err, n);
    case SC_CAR:
        /* the arcade: [0x70AB6E] = 1 (0x00462D65), back to SELECT LEVEL
         * (0x00483EB7) */
        if (f->mode == 2) return enter(f, SC_ARCLEVEL, err, n);
        if (f->mode == 1) return enter(f, SC_CHNEW, err, n);   /* 0x0049E2FA */
        return enter(f, f->mode == 3 ? SC_TTPLAYERS : SC_PLAYERS, err, n);
    case SC_ARCLEVEL:  return enter(f, SC_SRAL, err, n);
    case SC_CHNEW:     a8_close(f); return enter(f, SC_SRAL, err, n);
    case SC_STAGE:     return enter(f, SC_MAIN, err, n);
    case SC_TTSTAGE:   return enter(f, SC_MAIN, err, n);
    case SC_RESULTS:
        return f->mode == 3 && f->vm ? tt_after_results(f, err, n)       
                                     : enter(f, SC_REVIEW, err, n);
    case SC_TTBEST:    return enter(f, SC_REVIEW, err, n);
    case SC_REVIEW:    return enter(f, stage_screen(f), err, n);
    default:
        if (arc_screen(f->sc)) return arc_leave(f, 0, err, n);
        if (ch_screen(f->sc)) return ch_leave(f, 0, 1, err, n);   /* RETIRE */
    }
    return 0;
}

/* ---- the public face ---------------------------------------------------- */

fe *fe_open(const char *assets, const char *image, char *err, size_t n)
{
    fe *f = (fe *)calloc(1, sizeof *f);
    char path[1024];
    int k;
    uint32_t p0;
    if (!f) { snprintf(err, n, "out of memory"); return NULL; }
    vfs_init(&f->fs, assets, NULL);
    vfs_init(&f->inst, assets, NULL);            /* the install's var\ */
    vfs_set_user(&f->inst, assets);
    snprintf(f->assets, sizeof f->assets, "%s", assets);
    if (ral_image_open(&f->im, image, err, n) != 0) { free(f); return NULL; }
    if (vfs_resolve(&f->fs, "files\\locale\\english.txt", path, sizeof path) != 0 ||
        lexicon_load(&f->lx, path) != 0) {
        snprintf(err, n, "the lexicon: %s", path);
        goto fail;
    }
    if (load_buttons(f, err, n) || load_fonts(f, err, n) || wire_fonts(f, err, n))
        goto fail;
    gfx2d_surface_init(&f->surf, f->px, W, H);
    for (k = 0; k < SC_COUNT; k++) load_table(f, &f->tab[k], sc_table[k]);
    f->textflash = (int)rd(f, A_TEXTFLASH);
    f->itemflag = (int)rd(f, A_ITEMFLAG);
    {
        const unsigned char *b = ral_image_at(&f->im, A_BLENDLUT, 4 * 256);
        if (b) memcpy(f->blend, b, sizeof f->blend);
    }
    /* player 1's record as the image holds it ([0x708DE8] -> 0x007083B8),
     * until fe_load_saves puts the user's var\ral.dat over it */
    p0 = rd(f, 0x00708DE8u);
    f->p_car = (int32_t)rd(f, p0 + 0x08);
    f->p_gear = (int32_t)rd(f, p0 + 0x30);
    set_carlist(f, A_CARLIST);
    f->dir = 0; f->rally = 0; f->stage = 1;      /* [0x708230/38/34] */
    for (k = 0; k < SC_COUNT; k++) f->saved[k] = 0;
    f->saved[SC_SETUP] = (int)rd(f, 0x006FA0F0u);   /* the screen's own saved cursor */
    {
        char files[1024];
        if (vfs_resolve_dir(&f->fs, "files", files, sizeof files) == 0) {
            char e2[256];
            f->m3d = menu3d_open(&f->im, files, e2, sizeof e2);
            if (!f->m3d) fprintf(stderr, "frontend: the 3D models are off: %s\n", e2);
        }
    }
    if (!ensure_vm(f)) { snprintf(err, n, "out of memory"); goto fail; }
    if (enter(f, SC_MAIN, err, n)) goto fail;
    return f;
fail:
    fe_close(f);
    return NULL;
}

/* the keyboard layout the typing's character table (0x00407B8A) reads,
 * as the boot's locale step loads it (0x0040313F..0x00403193): the first
 * 0x400 bytes of files\%locale\*.kim (FindFirstFile's first; the install has
 * one, BRITISH.KIM) at 0x004EB79C, magic 'KEYB', its ten table offsets at
 * +0x30 made pointers, and [0x4EB7A8] = 0x004EB7CC + 0x14 * byte +0x0B.
 * Transcribed, not run: the step is a wildcard search the file layer's hooks
 * do not cover.  Without it the typing reads through a null table. */
static void load_kim(fe *f)
{
    char dir[1100], path[1400];
    plat_dir *d;
    const char *e;
    uint8_t *buf = NULL;
    size_t len = 0, i;
    vm_wr32(f->vm, 0x004EB7A8u, 0x004F2F9Cu);           /* 0x004030D9..0x004030EA */
    vm_wr8(f->vm, 0x004EB7A5u, 1);
    vm_wr8(f->vm, 0x004EB7A6u, 1);
    if (vfs_resolve_dir(&f->fs, "files\\locale", dir, sizeof dir) != 0) return;
    d = plat_dir_open(dir);
    if (!d) return;
    while ((e = plat_dir_next(d)) != NULL) {
        size_t n = strlen(e);
        char err[128];
        if (n > 4 && e[n - 4] == '.' && (e[n - 3] | 0x20) == 'k' &&
            (e[n - 2] | 0x20) == 'i' && (e[n - 1] | 0x20) == 'm') {
            snprintf(path, sizeof path, "%s/%s", dir, e);
            if (load_host_file(path, &buf, &len, err, sizeof err) != 0) buf = NULL;
            break;
        }
    }
    plat_dir_close(d);
    if (!buf) return;
    if (len < 0x30 + 40 || memcmp(buf, "KEYB", 4) != 0) {
        fprintf(stderr, "frontend: %s is not a keyboard file - typing is off\n", path);
        free(buf);
        return;
    }
    for (i = 0; i < len && i < 0x400; i++) vm_wr8(f->vm, 0x004EB79Cu + (uint32_t)i, buf[i]);
    free(buf);
    for (i = 0; i < 10; i++) {
        uint32_t a = 0x004EB7CCu + 4u * (uint32_t)i, x = vm_rd32(f->vm, a);
        if (x) vm_wr32(f->vm, a, x + 0x004EB7CCu);
    }
    vm_wr32(f->vm, 0x004EB7A8u, 0x004EB7CCu + 0x14u * vm_rd8(f->vm, 0x004EB7A7u));
}

/* Where var\ and savedata\ are, and whether the port may write there
 * (frontend.h). */
void fe_set_user(fe *f, const char *dir, int writes)
{
    vfs_set_user(&f->fs, dir);
    f->writes = writes;
}

/* The boot's two readers and the main menu's entry, as the original runs them
 * (frontend.h).  The readers are 0x00411013's: var\ral.dat (FUN_004A6696,
 * the 69-field flat list) and var\hst.dat (FUN_004A6CB9, which first
 * allocates the tables, 0x004787D0); then the run counter, and the players'
 * and the machine's default names from the lexicon (0x0041101E..).  The
 * main menu's entry, 0x00478937 (called once, 0x00470F33), names the players
 * in their records (0x0046D203), copies the music sliders, aliases the
 * allocated blocks at 0x0070803C.. and - when no hst.dat was read,
 * [0x6F9F88] != 1 - seeds every best-times table (0x004789C8..); its two
 * graphics calls are the port's own (the loading screen 0x00478F6B, the
 * buttons 0x00478799, load_buttons). */
/* the boot's two entry points are C (fe_logic.c), reaching this
 * file's hooks through vm_invoke; nothing is interpreted.  A failure is
 * reported as vm_run reports one: the original's fatal exit (its code at
 * 0x4F0274), or a service the C found no hook for. */
static void boot_run(fe *f, uint32_t va, int (*fn)(vm *), const char *what)
{
    if (fn(f->vm) == 0) return;
    if (!f->vm_bad) {
        if (fel_boot_error())
            fprintf(stderr, "frontend: %s (0x%08X): no hook for the service 0x%08X\n", what, va,
                    (uint32_t)fel_boot_error());
        else
            fprintf(stderr, "frontend: %s (0x%08X) took the original's fatal exit 0x%08X\n", what, va,
                    vm_rd32(f->vm, 0x004F0274));
    }
    f->vm_bad = 1;
}

int fe_load_saves(fe *f, int read)
{
    int had;
    char path[1100];
    if (!ensure_vm(f)) return 0;
    f->no_read = !read;
    had = read && (vfs_resolve(&f->fs, "var\\ral.dat", path, sizeof path) == 0 ||
                   vfs_resolve(&f->inst, "var\\ral.dat", path, sizeof path) == 0);
    if (had) {
        /* a 5.25 file only: the reader tests the magic and the version and
         * nothing else, and another build's layout
         * would land on the wrong globals */
        uint8_t *buf;
        size_t len;
        char err[128];
        if (load_host_file(path, &buf, &len, err, sizeof err) == 0) {
            if (len != 46698) {
                fprintf(stderr, "frontend: %s is not a 5.25 save (size %zu) - ignored\n",
                        path, len);
                f->no_read = 1;
                had = 0;
            }
            free(buf);
        }
    }
    load_kim(f);                                 /* 0x00401065, before the saves */
    vm_hook(f->vm, 0x00478F6B, vh_nop, f);
    vm_hook(f->vm, 0x00478799, vh_nop, f);
    f->vm_bad = 0;                               /* each says why it stopped */
    boot_run(f, 0x00411013, fel_boot_411013, "FUN_00411013 (the boot's readers)");
    f->vm_bad = 0;
    boot_run(f, 0x00478937, fel_boot_478937, "FUN_00478937 (the main menu's entry)");
    f->no_read = 0;
    f->p_car = (int32_t)vm_rd32(f->vm, 0x007083B8u + 0x08);
    f->p_gear = (int32_t)vm_rd32(f->vm, 0x007083B8u + 0x30);
    f->p_90 = (int32_t)vm_rd32(f->vm, 0x007083B8u + 0x90);
    return had && !f->vm_bad;
}

void fe_unlock_a8(fe *f)
{
    if (ensure_vm(f)) vm_wr32(f->vm, A_A8LOCK, 1);
}

/* 0x0041123E: var\ral.dat, then var\hst.dat (0x004A69BB, 0x004A6D9A), the
 * original's own writers.  The original calls it when the frontend returns
 * without the quit flag (0x00414FDA) and when an arcade level is won
 * (0x00486DA9). */
static void save_dat(fe *f)
{
    if (!f->vm) return;
    su_sync_player(f);
    vm_run(f, 0x0041123E, "FUN_0041123E (the ral.dat / hst.dat writer)");
}

void fe_close(fe *f)
{
    int k;
    if (!f) return;
    for (k = 0; k < 4; k++) free(f->fnt[k]);
    free(f->mbuts);
    vm_free(f->vm);
    menu3d_close(f->m3d);
    lexicon_free(&f->lx);
    ral_image_close(&f->im);
    free(f);
}

int fe_frame(fe *f, unsigned keys, uint32_t now_ms)
{
    char err[256];
    int r = FE_RUN;
    f->now = now_ms;
    if (f->snd) f->snd->now = now_ms;
    if (fe_typing(f)) {
        r = su_typing(f, keys);                  /* every frame */
    } else if (fe_awaits_key(f)) {
        /* the screen's own pre-draw takes the key from the mailbox
         * (opt_frame, below) - a raw key queued by fe_type, else the menu
         * keys as their folded codes (a script) */
        static const struct { unsigned bit; int dik; } kk[6] = {
            { FE_ESC, 0x01 }, { FE_ENTER, 0x1C }, { FE_UP, 0x66 },
            { FE_DOWN, 0x6B }, { FE_LEFT, 0x68 }, { FE_RIGHT, 0x69 } };
        int i;
        f->dik = 0;
        if (f->ntyped) {
            f->dik = f->typed[0];
            memmove(f->typed, f->typed + 1, sizeof f->typed - sizeof f->typed[0]);
            memmove(f->typed_shift, f->typed_shift + 1,
                    sizeof f->typed_shift - sizeof f->typed_shift[0]);
            f->ntyped--;
        } else {
            for (i = 0; i < 6; i++) if (keys & kk[i].bit) { f->dik = kk[i].dik; break; }
        }
        if (keys) f->key_ms = now_ms;
    } else if (keys) {
        if (keys & (FE_UP | FE_DOWN | FE_LEFT | FE_RIGHT | FE_ENTER))
            f->key_ms = now_ms;                  /* 0x00462C9F / 0x00462CD3 */
        if (f->sc == SC_SETUP)    r = setup_keys(f, keys);
        else if (f->sc == SC_CHSPLASH && !(keys & FE_ESC)) {
            /* 0x0047B468: any key but ESC stops the narration and
             * goes on; ESC is RETIRE (0x0047B471) */
            r = ch_leave(f, H_BACK, 0, err, sizeof err);
        }
        else if (keys & FE_ESC)   r = back(f, err, sizeof err);   /* silent */
        else if (keys & FE_ENTER) {
            fe_snd(f, SND_SELECT);               /* 0x00462CC3, before the handler */
            r = select_item(f, err, sizeof err);
        }
        else if (keys & FE_UP)    move(f, 0);
        else if (keys & FE_DOWN)  move(f, 1);
        else if (keys & FE_LEFT)  move(f, 2);
        else if (keys & FE_RIGHT) move(f, 3);
        if (r < 0) {
            fprintf(stderr, "frontend: %s\n", err);
            r = FE_QUIT;
        }
    } else if (f->sc == SC_CHSPLASH && !f->a8 && f->now - f->splash_ms >= 3000u &&
               !(f->snd && f->snd->n_on)) {
        /* 0x0047B445..0x0047B456: 3,000 ms, then until the narration
         * has ended */
        r = ch_leave(f, H_BACK, 0, err, sizeof err);
        if (r < 0) { fprintf(stderr, "frontend: %s\n", err); r = FE_QUIT; }
    }
    m3d_frame(f);                                /* 0x004664C9 (pre-draw) */
    /* 0x0047133B's order: the canvas, labels, items, the pre-draw list, the
     * 3D draw list (fe_draw_3d, after the canvas is on screen -) and
     * the post-draw list */
    memcpy(f->px, f->bg, sizeof f->px);
    if (f->sc != SC_CHSPLASH) {                  /* a picture, no screen */
        draw_labels(f);
        draw_items(f);
    }
    draw_caption(f);
    if (f->sc == SC_SETUP) {
        if (!keys && vm_rd32(f->vm, 0x0070CA08) == 1) setup_edit_frame(f->vm, 0);
        setup_draw(f);
    }
    if (f->sc == SC_RESULTS) results_draw(f);
    if (opt_screen(f->sc) && f->vm) {            /* the pre-draw list */
        f->itemflag = (int)vm_rd32(f->vm, A_ITEMFLAG);
        opt_frame(f);
    }
    if (f->sc == SC_CAR || f->sc == SC_ARCCAR) pre_car(f);
    /* the arcade screens' handlers (sc_table's lists) */
    switch (f->sc) {
    case SC_ARCLEVEL: vm_draw(f, 0x00486DB0, "SELECT LEVEL's lock"); break;
    case SC_ARCNEXT:
        /* block D's pointer is set at the main menu's entry (read or seeded,
         * were the allocation to fail it would be 0 and the
         * best-time row 0x00470AFA would read through it: skipped then, as
         * SELECT STAGE's is (0x0047E022) */
        if (!vm_rd32(f->vm, 0x00708048u)) vm_hook(f->vm, 0x00470AFA, vh_nop, f);
        vm_draw(f, 0x00485AB9, "NEXT STAGE's draw");
        break;
    case SC_ARCRES:   vm_draw(f, 0x004854A1, "the arcade RESULTS' draw"); break;
    case SC_ARCOVER:  vm_draw(f, 0x00484F23, "the lost level's draw"); break;
    case SC_ARCBEST:  vm_draw(f, 0x00486939, "BEST TIMES' draw"); break;
    case SC_TTBEST:   vm_draw(f, 0x00488E1A, "BEST TIMES' draw (time trial)"); break;
    case SC_ARCTOP:   vm_draw(f, 0x00486B6C, "TOP SCORES' draw"); break;
    /* the championship's pre-draw lists (frontend.py tables) */
    case SC_CHLEG:    vm_draw(f, 0x0046F2EB, "LEG DETAILS' draw"); break;
    case SC_CHNEXT:
        /* the best-time row 0x0047091E reads hst.dat's block B ([0x708040],
         * direction 0): without the file it is skipped, as the arcade's
         * 0x00470AFA is */
        if (!vm_rd32(f->vm, 0x00708040u)) vm_hook(f->vm, 0x0047091E, vh_nop, f);
        vm_draw(f, 0x00470443, "NEXT STAGE's draw");
        vm_run(f, 0x0046C40F, "0x0046C40F (the next narration file)");   /* 0x004702E3 */
        break;
    case SC_CHCLASS:  vm_draw(f, 0x0049DD98, "CLASS STAGE RESULTS' draw");
                      vm_draw(f, 0x0049DC42, "the page's range"); break;
    case SC_CHOVER:   vm_draw(f, 0x0049EBCA, "OVERALL STAGE RESULTS' draw");
                      vm_draw(f, 0x0049DC42, "the page's range"); break;
    case SC_CHRCLASS: vm_draw(f, 0x0049EFF7, "the rally's class standings' draw");
                      vm_draw(f, 0x0049DC42, "the page's range"); break;
    case SC_CHRALLY:  vm_draw(f, 0x0049F460, "the rally's standings' draw");
                      vm_draw(f, 0x0049DC42, "the page's range"); break;
    case SC_CHCCLASS: vm_draw(f, 0x004A0A93, "CLASS CHAMPIONSHIP's draw"); break;
    case SC_CHCHAMP:  vm_draw(f, 0x004A1082, "OVERALL CHAMPIONSHIP's draw"); break;
    case SC_CHBEST:   vm_draw(f, 0x0049FE29, "BEST TIMES' draw"); break;
    case SC_CHBESTCH: vm_draw(f, 0x004A4998, "BEST CHAMPIONSHIP SCORES' draw"); break;
    case SC_CHBESTOV: vm_draw(f, 0x004A4B78, "BEST OVERALL SCORES' draw"); break;
    /* [0x707E70] = 0x70B3DE, the panel; [0x707E68] = 0x70B5A2, the
     * notice ([0x707E64]'s list 0x70B102 is empty) */
    case SC_VIEWSTAGE:                                   
        if (!vm_rd32(f->vm, 0x00708040u)) vm_hook(f->vm, 0x0047091E, vh_nop, f);
        vm_draw(f, 0x0047C127, "VIEW STAGE's draw");
        vm_run(f, 0x0046C40F, "0x0046C40F (the next narration file)");   /* 0x0047BF9B */
        break;
    /* [0x707E64] = 0x70B022, one draw, 0x0048300D; the other two
     * lists are empty and the loop never calls 0x0046C40F.  The best-time
     * row (0x0047091E) is guarded by the block pointers in its own code. */
    case SC_TTVIEWSTAGE:
        vm_draw(f, 0x0048300D, "VIEW STAGE's draw (time trial)");
        break;
    /* the post-draw list 0x70B51E (the page's draw over the car's record),
     * then 0x0046C40F, the next narration file (Mobil 1 only), 0x00476B35 */
    case SC_CSPECS:
        vm_draw(f, 0x00478F77, "CAR SPECIFICATIONS' page");
        vm_run(f, 0x0046C40F, "0x0046C40F (the next narration file)");
        break;
    /* SLIDESHOW's lists are empty; the loop runs 0x0046C40F (0x00476DD3) */
    case SC_SLIDES:
        vm_run(f, 0x0046C40F, "0x0046C40F (the next narration file)");
        break;
    case SC_CHINSPECT: vm_draw(f, 0x0049B994, "the INSPECTION AREA's panel");
                       vm_draw(f, 0x00479F65, "the INSPECTION AREA's notice"); break;
    case SC_CHLOAD: case SC_CHSAVE:                  /* 0x0070AECA / E2 */
                      vm_draw(f, 0x00469EBF, "the slots' numbers");
                      vm_draw(f, 0x0046A0B7, "the slots' names");
                      vm_draw(f, 0x0046A4CC, "the slot's details"); break;
    case SC_RPLSAVE: case SC_RPLLOAD:                 /* /: [0x707E64] */
                      vm_draw(f, 0x0046C09C, "the replay slots' names"); break;
    case SC_RETIRE:   vm_draw(f, 0x0046C6B1, "RETIRE's no / yes");
                      vm_draw(f, 0x0046C6CC, "RETIRE's question"); break;
    /* the slot screens' draw lists (0x0070B23E / 0x0070B236) and
     * post-draws (0x0070B482 / 0x0070B47A); no 3D between them */
    case SC_SULOAD:   vm_draw(f, 0x0049BB10, "the slot screen's panels");
                      vm_draw(f, 0x0046BD33, "LOAD SETTINGS' slots"); break;
    case SC_SUSAVE:   vm_draw(f, 0x0049BB10, "the slot screen's panels");
                      vm_draw(f, 0x0046BCDD, "SAVE SETTINGS' slots"); break;
    }
    /* SELECT LEVEL's post-draw 0x00486DB0 draws its box while the
     * cursor is on a locked level; this one appears when a deferred entry is
     * CHOSEN (the original's own screenshots hover NETWORKED RALLY with no
     * box) and stays until the cursor leaves it */
    if (f->soon_on && (f->sc != f->soon_sc || f->cursor != f->soon_cur))
        f->soon_on = 0;
    f->post_on = 0;
    if (f->soon_on && f->vm) {
        f->soon = 1;
        post_box(f, 0x00415A0C, "the COMING SOON box");
        f->soon = 0;
    }
    /* START RALLY's post-draw - NOT YET AVAILABLE! while the cursor
     * is on A8's slot (item 3), the slot is locked and cheat bit 0x11 is
     * clear; the original's code decides */
    if (f->sc == SC_SRAL && f->vm) post_box(f, 0x004A6652, "START RALLY's post-draw");
    if (f->sc == SC_STAGE || f->sc == SC_TTSTAGE) pre_stage(f);
    return r == FE_LAUNCH ? FE_LAUNCH : r == FE_QUIT ? FE_QUIT : r == FE_VIEW ? FE_VIEW : FE_RUN;
}

const gfx2d_surface *fe_canvas(const fe *f)
{
    return &f->surf;
}

const gfx2d_surface *fe_post(fe *f)
{
    if (!f->post_on) return NULL;
    gfx2d_surface_init(&f->post_surf, f->post, W, H);
    return &f->post_surf;
}

/* wear.h's record access, through the interpreter's memory */
static uint32_t wear_vm_rd(void *ud, uint32_t a) { return vm_rd32((vm *)ud, a); }
static void wear_vm_wr(void *ud, uint32_t a, uint32_t v) { vm_wr32((vm *)ud, a, v); }

void fe_launch_get(const fe *f, fe_launch *o)
{
    memset(o, 0, sizeof *o);
    o->gmode = f->mode;                          /* EBX at 0x0047E4C5: 0 single, 3 time trial;
                                                    2 at 0x00483F32, the arcade */
    o->track = f->track;
    o->direction = f->crsdirec;
    o->carnum = f->p_car;
    o->gearbox = f->p_gear;
    o->rally = f->rally;
    o->stage = f->stage;
    if (f->mode == 2 && f->vm) {                 /* [0x70CF64] / [0x70CF60]  */
        o->rally = (int32_t)vm_rd32(f->vm, A_ARCLEVEL);
        o->stage = (int32_t)vm_rd32(f->vm, A_ARCSTAGE);
    }
    /* what graham_to_shaun hands the arcade - [0x70CF6C], the last
     * race's arcadepo, as the start position (0x00445A91); and player 2's
     * gearbox [0x70CF04] (0x00445407) */
    o->arcadepo = f->vm ? (int32_t)vm_rd32(f->vm, 0x0070CF6Cu)
                        : (int32_t)rd(f, 0x0070CF6Cu);
    o->gearbox2 = f->vm ? (int32_t)vm_rd32(f->vm, 0x0070CF04u)
                        : (int32_t)rd(f, 0x0070CF04u);
    if (f->mode == 1 && f->vm) {                 /* the rally, and the */
        o->rally = (int32_t)vm_rd32(f->vm, A_CHRALLY);   /* entry's stage number */
        o->stage = (int32_t)vm_rd32(f->vm, ch_entry(f, 0) + 0x40);
    }
    snprintf(o->level, sizeof o->level, "LEVEL%02d", (int)f->track);
    if (f->vm) {                                 /* [0x70CC38]        */
        uint32_t g = vm_rd32(f->vm, 0x0070CC38u);
        int k;
        o->have_ghosts = g != 0;
        for (k = 0; g && k < 90; k++) {
            uint32_t r = g + 0x50u * (uint32_t)k;
            o->ghosts[k].name = (int32_t)vm_rd32(f->vm, r);
            o->ghosts[k].type = (int32_t)vm_rd32(f->vm, r + 8u);
            o->ghosts[k].total = (int32_t)vm_rd32(f->vm, r + 0xCu);
        }
    }
    if (f->vm) {                                 /* 0x00480E07..0x00480E3E */
        uint32_t m = vm_rd32(f->vm, 0x007082F4u);
        o->retire_all = !(m == 5 || m == 4 || m == 0xF || m == 0xA
                          || vm_rd32(f->vm, 0x007082F0u) == 1);
    }
    o->has_setup = f->su_valid;
    o->tyre = f->su_sl.tyre; o->bbal = f->su_sl.bbal; o->seth = f->su_sl.seth;
    o->sets = f->su_sl.sets; o->stes = f->su_sl.stes; o->setg = f->su_sl.setg;
    {
        /* the current player's record, [0x708DE8 + [0x7082F8] * 4], as the
         * launcher reads it (the interpreter's data: var\ral.dat included) */
        uint32_t P = f->vm ? setup_player(f->vm) : rd(f, 0x00708DE8u);
        int k;
        for (k = 0; k < 12; k++) {
            const unsigned char *b = ral_image_at(&f->im, P + 0x1Cu + (uint32_t)k, 1);
            o->keys[k] = f->vm ? vm_rd8(f->vm, P + 0x1Cu + (uint32_t)k)
                               : (b ? *b : 0);
        }
        for (k = 0; k < 11; k++)
            o->joymap[k] = f->vm ? vm_rd32(f->vm, P + 0x50u + 4u * (uint32_t)k)
                                 : rd(f, P + 0x50u + 4u * (uint32_t)k);
        o->carparts = f->vm ? (int32_t)vm_rd32(f->vm, P + 0x278u)
                            : (int32_t)rd(f, P + 0x278u);
        if (f->vm) {                         /* 0x00480A49..0x00480C5D */
            wear_from_record(&o->wear, wear_vm_rd, f->vm, P);
            o->have_wear = 1;
        }
        o->racenum = f->vm ? (int32_t)vm_rd32(f->vm, P + 0x28u)
                           : (int32_t)rd(f, P + 0x28u);
        o->champ88 = f->vm ? (int32_t)vm_rd32(f->vm, P + 0x288u)
                           : (int32_t)rd(f, P + 0x288u);
        if (f->mode == 1 && f->vm) o->champ88 = f->ch_champ88;  /* see vh_launch */
        o->cheats = f->vm ? vm_rd32(f->vm, A_CHEATS) : rd(f, A_CHEATS);
        o->codriver = f->vm ? (int32_t)vm_rd32(f->vm, P + 0x154u)
                            : (int32_t)rd(f, P + 0x154u);
        /* the camera, the device, the axis mode, the shifter and the
         * sensitivity, as the launcher reads them (0x00480D0F..0x00480D2F) */
        o->camera   = f->vm ? (int32_t)vm_rd32(f->vm, P + 0x0Cu)  : (int32_t)rd(f, P + 0x0Cu);
        o->device   = f->vm ? (int32_t)vm_rd32(f->vm, P + 0x2Cu)  : (int32_t)rd(f, P + 0x2Cu);
        o->axismode = f->vm ? (int32_t)vm_rd32(f->vm, P + 0x34u)  : (int32_t)rd(f, P + 0x34u);
        o->shifter  = f->vm ? (int32_t)vm_rd32(f->vm, P + 0x3Cu)  : (int32_t)rd(f, P + 0x3Cu);
        o->sens     = f->vm ? (int32_t)vm_rd32(f->vm, P + 0x280u) : (int32_t)rd(f, P + 0x280u);
        o->lowerdz = f->vm ? (int32_t)vm_rd32(f->vm, P + 0x44u) : (int32_t)rd(f, P + 0x44u);
        /* the device is configured by 0x004680E6, its one caller of
         * 0x00409250, with EDX = +0x44 and EBP = 0 - so the scheme's upper
         * dz (+0x14) is 0 and SETUP CONTROLLER's upper dz +0x40, edited
         * and saved, never reaches the device.  Kept. */
        o->upperdz = 0;
        o->race_music_on = f->vm ? (int32_t)vm_rd32(f->vm, 0x0070CC68u)
                                 : (int32_t)rd(f, 0x0070CC68u);
        /* the launcher's read, 0x004804E5 (its copy to 0x70CC6C is
         * not run here, and the arrow's own copy only follows a press) */
        o->lens_flare = f->vm ? (int32_t)vm_rd32(f->vm, 0x00707FB8u)
                              : (int32_t)rd(f, 0x00707FB8u);
        /* 0x004804EF..0x00480513, the four rows after it */
        o->reflections = f->vm ? (int32_t)vm_rd32(f->vm, 0x00707FC4u)
                               : (int32_t)rd(f, 0x00707FC4u);
        o->car_shadows = f->vm ? (int32_t)vm_rd32(f->vm, 0x00707FC8u)
                               : (int32_t)rd(f, 0x00707FC8u);
        o->car_detail  = f->vm ? (int32_t)vm_rd32(f->vm, 0x00707FCCu)
                               : (int32_t)rd(f, 0x00707FCCu);
        o->headlights  = f->vm ? (int32_t)vm_rd32(f->vm, 0x00707FD0u)
                               : (int32_t)rd(f, 0x00707FD0u);
        {
            /* 0x00480B02..0x00480B6B: the flags into 0x70CD64.., +0x284 into
             * [0x70CF78]; 0x00445479..: each flag != 0 sets its bit */
            static const struct { uint32_t off, bit; } hb[9] = {
                { 0x158, 0x10 }, { 0x15C, 0x08 }, { 0x160, 0x01 }, { 0x168, 0x04 },
                { 0x170, 0x02 }, { 0x178, 0x100 }, { 0x16C, 0x40 }, { 0x164, 0x20 },
                { 0x174, 0x80 } };
            uint32_t over = f->vm ? vm_rd32(f->vm, P + 0x284u) : rd(f, P + 0x284u);
            o->hud_mask = 0;
            for (k = 0; k < 9; k++)
                if ((f->vm ? vm_rd32(f->vm, P + hb[k].off) : rd(f, P + hb[k].off)) != 0)
                    o->hud_mask |= hb[k].bit;
            if (over != 0xFFFFFFFFu) o->hud_mask = over;
        }
    }
}

void fe_after_race(fe *f, uint32_t now_ms, const fe_result *r)
{
    char err[256];
    f->launched = 0;
    f->key_ms = now_ms;
    if (f->vm && r) vm_wr32(f->vm, 0x00574548u, r->replay ? 1u : 0u);   
    if (f->vm && r) {
        /* the launcher's copy of the race back into the player record,
         * 0x00481518..0x0048153E: the stage time (hundredths of a second) and
         * the top speed x 10; a retired car gets the no-time sentinel 1 h and
         * state 6 (0x00481502) */
        uint32_t P = setup_player(f->vm);
        su_sync_player(f);
        if (r->have_wear) {
            /* 0x0048120F..0x004814D7, before the finished test - so
             * on every race: the HUD mask, the dents, the parts left, the
             * wear and its five summaries (wear.h) */
            vm_wr32(f->vm, P + 0x284, r->hud_mask);
            vm_wr32(f->vm, P + 0x278, (uint32_t)r->carparts);
            wear_into_record(&r->wear, wear_vm_wr, f->vm, P);
        }
        if (r->finished) {
            vm_wr32(f->vm, P + 0x88, (uint32_t)r->time_cs);
            vm_wr32(f->vm, P + 0x94, (uint32_t)r->speed_x10);
        } else {
            vm_wr32(f->vm, P + 0x88, 0x36EE80u);
            vm_wr32(f->vm, P + 0xD0, 6u);
        }
    }
    if (f->snd) {
        /* the launcher's return: the frontend sound mode (0x00480E75), then
         * menu music 3 (0x0048156E); the playlist position back on its
         * global, for the ral.dat writer */
        f->snd->now = now_ms;
        snd_mode_frontend(f->snd);
        snd_cue_after_race(f->snd);
        if (f->vm) vm_wr32(f->vm, 0x00511E88u, (uint32_t)f->snd->set.pos);
    }
    if (f->mode == 1 && f->vm) {
        /* the launcher's gof [0x70CF94] (2 finished, -1 out of the
         * race), then FUN_0046F785's tail (0x0046F8AD): out of the race is
         * out of the rally, [0x708294] = 0 - its 0x004A526C ran with the
         * hooked launcher.  The championship's race rules - the 90 rivals as
         * ghosts, the false start - are the: the
         * race was a single stage, and the rivals' times are the menus' own
         * (0x0046D5B5, 0x0046F785), the original's code. */
        vm *v = f->vm;
        int rc;
        vm_wr32(v, 0x0070CF94u, r && r->finished ? 2u : 0xFFFFFFFFu);
        if (!(r && r->finished)) vm_wr32(v, 0x00708294u, 0);
        rc = ch_go(f, f->ch_pc, err, sizeof err);
        if (rc < 0) fprintf(stderr, "frontend: %s\n", err);
        return;
    }
    if (f->mode == 2 && f->vm) {
        /* the arcade.  The race's end hands slot 0's catileft and arcadepo
         * to [0x70CF68] / [0x70CF6C] (FUN_00447A1B, 0x00447A4C..0x00447A5B:
         * catileft since, the streamer's position since), and
         * gof to [0x70CF94]: 2 finished, -1 out of the race (section 5).  Then 0x00483F3C..0x00483F6A: out of the race, the
         * position 0 and the level lost; else [0x70AAC0] = [0x70CF68] and
         * [0x70AAD0] = [0x70CF6C].  ~~The STAND-IN, first place for a
         * finish, with [0x70AAC0] = 0~~ - gone with. */
        vm *v = f->vm;
        int rc;
        vm_wr32(v, 0x0070CF94u, r && r->finished ? 2u : 0xFFFFFFFFu);
        if (r) {
            vm_wr32(v, 0x0070CF68u,
                    r->time_left_cs >= 0 ? (uint32_t)r->time_left_cs : 0u);
            if (r->arcadepo >= 0) vm_wr32(v, 0x0070CF6Cu, (uint32_t)r->arcadepo);
        }
        if (!(r && r->finished)) {
            vm_wr32(v, A_ARCPOS, 0);
            vm_wr32(v, A_ARCFAIL, 1);
        } else {
            vm_wr32(v, 0x0070AAC0u, vm_rd32(v, 0x0070CF68u));
            vm_wr32(v, A_ARCPOS, vm_rd32(v, 0x0070CF6Cu));
        }
        vm_wr32(v, 0x007082F8u, 0);
        vm_run(f, 0x00486374, "FUN_00486374 (the level's score)");
        rc = vm_rd32(v, A_ARCFAIL) == 1 ? arc_end(f, err, sizeof err)
                                        : enter(f, SC_ARCRES, err, sizeof err);
        if (rc < 0) fprintf(stderr, "frontend: %s\n", err);
        return;
    }
    if (f->mode == 3 && f->vm && !(r && r->finished)) {
        /* FUN_00486E1B, 0x00486ED8 - out of the race ([0x70CF94] -1)
         * goes past RESULTS and the record straight to REVIEW OPTIONS */
        vm_wr32(f->vm, 0x0070CF94u, 0xFFFFFFFFu);
        if (enter(f, SC_REVIEW, err, sizeof err)) fprintf(stderr, "frontend: %s\n", err);
        return;
    }
    if (enter(f, SC_RESULTS, err, sizeof err)) fprintf(stderr, "frontend: %s\n", err);
}

/* The settings the requests read, off the interpreter's data (the
 * image's, with var\ral.dat over them), the volume table 0x00501B0C and the
 * frontend set's file names (the sound table 0x0050571C, +0x18 by id); then
 * what the boot does before the first frame: mode_dispatch's frontend sound
 * mode (0x0040EE16), and the main menu's entry - 0x00478937 copies the two
 * music sliders over the music volumes, 0x00470F88 cues track 4.  The main
 * menu's loop re-enters below that cue (0x00470F8D), so 4 is cued once. */
void fe_set_sound(fe *f, snd *s)
{
    static const uint32_t ids[9] = { 0xB8, 0x137, 0x16B, 0x19F, 0x1A0, 0x19E,
                                     0x19C, 0x19D, 0x19B };   /* sound.c's bit order */
    int32_t mb[65];
    int k;
    f->snd = s;
    if (!s) return;
    for (k = 0; k < 65; k++) mb[k] = (int32_t)rd(f, 0x00501B0Cu + 4u * (uint32_t)k);
    if (mb[64] == 0 && mb[0] == -10000) snd_set_table(s, mb);
    else fprintf(stderr, "frontend: the volume table 0x00501B0C is not this "
                         "build's - the formula stands in\n");
    for (k = 0; k < 9; k++)
        snprintf(s->fe_path[k], sizeof s->fe_path[k], "%s",
                 rdstr(f, rd(f, 0x0050571Cu + 0x40u * ids[k] + 0x18u)));
    if (f->vm) {
        s->set.overall       = (int32_t)vm_rd32(f->vm, 0x004F3298u);
        s->set.pos           = (int32_t)vm_rd32(f->vm, 0x00511E88u);
        s->set.music_menu    = (int32_t)vm_rd32(f->vm, 0x00707FF0u);   /* 0x0047894C */
        s->set.music_race    = (int32_t)vm_rd32(f->vm, 0x00707FF4u);   /* 0x00478956 */
        s->set.music_race_set = s->set.music_race;                     
        s->set.fx_menu       = (int32_t)vm_rd32(f->vm, 0x00707FF8u);
        s->set.fx_race       = (int32_t)vm_rd32(f->vm, 0x00707FFCu);
        s->set.speech_menu   = (int32_t)vm_rd32(f->vm, 0x00708000u);
        s->set.race_music_on = (int32_t)vm_rd32(f->vm, 0x0070CC68u);
        /* the co-driver's slider (the voice is the launch's) */
        s->set.codriver      = (int32_t)vm_rd32(f->vm, 0x00708004u);
        /* the car engine's */
        s->set.engine        = (int32_t)vm_rd32(f->vm, 0x00708008u);
    }
    s->now = f->now;
    snd_mode_frontend(s);
    snd_cue_main_menu(s);
}

const char *fe_screen_name(const fe *f)
{
    return sc_name[f->sc];
}

uint64_t fe_vm_steps(const fe *f)
{
#ifdef RALLY_INTERP
    return f->vm ? vm_steps(f->vm) : 0;
#else
    (void)f;
    return 0;                                   /* nothing is interpreted: no interpreter */
#endif
}

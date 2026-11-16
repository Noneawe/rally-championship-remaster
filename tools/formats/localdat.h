/* localdat.h - Rally Championship `LOCALDAT`, as a buffer-taking parser.
 *
 * The level's TEXTURE SET, and
 * needs it beside cdat.c: a stage drawn without it is a stage drawn with the
 * wrong plates and with still water.
 *
 * It is NOT a container and NOT track data. It is a fixed-size 8,300-byte
 * image of a live global block at 0x005bb970, which the game reads with one
 * ReadFile and writes back with one WriteFile. Its own name for itself is
 * `prefs`:
 *
 *     LOADLEVEL_INIT : load prefs        -> load_prefs_q         0x00443833
 *     LOADLEVEL_INIT : initgametexinfbuf -> initgametexinfbuf_q  0x00437b1d
 *     LOADLEVEL_INIT : init ed textures  -> init_ed_textures_q   0x00422cee
 *
 * Every constant below is an operand in one of the game's own readers - not
 * one was inferred from the file's contents. See
 *
 *
 * This is a transcription of tools/localdat.py and is held to BYTE-IDENTICAL
 * output by check.sh.
*/
#ifndef RAL_LOCALDAT_H
#define RAL_LOCALDAT_H

#include <stddef.h>
#include <stdint.h>

/* `mov ecx,0x206c` at 0x00443840 and 0x004576a4 - the size, and it is the
 * whole story. Note it is NOT a multiple of 8. */
#define LD_SIZE 0x206c
#define LD_BASE 0x005bb970u         /* the live block the file is an image of */
#define LD_NDW  (LD_SIZE / 4)       /* 2,075 */

#define LD_TEXINFO_OFF 0x0100
#define LD_TEXINFO_N   256

/* TEXTINFO's record count is stated nowhere in the code. It is the file's size
 * divided by the stride the reader uses, and both halves are measured:
 *   0x00438077  mov ecx,0x1f4000   reading "files\a\textinfo"  (2,048,000 b)
 *   0x00437b5b  imul eax,eax,0x80                              (0x80 stride) */
#define LD_TEXTINFO_BYTES   0x1f4000
#define LD_TEXTINFO_STRIDE  0x80
#define LD_TEXTINFO_RECORDS (LD_TEXTINFO_BYTES / LD_TEXTINFO_STRIDE)   /* 16,000 */

/* The per-slot animation pair, both indexed by the same 0..255 slot number the
 * geometry record carries, which is also TEXINFO's index space. */
#define LD_TEXANIM_OFF     0x06d0
#define LD_TEXANIM_PHASES  8
#define LD_TEXRATE_OFF     0x0f98
#define LD_TEXSLOTS        256

/* The per-slot DIFFUSE TINT, indexed by the same 0..255 slot number, and the
 * live address is 0x005bd53c = LD_BASE + 0x1bcc.  It ends exactly where the
 * preset region begins, which is how the 0x400 length is known - the only
 * reader states the base and the stride and nothing states a count:
 *   0044d9c2  MOVZX EAX,byte [ESI+0x01]        ; the polygon's primary tex id
 *   0044d9c6  MOV   EAX,[EAX*4 + 0x5bd53c]
 *   0044d9cd  CMP   EAX,0 / JE  -> skip the modulate entirely
 * A non-zero entry is multiplied into the global colour [0x005d6f60] as
 * (a*b)>>8 per byte for the duration of ONE polygon, then popped back.
 */
#define LD_SLOT_TINT_OFF   0x1bcc
#define LD_SLOT_TINT_N     256

/* The per-level colour grade, reloaded before EVERY plate that is loaded:
 *   004241fc  [0x005bbfbc] -> [0x004efa00]    LOCALDAT +0x64c   SATURATION
 *   00424206  [0x005bbfc0] -> [0x004ef9fc]    LOCALDAT +0x650   BRIGHTNESS
 * 0x100 is neutral and the pair is 8.8 fixed point.  Note the crossover: the
 * LOWER offset lands in the HIGHER destination.
 */
#define LD_GRADE_OFF     0x064c
#define LD_GRADE_NEUTRAL 0x100

/* ---- the fog block, ---------------------------------- *
 *
 * FOGCOLOR.  0x0044DBA2 packs three consecutive dwords into (R<<16)|(G<<8)|B,
 * stores it at [0x005D6E94], ORs 0xFF000000 and hands it to the image's ONLY
 * D3DRS_FOGCOLOR write site (0x0041A187, state 34):
 *
 *   0044dbac  EAX=[0x005bbf88] / SHL 0x10      LOCALDAT +0x618   R
 *   0044dbb6  EAX=[0x005bbf8c] / SHL 0x08      LOCALDAT +0x61c   G
 *   0044dbc0  EAX=[0x005bbf90]                 LOCALDAT +0x620   B
 *
 * There is no sky geometry in the LEVEL PAYLOAD; this is the colour the
 * horizon is.  (Narrowed 2026-09-20: the sky is a 37-polygon dome
 * loaded as object type 1, out of files/a/OBJnnnn.RO3 rather than out of any
 * level file..)
 */
#define LD_FOGCOLOR_OFF 0x0618

/* The shade ramp's two ends, three dwords (r, g, b) each: FUN_0044D0EF's race
 * arm (`[0x005D41B0]` set) points FUN_0044D1AD at `[0x005BBF70]` = +0x600,
 * which reads lo at +0x00..+0x08 and hi at +0x0C..+0x14 (0x0044D1BC..0x0044D1E8).
 */
#define LD_SHADE_LO_OFF 0x0600
#define LD_SHADE_HI_OFF 0x060c

/* The two baked-light ramps' colours, black -> colour (FUN_0044D200 at
 * 0x0044D168 / 0x0044D179): ramp1 [0x005E83DC] and ramp2 [0x005E87DC], which
 * a flag-4 stage polygon's bytes +0x28.. / +0x2c.. index. */
#define LD_LIGHT1_OFF   0x1898
#define LD_LIGHT2_OFF   0x18a4
/* ramp 3, [0x005E8BDC] (0x0044D18A): the cars' headlights on the ground,
 * indexed by 0x0044D65A's falloff */
#define LD_LIGHT3_OFF   0x18b0

/* The fog ramp's own two inputs that are NOT preset-selected.  Both are read
 * only through [0x005F4498] * [0x005F449C], the sun-glare term
 * (0x0044DC2F / 0x0044DC4B), and [0x005F449C] is zeroed at 0x0045350B unless
 * +0x65c + +0x6ac is non-zero (0x00453515).  Measured over the 48 shipped
 * levels: +0x65c is 0 on 47 of 48 (LEVEL55 has 1) and +0x6ac is 0 on 35 of
 * 48 (the census: the gate +0x65c + +0x6ac is open on 13). */
#define LD_FOGSUN_OFF  0x065c           /* -> [0x005E94B4], the ramp bias   */
#define LD_FOGSUN2_OFF 0x06ac           /* -> [0x005E94B8], read elsewhere  */

#define LD_MANIFEST_BASE 0x18c8
#define LD_MANIFEST_SLOT 0x10
#define LD_MANIFEST_END  0x1ae8

/* A file no longer than this does not contain the presets region at all - the
 * April 1999 rolling demo's six LOCALDAT stop exactly here. */
#define LD_PRESET_REGION_OFF 0x1fcc
#define LD_PRESET_INDEX_OFF  0x1fd0
#define LD_PRESET_N          5

/* The build tool's COUNTING bound on a manifest id: `cmp eax,0x3e8` / `jnc` at
 * 0x004576bb and 0x004578ef, guarding a 1,000-slot presence array. It is NOT a
 * validity check - the branch skips the id, it does not reject the file - and
 * the corpus proves the difference. */
#define LD_MANIFEST_COUNTED 0x3e8
#define LD_MANIFEST_BAND_LO 0x190
#define LD_MANIFEST_BAND_HI 0x320

typedef struct {
    unsigned off;
    unsigned len;
    const char *key;
    const char *note;
} ld_region;

typedef struct {
    int slot;
    unsigned kind;
} ld_manifest_slot;

extern const ld_region        ld_regions[];
extern const size_t           ld_nregions;
extern const ld_manifest_slot ld_manifest[];    /* live slots, in reader order */
extern const size_t           ld_nmanifest;
extern const int              ld_manifest_dead[];
extern const size_t           ld_nmanifest_dead;

/* The selector FUN_00437aa7, one row per `mov eax,[ebx*4+TABLE]` / `mov [DEST]`
 * pair, in the order the function emits them.
 *
 * WHAT THE SIX SLOTS ARE FOR, and it was not known when this table was
 * written -,.  FOUR of the six exist to drive the FOG RAMP:
 * FUN_0044DBFC, the ramp builder, CALLS FUN_00437aa7 as its first instruction
 * (0x0044DBFC) and then reads +0x654, +0x658, +0x66c and +0x6a4 straight out
 * of the destinations this selector just filled.  The other two are the
 * visible-cell radius R ([0x005BD538]) and the depth divisor D
 * ([0x005BD93C]). proved this mapping over the corpus
 * by an invariant and never found a reader; this is the reader.
 *
 * THE SELECTOR'S INDEX IS LOCALDAT +0x1fd0 AND IT HAS NO WRITER.
 * `MOV EBX,[0x005BD940]` at 0x00437AA9 is the ONE absolute reference to
 * 0x005BD940 image-wide (`python tools/ralimg.py refs 0x5bd940`), 0x005BD940
 * is LD_BASE + 0x1fd0, and the index is clamped to 0..4 at 0x00437AB0 /
 * 0x00437ABA.  All 48 shipped levels carry 0 there, and only LEVEL76
 * populates a second slot in any of the six tables. */
typedef struct { unsigned src, dst; } ld_preset_pair;
extern const ld_preset_pair ld_preset_map[6];
extern const unsigned       ld_preset_tables[6];

typedef struct {
    const uint8_t *blob;    /* the file as it is on disk, however long that is */
    size_t         len;
    uint32_t       word[LD_NDW];
} ld_file;

/* The game passes 0x206c as a MAXIMUM: 0x00420846 records the byte count
 * actually read at [0x005be004] and never compares it, so a short file simply
 * leaves the tail of the block holding whatever was there. This reader does
 * what the game does - zero-fills - and reports the on-disk length so a caller
 * can tell padding from data. The build tool is the one that insists
 * (`cmp [0x5be004],0x206c` at 0x004578bf). */
void ld_parse(const void *buf, size_t len, ld_file *f);

uint32_t ld_dw(const ld_file *f, unsigned off);

/* Byte reads out of the raw blob, zero past EOF - the texanim and texrate
 * tables are byte tables, not dword ones. */
unsigned ld_byte(const ld_file *f, unsigned off);

void ld_slot_ids(const ld_file *f, int slot, uint32_t out[4]);

#endif /* RAL_LOCALDAT_H */

/* localdat.c - see localdat.h.
 *
 * The region notes are reproduced VERBATIM from tools/localdat.py's REGIONS
 * table, because `localdat fields` is part of the compared surface and
 * check.sh is a plain diff. Where the prose looks fussy, the fussiness is the
 * point.
 */

#include "localdat.h"

#include <string.h>

const ld_region ld_regions[] = {
    { 0x0000, 0x0100, "objset_q",
      "64 dwords. NO reader in retail reads this by absolute address -- only the "
      "load/save `lea` sites touch it. Values lie in the manifest's 0..999 id "
      "space, so it reads like a second object set, but that is a lead, not a "
      "finding." },
    { 0x0100, 0x0400, "texinfo",
      "256 dwords, each a record index into `files\\a\\TEXTINFO`. "
      "`initgametexinfbuf_q` 0x00437b1d loops ecx=0..0xff, and for each entry "
      "stores value&0xf at +0x24, value>>4 at +0x20, and copies the 0x80-byte "
      "TEXTINFO record at value*0x80 into a 0x40-byte slot at 0x00922330+ecx*0x40." },
    { 0x0500, 0x0100, "table581bc4",
      "64 dwords copied wholesale to 0x00581bc4 by the loop at 0x00424827, a "
      "callee of `reloadobjects` (`LOAD OBJECTS - START`). "
      "(ecx=1..0x3f, so entry 0 is never copied and is 0 in every shipped file). "
      "Entries 56..63 are overwritten with constants at 0x004247d2 before the "
      "copy, so the file's values there are dead." },
    { 0x0600, 0x00d0, "params",
      "The scalar parameter block: three RGB triples, then ~40 scalars. "
      "0x0044dba2 packs +0x618/+0x61c/+0x620 as (r<<16)|(g<<8)|b into an ARGB "
      "colour, and +0x600 is expanded into a 256-entry ramp by 0x0044d1ad -- "
      "either one fixes the triples as colours. +0x6cc is zeroed by the game at "
      "0x00437c48 before it is read, so the file's value there is dead." },
    { 0x06d0, 0x0800, "texanim",
      "256 rows of 8 BYTES -- for each texture slot, the 8-phase cycle of "
      "substitute slots. 0x004505e0 `movzx ebx,byte [eax + ebx*8 + 0x5bc040]`, "
      "eax = phase & 7, ebx = the slot byte out of the geometry record." },
    { 0x0ed0, 0x00c8, "unmapped_a",
      "200 bytes between the two animation tables. Unmapped." },
    { 0x0f98, 0x0100, "texrate",
      "256 BYTES, one per texture slot: the animation rate, 0 meaning static. "
      "0x004505a4 `movzx eax,byte [ebx + 0x5bc908]`; when it is 0 the slot is "
      "used as-is and the texanim row is never read." },
    { 0x1098, 0x0800, "sparse",
      "All-zero in every shipped file from +0x1094 on. Unmapped." },
    { 0x1898, 0x0030, "fade_ramps",
      "FOUR RGB TRIPLES. 0x0044d168/179/18a/19b pass each to FUN_0044d200, "
      "which writes dst[i] = 0xff000000 | (src*i/255) for i in 0..255 -- a "
      "256-entry ARGB ramp from black to the colour, 0x400 bytes, at "
      "0x005e83dc/87dc/8bdc/8fdc. +0x600 is a fifth, via the sibling 0x0044d1ad "
      "to 0x005e7fdc. All 600 values in the corpus lie in 0..255." },
    { 0x18c8, 0x0220, "manifest",
      "48 slots of 4 dwords; 27 of them live. 0x00422cee "
      "(`init ed textures`, and it prints `NUM DIFFERENT CARS/OBJECTS/TEXTURE "
      "SETS`) walks the live slots and tags each entry with a kind constant; "
      "`setplatesall` 0x00423e78 copies the same slots to 0x0058c6b0 at the "
      "identity offset." },
    { 0x1ae8, 0x00e4, "tail_sparse",
      "Sparse; +0x1bc8 is the only hot field (11 readers)." },
    { 0x1bcc, 0x0400, "slot_tint",
      "256 dwords, one per texture slot: a packed 0x00RRGGBB tint. "
      "FUN_0044d9bc 0x0044d9c6 `mov eax,[eax*4 + 0x5bd53c]` with eax = the "
      "polygon's primary texture id at pols+0x01; a NON-ZERO entry is "
      "multiplied into the global colour [0x005d6f60] channel-by-channel as "
      "(a*b)>>8 for the duration of that polygon, and a ZERO entry skips the "
      "modulate entirely. It is the only absolute reference to 0x005bd53c in "
      "the image." },
    { 0x1fcc, 0x0080, "presets",
      "The five-preset selector. 0x00437aa7 clamps the index at +0x1fd0 to "
      "[0,4] and uses it to pick one entry from each of six 5-dword tables at "
      "+0x1fd4 +0x1fe8 +0x1ffc +0x2010 +0x2024 +0x2038, writing them into the "
      "live params block. Reached from the sky-ramp builder 0x0044dbfc." },
    { 0x204c, 0x0020, "tail",
      "8 dwords; +0x204c/+0x2050/+0x2054 read by 0x00448123 and 0x004484e0, "
      "+0x2058 by 0x0045f07c, +0x205c/+0x2064/+0x2068 by 0x0043a1ba." }
};
const size_t ld_nregions = sizeof ld_regions / sizeof ld_regions[0];

/* Extracted mechanically by matching
 *     LEA ESI,[0x5bdXXX]  ...  MOV dword ptr [EDX*0x4 + 0x582828],K
 * over FUN_00422cee's rows in disasm.tsv;.
 *
 * 0x00423e78 copies slot 32 as 7 dwords + 1 dword, i.e. slots 32 and 33 are
 * one contiguous 8-dword run; every other live slot is 4 dwords. Slot 33 is
 * last here because localdat.py appends MANIFEST_EXTRA to MANIFEST_SLOTS and
 * the printed order is that concatenation. */
const ld_manifest_slot ld_manifest[] = {
    { 0, 0x0d }, { 1, 0x0c }, { 2, 0x0b }, { 3, 0x0a }, { 4, 0x09 },
    { 5, 0x08 }, { 6, 0x07 }, { 7, 0x06 }, { 8, 0x05 }, { 9, 0x04 },
    { 10, 0x03 }, { 11, 0x02 }, { 12, 0x01 },
    { 16, 0x20 }, { 17, 0x1f }, { 18, 0x1e }, { 19, 0x1d }, { 20, 0x1c },
    { 21, 0x1b }, { 22, 0x1a }, { 23, 0x19 }, { 24, 0x18 }, { 25, 0x17 },
    { 26, 0x16 }, { 27, 0x15 },
    { 32, 0x3d },
    { 33, 0x3d }
};
const size_t ld_nmanifest = sizeof ld_manifest / sizeof ld_manifest[0];

/* set(range(0, 34)) minus the live slots, sorted - the slots every reader
 * steps over, which the corpus check asserts are zero. */
const int ld_manifest_dead[] = {
    13, 14, 15, 28, 29, 30, 31
};
const size_t ld_nmanifest_dead =
    sizeof ld_manifest_dead / sizeof ld_manifest_dead[0];

/*   0x00437ac1/ac8   +0x1fd4 -> +0x1bc8   (then clamped to >= 1)
 *   0x00437ae0/ae7   +0x1fe8 -> +0x0658
 *   0x00437aec/af3   +0x1ffc -> +0x06a4
 *   0x00437af8/aff   +0x2010 -> +0x0654
 *   0x00437b04/b0b   +0x2024 -> +0x066c
 *   0x00437b10/b17   +0x2038 -> +0x1fcc
 * Since the index at +0x1fd0 is 0 in every shipped file, entry 0 of each table
 * must equal the destination field as saved. It does, in 300 of 300. */
const ld_preset_pair ld_preset_map[6] = {
    { 0x1fd4, 0x1bc8 }, { 0x1fe8, 0x0658 }, { 0x1ffc, 0x06a4 },
    { 0x2010, 0x0654 }, { 0x2024, 0x066c }, { 0x2038, 0x1fcc }
};
const unsigned ld_preset_tables[6] = {
    0x1fd4, 0x1fe8, 0x1ffc, 0x2010, 0x2024, 0x2038
};

void ld_parse(const void *buf, size_t len, ld_file *f)
{
    const uint8_t *b = (const uint8_t *)buf;
    size_t whole = len / 4;
    size_t i;

    memset(f, 0, sizeof(*f));
    f->blob = b;
    f->len = len;
    if (whole > LD_NDW) {
        whole = LD_NDW;
    }
    for (i = 0; i < whole; i++) {
        f->word[i] = (uint32_t)b[i * 4] | ((uint32_t)b[i * 4 + 1] << 8) |
                     ((uint32_t)b[i * 4 + 2] << 16) | ((uint32_t)b[i * 4 + 3] << 24);
    }
}

uint32_t ld_dw(const ld_file *f, unsigned off)
{
    return f->word[off / 4];
}

unsigned ld_byte(const ld_file *f, unsigned off)
{
    return off < f->len ? f->blob[off] : 0u;
}

void ld_slot_ids(const ld_file *f, int slot, uint32_t out[4])
{
    unsigned off = (unsigned)(LD_MANIFEST_BASE + slot * LD_MANIFEST_SLOT);
    int i;

    for (i = 0; i < 4; i++) {
        out[i] = ld_dw(f, off + 4u * (unsigned)i);
    }
}

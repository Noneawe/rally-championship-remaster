/* rplrec.h -: the replay RECORDER, the original's write path.
 *
 * A replay is a deterministic re-simulation seeded by a snapshot every 100
 * steps.  This is the writer half, in plain C
 * with no game state: the caller hands it the bytes the original's packer
 * reads (rplrec_snapshot), the four device words of each step (rplrec_frame)
 * and says when a chunk is complete (rplrec_commit).  What it does with them
 * is the original's, address for address:
 *
 *   rplrec_begin    fmt_replay_start_record  0x0041D928  the 0x430 header
 *   rplrec_snapshot replay_pack_chunk        0x0045DC0D  8,656 bytes
 *   rplrec_frame    replay_frame_write_q     0x0045DCFD  16 bytes a step
 *   rplrec_commit   fmt_replay_flush_marker_q 0x0041DB45 + _flush_chunk
 *   rplrec_end      fmt_replay_stop_record   0x0041DA74  the 0x3850 trailer
 *   rplrec_pack     fmt_rle_pack             0x0041E1F7  PackBits, run bias 1
 *
 * The frame being recorded and its chunk come from the car's own step counter
 * (alivet), exactly as 0x0045DB0B derives them: chunk = alivet / 100, frame
 * = alivet % 100, a snapshot at frame 0 and a commit at frame 99.  A partial
 * last chunk is NOT written (0x0041DA7E: [0x577DEC] is 0 between commits, so
 * the padding loop never runs) - which is why both real files are an exact
 * multiple of 100 frames.
*/

#ifndef RALLY_RPLREC_H
#define RALLY_RPLREC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define RPL_FILE_HDR     0x430u      /* 'REPL' header + the replayheader    */
#define RPL_HDR_SIZE     0x400u      /* replayheader                        */
#define RPL_TRAILER      0x3850u     /* 'RCIB' + 1800 index entries         */
#define RPL_MAX_CHUNKS   1800u       /* 0x708 (0x0041DBFC stops the file)   */
#define RPL_CHUNK        0x2810u     /* one chunk, uncompressed             */
#define RPL_CAR_BYTES    1668u       /* CAR_REPLAY_PREFIX                   */
#define RPL_PARTS_BYTES  960u        /* 8 detached parts of 120 (0x6CE5A4)  */
#define RPL_TAB_BYTES    1024u       /* the unnamed table (0x6CEA60)        */
#define RPL_FRAMES       100u        /* frames per chunk                    */
#define RPL_VERSION      0x00050400u

/* replayheader's named dwords (the offsets) */
#define RPLH_TRACK       0x00        /* [0x5D671C]                          */
#define RPLH_GMODE       0x04        /* [0x5D6750]                          */
#define RPLH_NUMPL       0x08        /* [0x5F0A54]                          */
#define RPLH_NUMCARS     0x0C        /* [0x5F0A50]                          */
#define RPLH_CAR1        0x10        /* car[0..3]+0x24 (shauncarnum1)       */
#define RPLH_MODE20      0x20        /* [0x50F2EC]: 1 = one local player    */
#define RPLH_MODE24      0x24        /* [0x70CD8C]                          */
#define RPLH_MODE28      0x28        /* [0x70CEF8]                          */
#define RPLH_ARCSTART    0x2C        /* [0x5B951C]                          */
#define RPLH_NUMARC      0x30        /* [0x5B9514]                          */
#define RPLH_ARCRANDOM   0x34        /* [0x5B94FC]                          */
#define RPLH_GAMEOVER1   0x38        /* car[0]+0xB0  (0x6C21C0)             */
#define RPLH_GAMEOVER2   0x3C        /* car[1]+0xB0  (0x6C31A0)             */

typedef struct rplrec {
    FILE    *f;
    int      open;                   /* [0x57454C]: recording               */
    int      done;                   /* [0x574548]: a recording exists      */
    int      full;                   /* the 1,800-chunk file ended it       */
    uint32_t nchunks;                /* [0x574590]                          */
    int32_t  frames;                 /* [0x574594]                          */
    uint32_t pos;                    /* [0x577DE8]: the next chunk's offset */
    int32_t  mode20;                 /* replayheader+0x20, read by commit   */
    int32_t  idx_off[RPL_MAX_CHUNKS];
    int32_t  idx_len[RPL_MAX_CHUNKS];
    uint8_t  chunk[RPL_CHUNK];       /* 0x9097A4: the chunk being built     */
} rplrec;

/* fmt_rle_pack: PackBits as the original writes it - a run of 2..128 is
 * (count-1, byte), a literal of 1..128 is (-count, bytes), a zero ends the
 * stream.  `out` needs n + n/128 + 8 bytes; returns the length INCLUDING the
 * terminator (the original's ECX). */
size_t rplrec_pack(const uint8_t *src, size_t n, uint8_t *out);

/* fmt_rle_unpack (0x0041E28B), the inverse; returns bytes produced */
size_t rplrec_unpack(const uint8_t *src, size_t srclen, uint8_t *out, size_t outcap);

/* The three device dwords the record packs into one (0x00409227) and back
 * (0x00409237).  Out-of-range words do not survive the round trip, in the
 * original either: 9 bits unsigned, 9 unsigned, 10 signed. */
uint32_t rplrec_word_pack(uint32_t ax, int32_t cx, uint32_t dx);
void     rplrec_word_unpack(uint32_t w, uint32_t *ax, int32_t *cx, uint32_t *dx);

/* The packed date and time of 0x00410D81: (((((y<<4|mo)<<5|d)<<5|h)<<6)|mi) */
uint32_t rplrec_stamp(int year, int month, int day, int hour, int minute);

/* fmt_replay_start_record's header and file, after the caller has filled the
 * 0x400-byte replayheader (named dwords above, the rest zero).  Returns 0, or
 * -1 if the file cannot be created.  The tag is the file's +0x10, "tmpreplay"
 * for the working file. */
int  rplrec_begin(rplrec *r, const char *path, const uint8_t hdr[RPL_HDR_SIZE],
                  uint32_t stamp);

/* replay_pack_chunk: the six blocks at the chunk's head, in the original's
 * order.  car[k] is the 1,668-byte record of slot k (NULL = zeroes); parts
 * and tab likewise.  Call at frame 0 of a chunk, BEFORE the step. */
void rplrec_snapshot(rplrec *r, const uint8_t *const car[4],
                     const uint8_t *parts, const uint8_t *tab);

/* replay_frame_write_q: player `p` (0 or 1) of frame `fr` (0..99): the raw
 * button word, and the three analogue words packed.  Out-of-range players
 * are ignored, as the original's guard does. */
void rplrec_frame(rplrec *r, unsigned fr, unsigned p, uint32_t bx, uint32_t ax,
                  int32_t cx, uint32_t dx);

/* fmt_replay_flush_marker_q + fmt_replay_flush_chunk: the chunk is complete.
 * Returns 0, or -1 when the 1,800th chunk filled the file (the recording
 * ends, as the original's does) or a write failed. */
int  rplrec_commit(rplrec *r);

/* fmt_replay_stop_record: the trailer, closed.  Safe on a closed recorder. */
int  rplrec_end(rplrec *r);

/* ---- the READER: fmt_replay_load / fmt_replay_read_chunk ----------
 *
 * The whole file in memory, its trailer's index parsed, a chunk at a time
 * unpacked (fmt_replay_read_block: a negative length is RLE).  What playback
 * does with it is the original's: at the first step of every chunk the
 * snapshot is put back (replay_unpack_chunk) and the steps after it take
 * their device words from the chunk's records (replay_frame_read_q). */
typedef struct rplrd {
    uint8_t *data;
    size_t   size;
    uint32_t nchunks;                /* the trailer's, clamped to 1,800     */
    int32_t  frames;                 /* the trailer's total                 */
    int32_t  idx_off[RPL_MAX_CHUNKS];
    int32_t  idx_len[RPL_MAX_CHUNKS];
    int      cur;                    /* the chunk in `raw`, -1 none         */
    uint8_t  raw[RPL_CHUNK];
} rplrd;

/* Reads and checks `path` as 0x0041DC14 does (magic, version, trailer, a chunk
 * count above 0); 0, or -1 with a reason in `why` (may be NULL). */
int      rplrd_open(rplrd *r, const char *path, char *why, size_t whyn);
void     rplrd_close(rplrd *r);
/* chunk k unpacked into r->raw (cached); 0, or -1 for a bad chunk */
int      rplrd_chunk(rplrd *r, uint32_t k);
/* replayheader's dword at `off` (the RPLH_ offsets) */
int32_t  rplrd_hdr(const rplrd *r, unsigned off);
/* the file's +0x10 name, NUL-ended, for the viewer's title */
void     rplrd_tag(const rplrd *r, char out[33]);
/* one step's two dwords for player p (0 or 1) of frame fr in r->raw */
void     rplrd_frame(const rplrd *r, unsigned fr, unsigned p, uint32_t *bx,
                     uint32_t *ax, int32_t *cx, uint32_t *dx);

/* The port's addition to the header, in the 960 bytes the original copies
 * verbatim and never reads: the course direction ([0x005D6720]), which the
 * original's header does not keep (it is bit 7 of a track byte it has already
 * split) - 0 in every file the game wrote, and so 0 here. */
#define RPLH_PORT_DIR    0x40

/* fmt_replay_save_to_slot_q, 0x0041DF27 (with 0x0041DC14's checks): the working
 * file `src` copied to `dst` - the header re-stamped and given the slot's
 * 32-byte name (`tag`, the file's +0x10), every chunk read, unpacked and
 * written again by the packer, the index and the trailer rebuilt.  A file the
 * packer wrote comes back identical but for the stamp and the tag.  Returns 0;
 * -1 if `src` is not a replay (magic, version, trailer) or `dst` cannot be
 * written - and then `dst` is not touched. */
int  rplrec_resave(const char *src, const char *dst, const uint8_t tag[32],
                   uint32_t stamp);

#endif

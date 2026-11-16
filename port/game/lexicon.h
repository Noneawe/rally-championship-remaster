/* lexicon.h - the game's text, by token id.
 *
 * A TOKEN ID IS THE 0-BASED POSITION OF ITS `tok...` LINE in the install's
 * files\locale\ENGLISH.TXT (
 * nine ids read out of code agree).  The original does not read that file
 * this way: its lexicon compiler (the `lc_` cluster, 0x004032B5..0x00403E94,
 * disposition DELETE in) builds a packed table that
 * str_emit_value_q (0x00401F8F) walks.  This reader is THE PORT'S, a
 * REPLACEMENT for that compiler, and what it does with the text is the file's
 * own documented syntax (its header, lines 6-25), not a transcription:
 *
 *   <label> <whitespace> <message>      ; to the end of the line is a comment
 *   \n \t \s \\ \{ \} \;                newline, tab, space, the literal
 *   \1 .. \8                            "move back X pixels": byte X, which
 *                                       draw_text reads as a back-step (<0x10)
 *   \#HH                                the byte 0xHH (\#0E\#xx is delta x)
 *   {...}   {}                          a group (braces dropped); the null phrase
 *
 * Bytes >= 0x80 are passed through as the file has them (Latin-1, e.g. 0xE9
 * for e-acute), because the frontend's font sheets hold glyphs for every byte
 * 0x20..0xFF at index byte - 0x20.
 *
 * TENTATIVE, and named: a literal TAB inside a message (tokPeugeot206 has
 * one) becomes a space; ~NNN (double-byte characters) is kept verbatim.
 * owns the lexicon; the compiled form's exact output is unread.
*/
#ifndef RALLY_GAME_LEXICON_H
#define RALLY_GAME_LEXICON_H

typedef struct {
    char  **text;          /* text[id], never NULL for id < n                */
    char  **name;          /* the tok... label                               */
    int     n;
    char   *blob;
} lexicon;

/* 0 on success; -1 the file is missing or unreadable. */
int  lexicon_load(lexicon *lx, const char *path);
void lexicon_free(lexicon *lx);

/* The text of token `id`, or "" past the end. */
const char *lexicon_text(const lexicon *lx, int id);

#endif

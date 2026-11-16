/* fd_engine.c - see fd_engine.h. family E, 2026-11-09.
 *
 * Every function names the original it transcribes.  The state is the VM's
 * data copy at the original's own addresses; arithmetic is 32-bit and wraps
 * as the original's does. */
#include "fd_engine.h"

#include "fe_champ.h"

#define R(a)    vm_rd32(v, (uint32_t)(a))
#define W(a, x) vm_wr32(v, (uint32_t)(a), (uint32_t)(x))

enum { A_PLAYERS = 0x00708DE8, A_CUR = 0x007082F8 };

static int call(const fde_services *s, uint32_t va, uint32_t r[8])
{
    return s && s->svc ? s->svc(s->ud, va, r) : 0;
}

/* a service that takes no register: the registers are passed as zeros */
static void call0(const fde_services *s, uint32_t va)
{
    uint32_t z[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    call(s, va, z);
}

static uint32_t cur_player(vm *v) { return R(A_PLAYERS + 4u * R(A_CUR)); }

/* 0x0048C18C: EAX = ((EAX >> 3) + 0x4000000) * [0x70C9F4] / 0x14.  The MUL is
 * 64-bit; the DIV would fault (#DE) if the quotient did not fit 32 bits, which
 * 0x004664C9's clamp ([0x70C9F4] <= 0x64) makes unreachable: the interpreter
 * truncates it and so does this. */
static uint32_t f48c18c(vm *v, uint32_t eax)
{
    uint64_t p = (uint64_t)((eax >> 3) + 0x4000000u) * (uint64_t)R(0x0070C9F4);
    return (uint32_t)(p / 0x14u);
}

/* 0x0048C114: the carousel's turn.  [0x70C5CC] the direction (0 none, negative
 * back, positive on), [0x70C9EC] the angle, stepped by the frame's share
 * (0x0048C18C); a step past either end stops the turn, and going on past the
 * end moves [0x70C9E8] along the [0x70C9E4] pictures. */
static void f48c114(vm *v)
{
    uint32_t eax, m;
    if (R(0x0070C5CC) == 0) return;
    if ((int32_t)R(0x0070C5CC) < 0) {
        eax = f48c18c(v, R(0x0070C9EC));
        m = R(0x0070C9EC);
        W(0x0070C9EC, m - eax);
        if (m >= eax) return;                        /* JAE: no borrow */
        W(0x0070C9EC, 0);
        W(0x0070C5CC, 0);
        return;
    }
    eax = f48c18c(v, 0u - R(0x0070C9EC));
    m = R(0x0070C9EC);
    W(0x0070C9EC, m + eax);
    if ((uint64_t)m + eax <= 0xFFFFFFFFu) return;    /* JAE: no carry */
    W(0x0070C9EC, 0);
    W(0x0070C5CC, 0);
    W(0x0070C9E8, R(0x0070C9E8) + 1u);
    if (R(0x0070C9E8) == R(0x0070C9E4)) W(0x0070C9E8, 0);
}

/* 0x004664C9 (PUSHAD ... POPAD): the carousel's frame step: the clock
 * [0x5C6568] less the last one's, kept in 0..0x64 (unsigned), then 0x0048C114 */
static int f4664c9(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    uint32_t eax = R(0x005C6568), ebx = R(0x0070C9F0);
    (void)r; (void)s; *cf = -1;
    W(0x0070C9F0, eax);
    eax -= ebx;
    if (eax >= 0x64u) eax = 0x64;
    W(0x0070C9F4, eax);
    f48c114(v);
    return FDE_OK;
}

/* 0x004680E6 (PUSHAD ... POPAD): the player's device configured through the
 * service 0x00409250 - EAX +0x2C - 1, EBX +0x34, ECX +0x38, EDX +0x44, EBP 0 */
static void f4680e6(vm *v, const fde_services *s)
{
    uint32_t P = cur_player(v), q[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    q[VM_EAX] = R(P + 0x2C) - 1u;
    q[VM_EBX] = R(P + 0x34);
    q[VM_ECX] = R(P + 0x38);
    q[VM_EDX] = R(P + 0x44);
    q[VM_EBP] = 0;
    call(s, 0x00409250u, q);
}

/* 0x004759BE (PUSHAD ... POPAD): the player's device, configured when its
 * number (+0x2C) is not above [0x4F28A0] */
static int f4759be(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    (void)r; *cf = -1;
    if (R(cur_player(v) + 0x2C) <= R(0x004F28A0)) f4680e6(v, s);
    return FDE_OK;
}

/* 0x00468114: the device released - the service 0x00409285, called with the
 * registers as they are (no PUSHAD: whatever the service leaves stays) */
static int f468114(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    (void)v; *cf = -1;
    call(s, 0x00409285u, r);
    return FDE_OK;
}

/* 0x0040929E: the pad's axes against their rest state (EAX 0: copy the rest
 * state - six words from 0x4F19F0 to 0x4F3230 - and CF set; else the first
 * axis the device [0x4F1EB8] marks (bit n of its +0x14) that moved by more than
 * 0x80 either way: EAX = n, or n | 0x10 for the upper side, CF clear; no axis:
 * CF set and EAX is the last difference).  PUSH EBX/ECX/ESI/EDI kept. */
static int f40929e(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    uint32_t eax = r[VM_EAX], ebx, n, d = 0;
    (void)s;
    if (eax == 0) {
        for (n = 0; n < 6; n++) W(0x004F3230u + 4u * n, R(0x004F19F0u + 4u * n));
        *cf = 1;
        return FDE_OK;
    }
    ebx = R(0x004F1EB8u) * 0x50u + 0x004F28A4u;
    for (n = 0; n < 6; n++) {
        d = R(0x004F19F0u + 4u * n) - R(0x004F3230u + 4u * n);
        if ((R(ebx + 0x14) >> n) & 1u) {
            if ((int32_t)d < -0x80) { r[VM_EAX] = n; *cf = 0; return FDE_OK; }
            if ((int32_t)d > 0x80) { r[VM_EAX] = n | 0x10u; *cf = 0; return FDE_OK; }
        }
    }
    r[VM_EAX] = d;
    *cf = 1;
    return FDE_OK;
}

/* 0x0046E362 with EBX = 1, as 0x0046DBE8 / 0x0046DC0C call it: the first item
 * record of the screen's table [0x707EF5] (the table's first word, +4, is the
 * record) */
static uint32_t first_item(vm *v) { return R(0x00707EF5) + 4u; }

/* 0x0046DBE8 / 0x0046DC0C (PUSHAD ... POPAD): the first item's two words (+0x1C
 * +0x20) and font (+0x50) as a screen's controller rows set them */
static int f46dbe8(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    uint32_t e = first_item(v);
    (void)r; (void)s; *cf = -1;
    W(e + 0x1C, 0xFFFFFFA9u);
    W(e + 0x20, 0xFFFFFFA9u);
    W(e + 0x50, 0x005184CCu);
    return FDE_OK;
}
static int f46dc0c(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    uint32_t e = first_item(v);
    (void)r; (void)s; *cf = -1;
    W(e + 0x1C, 0xFFFFFFA7u);
    W(e + 0x20, 0x006FA320u);
    W(e + 0x50, 0x005183ACu);
    return FDE_OK;
}

/* 0x0047C0F5 (PUSHAD ... POPAD): the stages to go in the rally - from the
 * itinerary entry [0x7082C4] rallies on and [0x7082C0] entries ahead
 * (0x0046EBBC), one past it, the run of entries whose kind word is 0 */
static int f47c0f5(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    uint32_t edi, ecx = 0, n = 0;
    int rc;
    (void)r; (void)s; *cf = -1;
    edi = fec_46ebbc(v, 0x006E7EE9u, R(0x007082C0), R(0x007082C4), &rc);
    if (rc != FEC_OK) return FDE_RUNAWAY;
    for (;;) {
        edi += 0xA4u;
        if (R(edi) != 0) break;
        ecx++;
        if (++n > 100000u) return FDE_RUNAWAY;
    }
    W(0x007082D0, ecx);
    return FDE_OK;
}

/* 0x00484207 and 0x00489D20 (PUSHAD ... POPAD): the record EDI (the stage's
 * record in the table) takes the current player's best time (+0x94) when it is
 * below the one there (a signed compare), with the player's +4, +8 and the
 * 0x20 bytes of the name that +0x18 points to.  0x00484207's fields are +0x30
 * (the time), +0x74, +0x78, +0x54; 0x00489D20's +0x2C, +0x9C, +0xA0, +0x7C. */
static void record_best(vm *v, uint32_t rec, uint32_t kt, uint32_t k4, uint32_t k8, uint32_t kn)
{
    uint32_t P = cur_player(v), best = R(P + 0x94), src, k;
    if ((int32_t)R(rec + kt) >= (int32_t)best) return;
    W(rec + kt, best);
    W(rec + k4, R(P + 4));
    W(rec + k8, R(P + 8));
    src = R(P + 0x18);
    for (k = 0; k < 0x20; k++) vm_wr8(v, rec + kn + k, vm_rd8(v, src + k));
}
static int f484207(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    (void)s; *cf = -1;
    record_best(v, r[VM_EDI], 0x30, 0x74, 0x78, 0x54);
    return FDE_OK;
}
static int f489d20(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    (void)s; *cf = -1;
    record_best(v, r[VM_EDI], 0x2C, 0x9C, 0xA0, 0x7C);
    return FDE_OK;
}

/* 0x004A4FDE: the first player (0..3) whose +0xB4 + 4 * [0x7082C4] word is 0x14
 * - the one placed in the rally - or -1.  EAX = [0x7082C4], EDI the record
 * last looked at. */
static uint32_t f4a4fde(vm *v, uint32_t *edi)
{
    uint32_t eax = R(0x007082C4), ecx;
    for (ecx = 0; ecx < 4; ecx++) {
        *edi = R(A_PLAYERS + 4u * ecx);
        if (R(*edi + eax * 4u + 0xB4u) == 0x14u) return ecx;
    }
    return 0xFFFFFFFFu;
}

/* the finish routines' engine calls, in the original's order (the same
 * sixteen for the arcade level and the rally's end); then 0x00407CDC with
 * EAX = 0 */
static void finish_calls(const fde_services *s)
{
    static const uint32_t SEQ[] = {
        0x004157F9, 0x0040EE51, 0x00478F6B, 0x00441843, 0x0044187C, 0x0044188F,
        0x00441832, 0x004264D7, 0x0040F639, 0x0040456A, 0x00478F6B, 0x00440F60,
        0x004713BA, 0x00440F60, 0x0040F639, 0x0040F5A3 };
    size_t k;
    for (k = 0; k < sizeof SEQ / sizeof SEQ[0]; k++) call0(s, SEQ[k]);
}
static void finish_tail(const fde_services *s)
{
    uint32_t z[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    call(s, 0x00407CDCu, z);
}

/* the scene's three words: the placed player's +0x28 and +8 (twice) */
static void finish_words(vm *v, uint32_t rec)
{
    W(0x0070D06C, R(rec + 0x28));
    W(0x0070D070, R(rec + 8));
    W(0x0070CC50, R(rec + 8));
    W(0x0070CC4C, 0);
}

/* 0x00484149 (PUSHAD ... POPAD): a won arcade level's finish - player 0's
 * words, the level's number 5 - [0x70AB54] for the scene, no fireworks */
static int f484149(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    (void)r; *cf = -1;
    finish_words(v, R(A_PLAYERS));
    W(0x0070D068, 5u - R(0x0070AB54));
    W(0x0070D074, 0);
    finish_calls(s);
    finish_tail(s);
    return FDE_OK;
}

/* 0x004A5009 (PUSHAD ... POPAD): a rally's end - the scene (and the sounds
 * after it) only for the player 0x004A4FDE finds */
static int f4a5009(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    uint32_t edi, ecx;
    (void)r; *cf = -1;
    W(0x0070D074, 0);
    W(0x0070CC4C, 0);
    ecx = f4a4fde(v, &edi);
    if (ecx != 0xFFFFFFFFu) {
        finish_words(v, R(A_PLAYERS + 4u * ecx));
        W(0x0070D068, R(0x007082C4));
        finish_calls(s);
    }
    finish_tail(s);
    return FDE_OK;
}

/* 0x0049D15B (PUSHAD ... POPAD): the championship's end.  The winner opens the A8 slot ([0x70CC34]) and
 * the fireworks ([0x70D074]): the first standings row ([0x708058], 0x78 a row, -1 ends) with a class (+0x10) and a
 * place (+4), by 0x004A07BA (EBX 0x63: +4 not 0; ECX its place); when the table [0x708038] is 0x13 by 0x004A078B
 * (EBX 0: class 0, +4 >= 1; ECX counts the class rows with no place it passed), the rank +0x70 being 1.  The winner's
 * player is the one ECX - 1 names (ECX 0 wraps: the word before the pointer table).  Not the winner: the
 * rally's player 0x004A4FDE finds, none -> no scene.  Then the same engine calls as the others, with a second
 * 0x0040EE51 after 0x0040456A.  The scans' fatal exits: no row at all. */
static int fatal(vm *v, uint32_t code, uint32_t arg)
{
    W(0x004F0274u, code);
    W(0x004F0278u, arg);
    return FDE_FATAL;
}

static int f49d15b(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    static const uint32_t SEQ[] = {
        0x004157F9, 0x0040EE51, 0x00478F6B, 0x00441843, 0x0044187C, 0x0044188F,
        0x00441832, 0x004264D7, 0x0040F639, 0x0040456A, 0x0040EE51, 0x00478F6B,
        0x00440F60, 0x004713BA, 0x00440F60, 0x0040F639, 0x0040F5A3 };
    uint32_t edi, ecx = 0, edx = 0, rec;
    size_t k;
    (void)r; *cf = -1;
    W(0x0070D074, 0);
    W(0x0070CC4C, 0);
    edi = R(0x00708058);
    if (R(0x00708038) != 0x13) {
        for (;;) {                                           /* 0x004A07BA */
            if (R(edi + 0x10) != 0) {
                if (R(edi + 4) != 0) { edx = R(edi + 0x70); ecx = R(edi + 4); break; }
                ecx++;
            }
            edi += 0x78;
            if (R(edi) != 0xFFFFFFFFu) continue;
            return fatal(v, 0x8070A0AEu, 0x004DA9D9u);
        }
    } else {
        for (;;) {                                           /* 0x004A078B, EBX 0 */
            if (R(edi + 0x10) == 0) {
                if (R(edi + 4) >= 1u) { edx = R(edi + 0x70); break; }
                ecx++;
            }
            edi += 0x78;
            if (R(edi) != 0xFFFFFFFFu) continue;
            return fatal(v, 0x8070A0AFu, 0x004DA9B1u);
        }
    }
    if (edx == 1u) {
        W(0x0070CC34, 1);
        W(0x0070D074, 1);
        ecx--;
        rec = R(A_PLAYERS + 4u * ecx);
    } else {
        W(0x0070D074, 0);
        ecx = f4a4fde(v, &edi);
        if (ecx != 0xFFFFFFFFu) rec = edi;
        else { finish_tail(s); return FDE_OK; }
    }
    finish_words(v, rec);
    W(0x0070D068, R(0x007082C4));
    for (k = 0; k < sizeof SEQ / sizeof SEQ[0]; k++) call0(s, SEQ[k]);
    finish_tail(s);
    return FDE_OK;
}

/* 0x00467C65 (PUSHAD ... POPAD): the program controller's item 12 - the 0x2C bytes at [0x707F19] go to the
 * current player's record +0x50, a byte at a time in the order of REP MOVSB (the two may overlap) */
static int f467c65(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    uint32_t dst = cur_player(v) + 0x50u, k;
    (void)r; (void)s; *cf = -1;
    for (k = 0; k < 0x2Cu; k++) vm_wr8(v, dst + k, vm_rd8(v, 0x00707F19u + k));
    return FDE_OK;
}

/* 0x0040431D with EAX = 0 (PUSHAD ... POPAD): the original's display-mode table, over the data copy.
 * Not a hook in the host: its code runs.  With EAX nonzero it asks the display (0x00404834, DirectDraw) for a
 * mode and is the platform's (not ported: no caller here makes it).  The table's base is S = 0x004F0094: [S] set
 * once the table has been made, [S+4] the display 0 / 1, [S+8] the chosen mode, [S+0xC] its depth index, [S+0x10]
 * the flags, [S+0x24] the count and the modes themselves, 0x10 bytes each, from S+0x28.  The searches are the
 * original's 0x00401786 (the first 8-byte entry whose first word is EAX) and 0x00401797 (whose first word has
 * every bit of EAX), both over EBX / ECX; a search that finds nothing leaves EBX past the entries it ran over. */
static uint32_t find_eq(vm *v, uint32_t eax, uint32_t ebx, uint32_t ecx)
{
    while (ecx >= 1u) {
        if (R(ebx) == eax) return ebx;
        ebx += 8;
        if (--ecx == 0) break;
    }
    return ebx;
}
static int f40431d_zero(vm *v)
{
    enum { S = 0x004F0094 };
    uint32_t eax, ebx, ecx, edi;
    if (R(S) != 1u) {                                    /* not made yet: the display, and the two sizes of the window */
        eax = (R(0x004EF91C) & 0xF000u) != R(0x004EFE04) ? 1u : 0u;
        W(S + 4, eax);
        W(S + 0x14, R(0x00564970));
        W(S + 0x18, R(0x00564974));
    }
    W(S + 0x1C, 0);
    W(S + 0x24, 0);
    eax = R(S + 4);
    if (eax > 1u) return 1;                              /* STC: the original's way out */
    eax = R(0x004EFE04u + 4u * eax);
    ebx = 0x004EFF10u;
    ecx = R(0x004EFF0C);
    for (;;) {                                           /* 0x00401797: the first entry with every bit of EAX */
        if (ecx < 1u) return 1;
        if ((R(ebx) & eax) == eax) break;
        ebx += 8;
        if (--ecx == 0) return 1;
    }
    edi = S + 0x28;
    for (;;) {                                           /* 0x0040440C: one mode a pass (the entry is read afresh each time:
                                                          * the mode being written may be the table's own bytes) */
        uint32_t k = find_eq(v, R(ebx) & 0xFFu, 0x004EFE24u, 8);
        W(edi, R(k + 4) & 0xFFFFu);
        W(edi + 4, (R(k + 4) >> 16) & 0xFFFFu);
        k = find_eq(v, R(ebx) & 0xF00u, 0x004EFE14u, 2);
        W(edi + 8, R(k + 4));
        W(edi + 0xC, R(ebx));
        W(S + 0x24, R(S + 0x24) + 1u);
        edi += 0x10;
        ebx += 8;                                        /* 0x004017D1: on to the next entry (ECX is 1 or more here) */
        if (--ecx == 0) break;
        for (;;) {                                       /* 0x00401797 again */
            if ((R(ebx) & eax) == eax) break;
            ebx += 8;
            if (--ecx == 0) goto done;
        }
    }
done:
    W(S + 0x20, R(0x00564E68));
    W(S + 0x1C, 1);
    /* (0x00404481 .. 0x004044D0 is not ported: it chooses a mode by the old word and writes it to [S+8], which the search
     * below writes again whatever it finds, and it has no other effect - the gate runs the original's code over it) */
    eax = R(0x004EF91C) & 0xFFFu;
    ebx = 0;
    edi = S + 0x28;
    do {                                                 /* 0x004044E2: the mode the configured word names */
        if (eax == (R(edi + 0xC) & 0xFFFu)) {
            uint32_t k;
            W(S + 8, ebx);
            k = find_eq(v, R(0x004EF91C) & 0xF0000u, 0x004EFE64u, 3);
            eax = R(k + 4) - 1u;
            if ((int32_t)eax < 0) eax = 0;
            W(S + 0xC, eax);
            goto flags;
        }
        edi += 0x10;
        ebx++;
    } while (ebx < R(S + 0x24));
    W(S + 8, 0);
    W(S + 0xC, 0);
flags:
    W(S + 0x10, R(S + 0x10) & R(S + 0x20));
    W(S, 1);
    return 0;
}

/* 0x0046CC6E (no PUSHAD): VISUAL OPTIONS item 2's arrow - both flags toggled, then the display's table, EAX 0.
 * The original's PUSHAD ... POPAD around it gives back EAX = 0 and the rest as they were. */
static int f46cc6e(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    (void)s; *cf = -1;
    W(0x00707FB4, R(0x00707FB4) ^ 1u);
    W(0x004F0098, R(0x004F0098) ^ 1u);
    r[VM_EAX] = 0;
    f40431d_zero(v);
    return FDE_OK;
}

/* 0x004A6652 (no PUSHAD, no register used): START RALLY's post-draw - the
 * COMING SOON box 0x00415A0C, unless [0x70AA78] bit 17 is set, the cursor
 * [0x707E91] is not 3, or the A8 championship is unlocked ([0x70CC34] == 1) */
static int f4a6652(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    *cf = -1;
    if ((R(0x0070AA78) >> 17) & 1u) return FDE_OK;
    r[VM_EAX] = R(0x00707E91);                   /* no PUSHAD: EAX is left holding it */
    if (r[VM_EAX] != 3u) return FDE_OK;
    if (R(0x0070CC34) == 1u) return FDE_OK;
    call(s, 0x00415A0Cu, r);
    return FDE_OK;
}

/* ---- 0x0041123E: var\ral.dat, then var\hst.dat (the original's own
 * writers, in C).  The file layer is the services' (0x00402BCD open: EAX the
 * name, EBX the mode 1 -> EAX the handle, CF = failed; 0x00402C3C write: EAX
 * the handle, EBX the buffer, ECX the count -> EAX the bytes, CF = failed;
 * 0x00402C08 close: EAX the handle, CF = failed) and the clock's stamp
 * (0x00410D81 -> EAX).  The buffer is built at 0x008E8550. */
enum { A_BUF = 0x008E8550, A_RAL_NAME = 0x00523100, A_HST_NAME = 0x0052310C };

/* the 69 copies 0x004A69BB makes after its 16-byte head (source, bytes);
 * their sum, with the head and the closing 0x23232323, is 46698 */
static const struct { uint32_t src; uint16_t n; } RAL_COPY[] = {
    { 0x00512938, 4 }, { 0x004EF918, 4 }, { 0x004EF91C, 4 }, { 0x004F00A4, 4 },
    { 0x004F3284, 4 }, { 0x004F3288, 4 }, { 0x004F3298, 4 }, { 0x00511E80, 4 },
    { 0x00511E84, 4 }, { 0x00511E88, 4 }, { 0x00511E8C, 4 }, { 0x00511E90, 4 },
    { 0x005242B8, 64 }, { 0x005242F8, 4 }, { 0x0052430C, 64 }, { 0x0052434C, 4 },
    { 0x005243BC, 16 }, { 0x005186D8, 4 }, { 0x0052135C, 4 }, { 0x00522028, 4 },
    { 0x005744CC, 32 }, { 0x005744EC, 4 }, { 0x004F16AD, 4 }, { 0x004F16B1, 4 },
    { 0x004F16B5, 4 }, { 0x004F16B9, 4 }, { 0x007083B8, 652 }, { 0x00708644, 652 },
    { 0x007088D0, 652 }, { 0x00708B5C, 652 }, { 0x00707FEC, 4 }, { 0x00707FF0, 4 },
    { 0x00707FF4, 4 }, { 0x00707FF8, 4 }, { 0x00707FFC, 4 }, { 0x00708000, 4 },
    { 0x00708004, 4 }, { 0x00708008, 4 }, { 0x00707FA8, 44 }, { 0x00707FD4, 24 },
    { 0x00707BC8, 4 }, { 0x00707E34, 4 }, { 0x0070CC34, 4 }, { 0x0070AB68, 6 },
    { 0x00707BCC, 160 }, { 0x00707C6C, 80 }, { 0x006E7EE9, 10824 }, { 0x006EBFB5, 1148 },
    { 0x006EBB35, 1148 }, { 0x006EB6B5, 1148 }, { 0x006EB235, 1148 }, { 0x006EADB5, 1148 },
    { 0x006EA935, 1148 }, { 0x006EC435, 1148 }, { 0x006EC8B5, 1148 }, { 0x006ECD35, 1148 },
    { 0x006ED1B5, 1148 }, { 0x006ED635, 1148 }, { 0x006EDAB5, 1148 }, { 0x006F5624, 3120 },
    { 0x006F6254, 3120 }, { 0x006F6E84, 3120 }, { 0x006F7AB4, 3120 }, { 0x006F86E4, 3120 },
    { 0x006F9314, 3120 }, { 0x007080EC, 32 }, { 0x0070810C, 32 }, { 0x0070812C, 32 },
    { 0x0070814C, 32 },
};

/* 0x00401815: EBX the buffer, ECX the bytes: zeroed */
static void zero_bytes(vm *v, uint32_t at, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++) vm_wr8(v, at + k, 0);
}

/* the clock's stamp, the service 0x00410D81 (EAX out) */
static uint32_t stamp(const fde_services *s)
{
    uint32_t q[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    call(s, 0x00410D81u, q);
    return q[VM_EAX];
}

/* the file layer: one call at `va` with EAX / EBX / ECX; EAX and CF back */
static int file_call(const fde_services *s, uint32_t va, uint32_t eax, uint32_t ebx,
                     uint32_t ecx, uint32_t *out)
{
    uint32_t q[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    int cf;
    q[VM_EAX] = eax; q[VM_EBX] = ebx; q[VM_ECX] = ecx;
    cf = call(s, va, q);
    *out = q[VM_EAX];
    return cf;
}

/* 0x004029E0 (PUSHAD ... POPAD): the buffer EBX, ECX bytes, written to the file
 * EAX names - opened for writing, written, closed; the first failure ends it */
static void f4029e0(const fde_services *s, uint32_t name, uint32_t buf, uint32_t count)
{
    uint32_t h, junk;
    if (file_call(s, 0x00402BCDu, name, 1, count, &h)) return;
    file_call(s, 0x00402C3Cu, h, buf, count, &junk);
    file_call(s, 0x00402C08u, h, 0, 0, &junk);
}

/* 0x004A69BB (PUSHAD ... POPAD): var\ral.dat - the 16-byte head ('R5CI', the
 * version 0x50004, the stamp), the sixty-nine copies, the closing '####' */
static void f4a69bb(vm *v, const fde_services *s)
{
    uint32_t at = A_BUF + 0x10, k, j;
    zero_bytes(v, A_BUF, 0x10);
    W(A_BUF, 0x49433552);
    W(A_BUF + 8, 0x00050004);
    W(A_BUF + 0xC, stamp(s));
    for (k = 0; k < sizeof RAL_COPY / sizeof RAL_COPY[0]; k++)
        for (j = 0; j < RAL_COPY[k].n; j++, at++) vm_wr8(v, at, vm_rd8(v, RAL_COPY[k].src + j));
    W(at, 0x23232323);
    at += 4;
    f4029e0(s, A_RAL_NAME, A_BUF, at - A_BUF);
}

/* 0x004A6D9A (PUSHAD ... POPAD): var\hst.dat, when both tables were read or
 * made ([0x6F9F84] and [0x6F9F88] are 1) - the head ('R5HS'), then the six
 * blocks (pointer, bytes) at 0x6F9F94.. written one after the other */
static void f4a6d9a(vm *v, const fde_services *s)
{
    static const uint32_t PAIR[6][2] = {
        { 0x006F9F94, 0x006F9F98 }, { 0x006F9F9C, 0x006F9FA0 }, { 0x006F9FA4, 0x006F9FA8 },
        { 0x006F9FAC, 0x006F9FB0 }, { 0x006F9FC4, 0x006F9FC8 }, { 0x006F9FCC, 0x006F9FD0 } };
    uint32_t h, junk, k;
    if (R(0x006F9F84) != 1u || R(0x006F9F88) != 1u) return;
    zero_bytes(v, A_BUF, 0x10);
    W(A_BUF, 0x53483552);
    W(A_BUF + 8, 0x00050004);
    W(A_BUF + 0xC, stamp(s));
    if (file_call(s, 0x00402BCDu, A_HST_NAME, 1, 0, &h)) return;
    file_call(s, 0x00402C3Cu, h, A_BUF, 0x10, &junk);
    for (k = 0; k < 6; k++) file_call(s, 0x00402C3Cu, h, R(PAIR[k][0]), R(PAIR[k][1]), &junk);
    file_call(s, 0x00402C08u, h, 0, 0, &junk);
}

/* 0x0041123E (PUSHAD ... POPAD): the two saves, ral.dat then hst.dat */
static int f41123e(vm *v, uint32_t r[8], int *cf, const fde_services *s)
{
    (void)r; *cf = -1;
    f4a69bb(v, s);
    f4a6d9a(v, s);
    return FDE_OK;
}

static const struct { uint32_t va; fde_fn fn; } TAB[] = {
    { 0x0041123E, f41123e },
    { 0x004664C9, f4664c9 }, { 0x004759BE, f4759be }, { 0x00468114, f468114 },
    { 0x0040929E, f40929e }, { 0x0046DBE8, f46dbe8 }, { 0x0046DC0C, f46dc0c },
    { 0x0047C0F5, f47c0f5 }, { 0x00484207, f484207 }, { 0x00489D20, f489d20 },
    { 0x00484149, f484149 }, { 0x004A5009, f4a5009 }, { 0x0049D15B, f49d15b }, { 0x00467C65, f467c65 }, { 0x0046CC6E, f46cc6e }, { 0x004A6652, f4a6652 },
};

fde_fn fde_lookup(uint32_t va)
{
    size_t i;
    for (i = 0; i < sizeof TAB / sizeof TAB[0]; i++)
        if (TAB[i].va == va) return TAB[i].fn;
    return NULL;
}

int fde_count(void) { return (int)(sizeof TAB / sizeof TAB[0]); }
uint32_t fde_va(int i) { return TAB[i].va; }

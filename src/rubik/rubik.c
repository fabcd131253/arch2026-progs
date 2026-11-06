#include "rubik.h"

#include "tables.h"

/*
 * Optimal 2x2x2 solver, ported from the C reference of Homework 1
 * (fabcd131253/minirubik, c/solver_ref.c). The search is unchanged: the same
 * tables, the same order of moves, so it returns the same solution.
 *
 * Algorithm: IDA* with h = max(h_perm[p], h_ori[o]). When h <= PERIMETER_K the
 * perimeter (every state within PERIMETER_K moves, with its exact distance) is
 * looked up: a hit gives the exact distance, a miss means at least
 * PERIMETER_K + 1. A node whose distance is exact and fits the bound ends the
 * search, and the rest of the solution is read off the perimeter.
 *
 * A state is kept as the permutation rank p (0..5039) and the orientation rank
 * o (0..728), each with its own transition table, so the search never forms
 * p * 729 + o. No variable is multiplied, divided or reduced modulo, which
 * would make GCC call __mulsi3, __udivsi3 or __umodsi3:
 *   p * (7 - i)  -> times_small(): adds x, x << 1, x << 2 by the bits of k
 *   sum mod 3    -> subtract 12, 6, 3 in turn (sum <= 14)
 *   x * 3        -> (x << 1) + x
 *
 * Tables: tables.h, written by host/rubik/gen_tables.c (74,913 bytes).
 */

/* One level of the search stack: the 16-byte layout of Homework 1. */
typedef struct {
    uint16_t p, o, tp, to;
    uint8_t face, turn, last_face;
    /* cppcheck-suppress unusedStructMember */
    uint8_t pad[5]; /* fills the frame to 16 bytes */
} Frame;

/*
 * NOTE: the stack of the target is 4 KiB, so the frames and the moves are
 * static. A solution has at most 11 moves, and the search keeps at most 11
 * frames.
 */
static Frame frames[12];
static uint8_t moves[16]; /* the solution, one move (0..8) per entry */

static const char names[9][3] = {"R",  "R2", "R'", "B", "B2",
                                 "B'", "D",  "D2", "D'"};

/* x * k for k in 0..7, without a multiply. */
static uint32_t times_small(uint32_t x, uint32_t k)
{
    uint32_t r = 0;
    if (k & 1)
        r += x;
    if (k & 2)
        r += x << 1;
    if (k & 4)
        r += x << 2;
    return r;
}

/*
 * Parse and validate the 14-character state s, and compute the permutation
 * rank *p_out and the orientation rank *o_out. Returns 0, or 1 when the state
 * is invalid.
 */
static uint32_t parse(const char *s, uint32_t *p_out, uint32_t *o_out)
{
    uint8_t pos[7], oris[7];
    uint32_t seen = 0, sum = 0;
    for (uint32_t i = 0; i < 7; i++) {
        /*
         * A character below '1', NUL included, wraps to a large unsigned
         * value, so a short string stops here before reading past its end.
         */
        uint32_t c = (uint32_t) (uint8_t) s[i] - 49u;
        if (c >= 7)
            return 1;
        uint32_t bit = 1u << c;
        if (seen & bit) /* a repeated cubie */
            return 1;
        seen |= bit;
        pos[i] = (uint8_t) c;
        uint32_t d = (uint32_t) (uint8_t) s[i + 7] - 49u;
        if (d >= 3)
            return 1;
        sum += d;
        oris[i] = (uint8_t) d;
    }
    if (s[14] != 0) /* the length must be exactly 14 */
        return 1;
    if (sum >= 12) /* sum mod 3, sum <= 14 */
        sum -= 12;
    if (sum >= 6)
        sum -= 6;
    if (sum >= 3)
        sum -= 3;
    if (sum != 0) /* the orientations must sum to a multiple of 3 */
        return 1;

    uint32_t p = 0; /* p = p * (7 - i) + (later cubies smaller than pos[i]) */
    for (uint32_t i = 0; i < 7; i++) {
        uint32_t smaller = 0;
        for (uint32_t j = i + 1; j < 7; j++)
            if (pos[j] < pos[i])
                smaller++;
        p = times_small(p, 7 - i) + smaller;
    }
    uint32_t o = 0; /* the first six orientations as a base-3 number */
    for (uint32_t i = 0; i < 6; i++)
        o = (o << 1) + o + oris[i];
    *p_out = p;
    *o_out = o;
    return 0;
}

/* Select the two transition tables of a face (0 = R, 1 = B, 2 = D). */
static void load_face(uint32_t face, const uint16_t **pq, const uint16_t **oq)
{
    if (face == 0) {
        *pq = perm_qt_R;
        *oq = ori_qt_R;
    } else if (face == 1) {
        *pq = perm_qt_B;
        *oq = ori_qt_B;
    } else {
        *pq = perm_qt_D;
        *oq = ori_qt_D;
    }
}

/* Returns h; *exact = 1 means h is the exact distance. */
static uint32_t heuristic(uint32_t p, uint32_t o, uint32_t *exact)
{
    uint32_t h = h_perm[p], ho = h_ori[o];
    if (h < ho)
        h = ho;
    *exact = h == 0;     /* only the solved state has h = 0 */
    if (h > PERIMETER_K) /* cannot be inside the perimeter */
        return h;
    for (uint32_t i = bucket_start[p], end = bucket_start[p + 1]; i < end;
         i++) {
        uint32_t e = bucket_entry[i], eo = e & ENTRY_O_MASK;
        if (eo < o) /* o increases inside a bucket: not there yet */
            continue;
        if (eo != o) /* passed it: not inside the perimeter */
            break;
        *exact = 1; /* found: the exact distance */
        return e >> ENTRY_D_SHIFT;
    }
    *exact = 0;
    return PERIMETER_K + 1; /* outside the perimeter: at least K + 1 */
}

/* Perimeter lookup: the distance, or -1 outside the perimeter. */
static int32_t lookup(uint32_t p, uint32_t o)
{
    for (uint32_t i = bucket_start[p], end = bucket_start[p + 1]; i < end;
         i++) {
        uint32_t e = bucket_entry[i], eo = e & ENTRY_O_MASK;
        if (eo < o)
            continue;
        if (eo != o)
            break;
        return (int32_t) (e >> ENTRY_D_SHIFT);
    }
    return -1;
}

/*
 * Walk from a state inside the perimeter to the solved state, each step to a
 * neighbor one move closer. Writes out[0..) and returns the number of moves.
 * Called only for an exact state, so a neighbor is always found.
 */
static uint32_t finish(uint32_t p, uint32_t o, uint8_t *out)
{
    const uint16_t *pq, *oq;
    uint32_t total = (uint32_t) lookup(p, o), remaining = total;
    while (remaining != 0) {
        remaining--; /* the distance the next move reaches */
        uint32_t found = 0;
        for (uint32_t face = 0; !found; face++) {
            load_face(face, &pq, &oq);
            uint32_t tp = p, to = o;
            for (uint32_t turn = 1; turn <= 3; turn++) {
                tp = pq[tp];
                to = oq[to];
                if (lookup(tp, to) == (int32_t) remaining) {
                    *out++ = (uint8_t) ((face << 1) + face + turn - 1);
                    p = tp;
                    o = to;
                    found = 1;
                    break;
                }
            }
        }
    }
    return total;
}

/*
 * IDA* without recursion from the root (root_p, root_o). Returns the length of
 * the solution, which is left in moves[], or -1 if every node was pruned.
 */
static int32_t search(uint32_t root_p, uint32_t root_o)
{
    const uint16_t *pq = perm_qt_R, *oq = ori_qt_R;
    uint32_t exact;
    uint32_t p = root_p, o = root_o;
    uint32_t bound = heuristic(p, o, &exact); /* first bound = h(root) */
    if (exact) /* the root is already exact: read the rest off */
        return (int32_t) finish(p, o, moves);

    for (;;) { /* each iteration restarts from the root */
        uint32_t next_bound = 255, d = 0;
        Frame *f = frames;
        uint32_t tp = 0, to = 0;
        uint32_t last = 3; /* the root has no previous face */
        uint32_t face = (uint32_t) -1;
        uint32_t turn = 3; /* move on to the first face */
        p = root_p;
        o = root_o;

        for (;;) {
            if (turn == 3) { /* next face, skipping the face just turned */
                face++;
                if (face == last)
                    face++;
                if (face >= 3) { /* all three faces tried: back up */
                    if (d == 0)
                        break; /* this iteration is over */
                    f--;
                    d--;
                    p = f->p;
                    o = f->o;
                    tp = f->tp;
                    to = f->to;
                    face = f->face;
                    turn = f->turn;
                    last = f->last_face;
                    load_face(face, &pq, &oq);
                    continue;
                }
                tp = p;
                to = o;
                turn = 0;
                load_face(face, &pq, &oq);
            }
            tp = pq[tp]; /* one more quarter turn: X, X2, X' */
            to = oq[to];
            turn++;
            moves[d] = (uint8_t) ((face << 1) + face + turn - 1);
            uint32_t h = heuristic(tp, to, &exact);
            uint32_t fval = d + 1 + h; /* f = g + h, g = d + 1 */
            if (fval > bound) {        /* prune, keep the smallest excess */
                if (fval < next_bound)
                    next_bound = fval;
                continue;
            }
            if (exact) { /* exact and within the bound: a shortest solution */
                d++;
                return (int32_t) (d + finish(tp, to, moves + d));
            }
            f->p = (uint16_t) p; /* save this level and go one deeper */
            f->o = (uint16_t) o;
            f->tp = (uint16_t) tp;
            f->to = (uint16_t) to;
            f->face = (uint8_t) face;
            f->turn = (uint8_t) turn;
            f->last_face = (uint8_t) last;
            f++;
            d++;
            p = tp;
            o = to;
            last = face;
            face = (uint32_t) -1;
            turn = 3;
        }
        /*
         * NOTE: unreachable with correct tables, since every valid state is
         * at most 11 moves away. The check keeps a broken table from looping
         * forever.
         */
        if (next_bound == 255)
            return -1;
        bound = next_bound; /* the bound of the next iteration */
    }
}

/*
 * Write the len moves of moves[] as names separated by single spaces, with a
 * NUL. Returns len, or -2 when the text and its NUL need more than cap bytes;
 * then out[0] = '\0' if cap >= 1.
 */
static int write_solution(char *out, size_t cap, uint32_t len)
{
    size_t need = 1; /* the NUL */
    for (uint32_t i = 0; i < len; i++)
        need += (i != 0) + (names[moves[i]][1] ? 2u : 1u);
    if (need > cap) {
        if (cap)
            out[0] = '\0';
        return -2;
    }
    char *w = out;
    for (uint32_t i = 0; i < len; i++) {
        const char *n = names[moves[i]];
        if (i != 0)
            *w++ = ' ';
        *w++ = n[0];
        if (n[1])
            *w++ = n[1];
    }
    *w = '\0';
    return (int) len;
}

int rubik_solve(const char *state, char *out, size_t cap)
{
    uint32_t p, o;
    if (parse(state, &p, &o))
        return -1; /* out is left untouched */
    int32_t n = search(p, o);
    if (n < 0)
        return -1;
    return write_solution(out, cap, (uint32_t) n);
}

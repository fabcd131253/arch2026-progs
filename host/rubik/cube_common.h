/*
 * The cube model and the host-side tables used by gen_tables.c:
 *   - the cube model (FROM/SPIN, the source/twist tables of
 *     sysprog21/minirubik solver.c and of tests/trubik.c)
 *   - ranking and unranking of the permutation index p (0..5039) and the
 *     orientation index o (0..728)
 *   - the transition tables and the pattern databases (written to
 *     src/rubik/tables.h and src/rubik/tables.inc)
 *   - the full distance table: a host-only oracle for the checks, never
 *     written to the target program
 *
 * The search keeps a state as the two indices (p, o) and never forms
 * p * 729 + o, so the search loop on the target needs no multiply or divide.
 */
#ifndef CUBE_COMMON_H
#define CUBE_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUBIES 7
#define PERMUTATIONS 5040 /* 7! */
#define ORIENTATIONS 729  /* 3^6 */
#define STATES (PERMUTATIONS * ORIENTATIONS)

/* Each destination i takes the cubie at FROM[face][i] and adds SPIN[face][i].
 */
static const uint8_t FROM[3][CUBIES] = {
    {1, 4, 2, 0, 3, 5, 6}, /* R */
    {0, 1, 2, 4, 5, 6, 3}, /* B */
    {0, 2, 5, 3, 1, 4, 6}, /* D */
};
static const uint8_t SPIN[3][CUBIES] = {
    {1, 2, 0, 2, 1, 0, 0}, /* R */
    {0, 0, 0, 1, 2, 1, 2}, /* B */
    {0, 0, 0, 0, 0, 0, 0}, /* D */
};

typedef struct {
    uint8_t pos[CUBIES];
    uint8_t ori[CUBIES];
} Cube;

static Cube quarter_turn(Cube c, int face)
{
    Cube out;
    for (int i = 0; i < CUBIES; i++) {
        int from = FROM[face][i];
        out.pos[i] = c.pos[from];
        out.ori[i] = (c.ori[from] + SPIN[face][i]) % 3;
    }
    return out;
}

/*
 * Ranks of the permutation (Lehmer code) and of the orientation (base 3,
 * first six cubies), kept apart instead of combined into one state rank.
 */

static uint16_t perm_rank(const uint8_t *pos)
{
    uint32_t r = 0;
    for (int i = 0; i < CUBIES; i++) {
        int smaller = 0;
        for (int j = i + 1; j < CUBIES; j++)
            if (pos[j] < pos[i])
                smaller++;
        r = r * (CUBIES - i) + smaller;
    }
    return (uint16_t) r;
}

static uint16_t ori_rank(const uint8_t *ori)
{
    uint32_t r = 0;
    for (int i = 0; i < 6; i++)
        r = r * 3 + ori[i];
    return (uint16_t) r;
}

static void perm_unrank(uint32_t r, uint8_t *pos)
{
    uint8_t remaining[CUBIES] = {0, 1, 2, 3, 4, 5, 6};
    uint32_t base = 720;
    for (int i = 0; i < CUBIES; i++) {
        int choice = r / base;
        r %= base;
        pos[i] = remaining[choice];
        for (int j = choice; j + 1 < CUBIES - i; j++)
            remaining[j] = remaining[j + 1];
        if (i < 5)
            base /= (6 - i);
    }
}

static void ori_unrank(uint32_t r, uint8_t *ori)
{
    int sum = 0;
    for (int i = 5; i >= 0; i--) {
        ori[i] = r % 3;
        sum += ori[i];
        r /= 3;
    }
    ori[6] = (3 - sum % 3) % 3;
}

/*
 * Transition tables: a quarter turn of each face on p and on o,
 * 3 * 5040 * 2 + 3 * 729 * 2 = 34,614 bytes.
 */

static uint16_t perm_qt[3][PERMUTATIONS];
static uint16_t ori_qt[3][ORIENTATIONS];

static void build_transition_tables(void)
{
    Cube c;
    for (uint32_t r = 0; r < PERMUTATIONS; r++) {
        perm_unrank(r, c.pos);
        memset(c.ori, 0, sizeof c.ori);
        for (int face = 0; face < 3; face++) {
            Cube n = quarter_turn(c, face);
            perm_qt[face][r] = perm_rank(n.pos);
        }
    }
    for (uint32_t r = 0; r < ORIENTATIONS; r++) {
        for (int i = 0; i < CUBIES; i++)
            c.pos[i] = (uint8_t) i;
        ori_unrank(r, c.ori);
        for (int face = 0; face < 3; face++) {
            Cube n = quarter_turn(c, face);
            ori_qt[face][r] = ori_rank(n.ori);
        }
    }
}

/*
 * Pattern databases: exact distances of the permutation alone and of the
 * orientation alone.
 */

static uint8_t h_perm[PERMUTATIONS]; /* 5,040 bytes */
static uint8_t h_ori[ORIENTATIONS];  /*   729 bytes */

/*
 * Breadth-first search from 0 over n abstract states; qt is a [3][n]
 * transition table. Returns the number of states reached and the largest
 * distance in *max_h.
 */
static uint32_t build_pdb(uint8_t *h,
                          const uint16_t *qt,
                          uint32_t n,
                          int *max_h)
{
    static uint16_t queue[PERMUTATIONS];
    uint32_t head = 0, tail = 0;
    memset(h, 0xFF, n);
    h[0] = 0;
    queue[tail++] = 0;
    *max_h = 0;
    while (head < tail) {
        uint16_t cur = queue[head++];
        for (int face = 0; face < 3; face++) {
            uint16_t x = cur;
            for (int t = 0; t < 3; t++) {
                x = qt[face * n + x];
                if (h[x] == 0xFF) {
                    h[x] = (uint8_t) (h[cur] + 1);
                    if (h[x] > *max_h)
                        *max_h = h[x];
                    queue[tail++] = x;
                }
            }
        }
    }
    return tail;
}

/* H2: both tables are fully populated and the solved entry is 0. */
static int build_pdbs(int verbose)
{
    int max_p, max_o;
    uint32_t np = build_pdb(h_perm, &perm_qt[0][0], PERMUTATIONS, &max_p);
    uint32_t no = build_pdb(h_ori, &ori_qt[0][0], ORIENTATIONS, &max_o);
    if (verbose) {
        printf("H2 h_perm: %u/%u filled, solved=%d, max=%d\n", np,
               (unsigned) PERMUTATIONS, h_perm[0], max_p);
        printf("H2 h_ori : %u/%u filled, solved=%d, max=%d\n", no,
               (unsigned) ORIENTATIONS, h_ori[0], max_o);
    }
    return np == PERMUTATIONS && no == ORIENTATIONS && h_perm[0] == 0 &&
           h_ori[0] == 0;
}

/*
 * Full distance table (3.5 MB): a host-only oracle for the checks, not part
 * of the search.
 */

static uint8_t *build_distance_oracle(void)
{
    uint8_t *dist = malloc(STATES);
    uint32_t *queue = malloc((size_t) STATES * sizeof(uint32_t));
    if (!dist || !queue) {
        free(dist);
        free(queue);
        return NULL;
    }
    memset(dist, 0xFF, STATES);
    uint32_t head = 0, tail = 0;
    dist[0] = 0;
    queue[tail++] = 0;
    while (head < tail) {
        uint32_t here = queue[head++];
        uint16_t p = here / ORIENTATIONS, o = here % ORIENTATIONS;
        for (int face = 0; face < 3; face++) {
            uint16_t np = p, no = o;
            for (int t = 0; t < 3; t++) {
                np = perm_qt[face][np];
                no = ori_qt[face][no];
                uint32_t there = (uint32_t) np * ORIENTATIONS + no;
                if (dist[there] == 0xFF) {
                    dist[there] = (uint8_t) (dist[here] + 1);
                    queue[tail++] = there;
                }
            }
        }
    }
    free(queue);
    if (tail != STATES) {
        free(dist);
        return NULL;
    }
    return dist;
}

#endif /* CUBE_COMMON_H */

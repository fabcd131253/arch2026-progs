/*
 * gen_tables.c: host-side table generator for src/rubik/. It writes the
 * tables as a GNU assembler data file (tables.inc, for rubik.S) and, with -c,
 * as a C header with the same tables, names and order (tables.h, for
 * rubik.c).
 *
 * Tables (label / bytes per entry / entries):
 *   perm_qt_R, perm_qt_B, perm_qt_D   2  5,040  p after one quarter turn
 *   ori_qt_R,  ori_qt_B,  ori_qt_D    2    729  o after one quarter turn
 *   bucket_start                      2  5,041  start of the perimeter bucket
 *                                               of p (2 entries with -k 0)
 *   bucket_entry                      2  count  o in the low 10 bits, the
 *                                               distance in the high 6 bits
 *                                               (the solved state only with
 *                                               -k 0)
 *   h_perm                            1  5,040  distance of the permutation
 *                                               alone
 *   h_ori                             1    729  distance of the orientation
 *                                               alone
 *
 * -k 0 (IDA* with the pattern databases only): the perimeter lookup stays in
 * the search and runs only when h <= K. With K = 0 only h = 0 (the solved
 * state, p = 0) looks it up, so bucket_start[0..1] and a single entry
 * (o = 0, distance 0) are enough.
 *
 * Checks before writing:
 *   transitions: each face table is a permutation of 0..n-1, and four
 *       applications are the identity
 *   H2: the pattern databases are full and 0 at the solved state; the
 *       perimeter has the right size, the bucket starts increase and o
 *       increases inside each bucket
 *   H1: the combined heuristic never overestimates on all 3,674,160 states,
 *       and an exact value equals the true distance
 * Check after writing: the output files are read back and every value of
 * every label is compared.
 *
 * Usage: gen_tables [-k K] [-s SECTION] [-o FILE] [-c HEADER]
 *   -k K        perimeter radius 0..5, default 5; 0 is the PDB-only IDA*
 *   -s SECTION  section directive, default ".section .rodata"
 *   -o FILE     assembler data file, default tables.inc
 *   -c HEADER   also write the C header; not written without -c
 * Exit status: 0 success, 1 a check or a file failed, 2 bad arguments
 *
 * Build and regenerate from the repository root:
 *   cc -O2 -std=c99 -Wall -Wextra -o build/gen_tables host/rubik/gen_tables.c
 *   build/gen_tables -o src/rubik/tables.inc -c src/rubik/tables.h
 */
#include "cube_common.h"

#define MAX_K 5
#define MAX_PERIMETER 12224 /* states within 5 moves */
#define O_MASK 0x3FF
#define D_SHIFT 10
#define VALUES_PER_LINE 16

/* Perimeter: one bucket per permutation p, sorted by orientation o. */

static int K;
static uint16_t bucket_start[PERMUTATIONS + 1];
static uint16_t bucket_entry[MAX_PERIMETER];
static uint32_t perimeter_count;

static void build_perimeter(const uint8_t *dist)
{
    perimeter_count = 0;
    for (uint32_t p = 0; p < PERMUTATIONS; p++) {
        bucket_start[p] = (uint16_t) perimeter_count;
        for (uint32_t o = 0; o < ORIENTATIONS; o++) {
            uint8_t d = dist[p * ORIENTATIONS + o];
            if (d <= K && perimeter_count < MAX_PERIMETER)
                bucket_entry[perimeter_count++] =
                    (uint16_t) (o | (uint32_t) d << D_SHIFT);
        }
    }
    bucket_start[PERMUTATIONS] = (uint16_t) perimeter_count;
}

static int lookup(uint16_t p, uint16_t o)
{
    for (uint32_t i = bucket_start[p]; i < bucket_start[p + 1]; i++) {
        uint16_t eo = bucket_entry[i] & O_MASK;
        if (eo >= o)
            return eo == o ? bucket_entry[i] >> D_SHIFT : -1;
    }
    return -1;
}

/*
 * The combined heuristic; *exact = 1 means the value is the true distance.
 * It decides in the same order as heuristic() of rubik.c (K = 0 takes the
 * same path), and H1 checks exactly this function.
 */
static int heuristic(uint16_t p, uint16_t o, int *exact)
{
    int h = h_perm[p] > h_ori[o] ? h_perm[p] : h_ori[o];
    *exact = h == 0;
    if (h > K)
        return h;
    int d = lookup(p, o);
    if (d >= 0) {
        *exact = 1;
        return d;
    }
    return K + 1;
}

/* Checks before writing. */

/* Transitions: one-to-one, and four applications give the identity. */
static int check_transitions(void)
{
    static uint8_t seen[PERMUTATIONS];
    for (int face = 0; face < 3; face++) {
        const uint16_t *qt[2] = {perm_qt[face], ori_qt[face]};
        const uint32_t n[2] = {PERMUTATIONS, ORIENTATIONS};
        for (int t = 0; t < 2; t++) {
            memset(seen, 0, n[t]);
            for (uint32_t i = 0; i < n[t]; i++) {
                if (qt[t][i] >= n[t] || seen[qt[t][i]]++)
                    return 0;
                uint32_t x = i;
                for (int k = 0; k < 4; k++)
                    x = qt[t][x];
                if (x != i)
                    return 0;
            }
        }
    }
    return 1;
}

/* H2: the structure of the perimeter. */
static int check_perimeter(const uint8_t *dist)
{
    uint32_t expected = 0;
    for (uint32_t r = 0; r < STATES; r++)
        if (dist[r] <= K)
            expected++;
    if (perimeter_count != expected || bucket_start[0] != 0 ||
        bucket_entry[0] != 0)
        return 0;
    for (uint32_t p = 0; p < PERMUTATIONS; p++) {
        if (bucket_start[p] > bucket_start[p + 1])
            return 0;
        for (uint32_t i = bucket_start[p]; i + 1 < bucket_start[p + 1]; i++)
            if ((bucket_entry[i] & O_MASK) >= (bucket_entry[i + 1] & O_MASK))
                return 0;
    }
    return 1;
}

/* H1: no overestimate; an exact value equals the true distance. */
static int check_admissible(const uint8_t *dist,
                            uint32_t *over,
                            uint32_t *wrong_exact)
{
    *over = *wrong_exact = 0;
    for (uint32_t r = 0; r < STATES; r++) {
        int exact;
        int h = heuristic(r / ORIENTATIONS, r % ORIENTATIONS, &exact);
        if (h > dist[r])
            (*over)++;
        if (exact && h != dist[r])
            (*wrong_exact)++;
    }
    return *over == 0 && *wrong_exact == 0;
}

/* Output. */

typedef struct {
    const char *label;
    const char *comment;
    int width; /* 1 = .byte, 2 = .half */
    const void *data;
    uint32_t count;
} Table;

static uint32_t table_value(const Table *t, uint32_t i)
{
    return t->width == 2 ? ((const uint16_t *) t->data)[i]
                         : ((const uint8_t *) t->data)[i];
}

/* Every table starts at an even address (.p2align 1). */
static uint32_t align2(uint32_t x)
{
    return (x + 1) & ~1U;
}

static int write_tables(const char *path,
                        const char *section,
                        const Table *tables,
                        int ntables,
                        uint32_t *bytes)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    fprintf(f, "# %s: generated by gen_tables -k %d -s %s (do not edit)\n",
            path, K, section);
    fprintf(f, "# perimeter radius K = %d%s\n", K,
            K ? "" : " (PDB-only IDA*: perimeter holds the solved state only)");
    fprintf(f, "\n.equ PERM_COUNT, %d\n", PERMUTATIONS);
    fprintf(f, ".equ ORI_COUNT, %d\n", ORIENTATIONS);
    fprintf(f, ".equ PERIMETER_K, %d\n", K);
    fprintf(f, ".equ PERIMETER_COUNT, %u\n", perimeter_count);
    fprintf(f, ".equ ENTRY_O_MASK, 0x%X\n", O_MASK);
    fprintf(f, ".equ ENTRY_D_SHIFT, %d\n", D_SHIFT);
    fprintf(f, "\n%s\n", section);

    uint32_t offset = 0;
    for (int k = 0; k < ntables; k++) {
        const Table *t = &tables[k];
        offset = align2(offset);
        fprintf(f, "\n# %s: %u x %d bytes\n", t->comment, t->count, t->width);
        fprintf(f, "    .p2align 1\n%s:\n", t->label);
        for (uint32_t i = 0; i < t->count; i++) {
            if (i % VALUES_PER_LINE == 0)
                fprintf(f, "    %s ", t->width == 2 ? ".half" : ".byte");
            fprintf(f, "%u", table_value(t, i));
            fputs(
                i % VALUES_PER_LINE == VALUES_PER_LINE - 1 || i + 1 == t->count
                    ? "\n"
                    : ", ",
                f);
        }
        offset += t->count * (uint32_t) t->width;
    }
    *bytes = offset; /* no padding after the last table */
    fprintf(f, "\n# total: %u bytes including alignment\n", *bytes);
    return fclose(f) == 0;
}

/* Read the written file back and compare every value of every label. */
static int verify_file(const char *path, const Table *tables, int ntables)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    uint32_t got[16] = {0};
    int cur = -1, ok = 1;
    char line[1024];
    while (ok && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        size_t len = strcspn(s, "\r\n");
        s[len] = '\0';
        if (len && s[len - 1] == ':') { /* a label */
            s[len - 1] = '\0';
            cur = -1;
            for (int k = 0; k < ntables; k++)
                if (!strcmp(s, tables[k].label))
                    cur = k;
            if (cur < 0)
                ok = 0;
            continue;
        }
        int width = !strncmp(s, ".half", 5)   ? 2
                    : !strncmp(s, ".byte", 5) ? 1
                                              : 0;
        if (!width)
            continue;
        if (cur < 0 || width != tables[cur].width) {
            ok = 0;
            break;
        }
        char *q = s + 5;
        for (;;) {
            char *end;
            long v = strtol(q, &end, 0);
            if (end == q)
                break;
            const Table *t = &tables[cur];
            if (got[cur] >= t->count ||
                (uint32_t) v != table_value(t, got[cur]))
                ok = 0;
            got[cur]++;
            q = end;
            while (*q == ' ' || *q == ',')
                q++;
        }
    }
    fclose(f);
    for (int k = 0; k < ntables; k++)
        if (got[k] != tables[k].count)
            ok = 0;
    return ok;
}

/* The C header: the same tables, names and order as the data file. */
static int write_c_header(const char *path, const Table *tables, int ntables)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    fprintf(f, "/* %s: generated by gen_tables -k %d (do not edit) */\n", path,
            K);
    fprintf(f,
            "#ifndef CUBE_TABLES_H\n#define CUBE_TABLES_H\n\n#include "
            "<stdint.h>\n\n");
    fprintf(f, "/* clang-format off */\n");
    fprintf(f, "#define PERM_COUNT %d\n", PERMUTATIONS);
    fprintf(f, "#define ORI_COUNT %d\n", ORIENTATIONS);
    fprintf(f, "#define PERIMETER_K %d\n", K);
    fprintf(f, "#define PERIMETER_COUNT %u\n", perimeter_count);
    fprintf(f, "#define ENTRY_O_MASK 0x%X\n", O_MASK);
    fprintf(f, "#define ENTRY_D_SHIFT %d\n", D_SHIFT);
    for (int k = 0; k < ntables; k++) {
        const Table *t = &tables[k];
        fprintf(f, "\n/* %s: %u x %d bytes */\n", t->comment, t->count,
                t->width);
        fprintf(f, "static const %s %s[%u] = {\n",
                t->width == 2 ? "uint16_t" : "uint8_t", t->label, t->count);
        for (uint32_t i = 0; i < t->count; i++) {
            if (i % VALUES_PER_LINE == 0)
                fputs("    ", f);
            fprintf(f, "%u,", table_value(t, i));
            fputs(
                i % VALUES_PER_LINE == VALUES_PER_LINE - 1 || i + 1 == t->count
                    ? "\n"
                    : " ",
                f);
        }
        fputs("};\n", f);
    }
    fputs("\n/* clang-format on */\n#endif /* CUBE_TABLES_H */\n", f);
    return fclose(f) == 0;
}

/*
 * Read the C header back and compare: "static const uintN_t name[" selects
 * the table, each line that starts with a digit is parsed, and "};" ends the
 * table.
 */
static int verify_c_header(const char *path, const Table *tables, int ntables)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    uint32_t got[16] = {0};
    int cur = -1, ok = 1;
    char line[1024];
    while (ok && fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        int width = !strncmp(s, "static const uint16_t ", 22)  ? 2
                    : !strncmp(s, "static const uint8_t ", 21) ? 1
                                                               : 0;
        if (width) {
            const char *name = s + (width == 2 ? 22 : 21);
            size_t len = strcspn(name, "[");
            cur = -1;
            for (int k = 0; k < ntables; k++)
                if (strlen(tables[k].label) == len &&
                    !strncmp(name, tables[k].label, len))
                    cur = k;
            if (cur < 0 || tables[cur].width != width)
                ok = 0;
            continue;
        }
        if (!strncmp(s, "};", 2)) {
            cur = -1;
            continue;
        }
        if (*s < '0' || *s > '9')
            continue;
        if (cur < 0) {
            ok = 0;
            break;
        }
        char *q = s;
        for (;;) {
            char *end;
            long v = strtol(q, &end, 10);
            if (end == q)
                break;
            const Table *t = &tables[cur];
            if (got[cur] >= t->count ||
                (uint32_t) v != table_value(t, got[cur]))
                ok = 0;
            got[cur]++;
            q = end;
            while (*q == ' ' || *q == ',')
                q++;
        }
    }
    fclose(f);
    for (int k = 0; k < ntables; k++)
        if (got[k] != tables[k].count)
            ok = 0;
    return ok;
}

int main(int argc, char **argv)
{
    const char *out = "tables.inc", *section = ".section .rodata",
               *header = NULL;
    K = MAX_K;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k") && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end || v < 0 || v > MAX_K) {
                fprintf(stderr, "radius must be 0..%d\n", MAX_K);
                return 2;
            }
            K = (int) v;
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            section = argv[++i];
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            header = argv[++i];
        } else {
            fprintf(stderr,
                    "usage: %s [-k 0..%d] [-s SECTION] [-o FILE] [-c HEADER]\n",
                    argv[0], MAX_K);
            return 2;
        }
    }

    build_transition_tables();
    if (!check_transitions()) {
        fputs("transition tables: not a bijection or order does not divide 4\n",
              stderr);
        return 1;
    }
    printf("transition tables: bijective, 4 quarter turns = identity\n");

    if (!build_pdbs(1)) {
        fputs("H2 failed: pattern database not fully populated\n", stderr);
        return 1;
    }

    uint8_t *dist = build_distance_oracle();
    if (!dist) {
        fputs("could not build distance oracle\n", stderr);
        return 1;
    }
    build_perimeter(dist); /* only the solved state when K = 0 */
    if (!check_perimeter(dist)) {
        fputs("H2 failed: perimeter malformed\n", stderr);
        free(dist);
        return 1;
    }
    printf("H2 perimeter (K=%d): %u states, buckets well-formed\n", K,
           perimeter_count);
    uint32_t over, wrong_exact;
    int admissible = check_admissible(dist, &over, &wrong_exact);
    free(dist);
    printf("H1 h > d on %u states, exact-but-wrong on %u states\n", over,
           wrong_exact);
    if (!admissible)
        return 1;

    Table tables[] = {
        {"perm_qt_R", "R quarter turn on permutation index", 2, perm_qt[0],
         PERMUTATIONS},
        {"perm_qt_B", "B quarter turn on permutation index", 2, perm_qt[1],
         PERMUTATIONS},
        {"perm_qt_D", "D quarter turn on permutation index", 2, perm_qt[2],
         PERMUTATIONS},
        {"ori_qt_R", "R quarter turn on orientation index", 2, ori_qt[0],
         ORIENTATIONS},
        {"ori_qt_B", "B quarter turn on orientation index", 2, ori_qt[1],
         ORIENTATIONS},
        {"ori_qt_D", "D quarter turn on orientation index", 2, ori_qt[2],
         ORIENTATIONS},
        {"bucket_start", "perimeter bucket start, indexed by p", 2,
         bucket_start, PERMUTATIONS + 1},
        {"bucket_entry", "perimeter entries: o | distance << 10", 2,
         bucket_entry, perimeter_count},
        {"h_perm", "distance ignoring orientation", 1, h_perm, PERMUTATIONS},
        {"h_ori", "distance ignoring permutation", 1, h_ori, ORIENTATIONS},
    };
    int ntables = (int) (sizeof tables / sizeof tables[0]);
    if (!K) /* only p = 0 is looked up: bucket_start[0], [1] suffice */
        tables[6].count = 2;

    uint32_t bytes;
    if (!write_tables(out, section, tables, ntables, &bytes)) {
        fprintf(stderr, "could not write %s\n", out);
        return 1;
    }
    if (!verify_file(out, tables, ntables)) {
        fprintf(stderr, "%s: read-back does not match the tables\n", out);
        return 1;
    }
    printf(
        "wrote %s: %d tables, %u bytes (budget 131072), read-back verified\n",
        out, ntables, bytes);
    if (header) {
        if (!write_c_header(header, tables, ntables)) {
            fprintf(stderr, "could not write %s\n", header);
            return 1;
        }
        if (!verify_c_header(header, tables, ntables)) {
            fprintf(stderr, "%s: read-back does not match the tables\n",
                    header);
            return 1;
        }
        printf("wrote %s: same %d tables, read-back verified\n", header,
               ntables);
    }
    return 0;
}

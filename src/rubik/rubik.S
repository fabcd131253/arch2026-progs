# rubik_solve_asm: optimal 2x2x2 cube solver, hand-written RV32I.
#
# int rubik_solve_asm(const char *state, char *out, size_t cap)
#   a0 = state, a1 = out, a2 = cap; result in a0
#
# RV32I and Zicsr only: no mul, div, rem, no floating point, no compressed
# instructions, no heap, no recursion. Tests: tests/trubik.c.
#
# Ported from the RV32I solver of Homework 1 (fabcd131253/minirubik,
# assembly/solver.s). Algorithm: IDA* with h = max(h_perm[p], h_ori[o]); a
# state with h <= PERIMETER_K is looked up in the perimeter, which gives the
# exact distance or proves it is at least PERIMETER_K + 1.
#
# Registers of rubik_solve_asm: a0 = state, then the result; a1 = out and
# a2 = cap are kept in the frame across parse and search. The frame (68
# bytes) holds s0-s11 at 0..44, tp at 48, gp at 52, out at 56, cap at 60 and
# ra at 64. search uses tp and gp as table bases, so both are saved too.

    .include "src/rubik/tables.inc"
    .section .rodata
    .p2align 2
face_tables:                            # per face: perm_qt, ori_qt (R, B, D)
        .word perm_qt_R, ori_qt_R
        .word perm_qt_B, ori_qt_B
        .word perm_qt_D, ori_qt_D

names:  .byte 82, 0, 0, 0               # "R"; 4 bytes per move, index = move * 4
        .byte 82, 50, 0, 0              # "R2"
        .byte 82, 39, 0, 0              # "R'"
        .byte 66, 0, 0, 0               # "B"
        .byte 66, 50, 0, 0              # "B2"
        .byte 66, 39, 0, 0              # "B'"
        .byte 68, 0, 0, 0               # "D"
        .byte 68, 50, 0, 0              # "D2"
        .byte 68, 39, 0, 0              # "D'"

    .data
    .p2align 2
root_p: .half 0
root_o: .half 0
pos:    .zero 8                         # parsed permutation pos[0..6]
oris:   .zero 8                         # parsed orientation oris[0..6], right after pos
moves:  .zero 16                        # the solution, one move (0..8) per byte
frames: .zero 96                        # search stack: 12 frames of 8 bytes, one per level
                                        # +0 p, +2 o (.half); +4 face, +5 turn, +6 last_face (.byte)

    .text

    .globl  rubik_solve_asm
    .type   rubik_solve_asm, @function
    .align  2
rubik_solve_asm:
solve:

    addi    sp, sp, -68
    sw      s0, 0(sp)
    sw      s1, 4(sp)
    sw      s2, 8(sp)
    sw      s3, 12(sp)
    sw      s4, 16(sp)
    sw      s5, 20(sp)
    sw      s6, 24(sp)
    sw      s7, 28(sp)
    sw      s8, 32(sp)
    sw      s9, 36(sp)
    sw      s10, 40(sp)
    sw      s11, 44(sp)
    sw      tp, 48(sp)
    sw      gp, 52(sp)
    sw      a1, 56(sp)                  # out
    sw      a2, 60(sp)                  # cap
    sw      ra, 64(sp)

    jal     ra, parse                   # a0 = 0 on success, a1 = p, a2 = o
    bnez    a0, solve_invalid
    la      t0, root_p
    sh      a1, 0(t0)
    la      t0, root_o
    sh      a2, 0(t0)
    jal     ra, search                  # a0 = length of the solution, -1 on failure
    bltz    a0, solve_fail
    lw      a1, 56(sp)                  # reload out and cap: search clobbered a1, a2
    lw      a2, 60(sp)
    jal     ra, write_solution          # a0 = length, or -2 when cap is too small
    j       solve_return
solve_invalid:
    li      a0, -1                      # invalid state: out is left untouched
    j       solve_return
solve_fail:
    li      a0, -1                      # the search found no solution
solve_return:

    lw      s0, 0(sp)
    lw      s1, 4(sp)
    lw      s2, 8(sp)
    lw      s3, 12(sp)
    lw      s4, 16(sp)
    lw      s5, 20(sp)
    lw      s6, 24(sp)
    lw      s7, 28(sp)
    lw      s8, 32(sp)
    lw      s9, 36(sp)
    lw      s10, 40(sp)
    lw      s11, 44(sp)
    lw      tp, 48(sp)
    lw      gp, 52(sp)
    lw      a1, 56(sp)                  # out
    lw      a2, 60(sp)                  # cap
    lw      ra, 64(sp)
    addi    sp, sp, 68

    ret

# ---------------------------------------------------------------- parse
# Validate the 14-character state in a0 and compute its permutation rank p and
# orientation rank o.
# Input: a0 = state. Output: a0 = 0 and a1 = p, a2 = o on success; a0 = 1 when
# the state is invalid.
# Registers: t0 = &state[i], t1 = &pos[i] (oris[i] is at 8(t1)), t3 = loop
# end, t4 = seen (bit n set once cubie n appeared), t5 = sum of orientations,
# t6 = 7, a3 = 3, a4 = 1; a1, a2, a5 are scratch. The rank loops reuse t1, t3,
# t6 and a2-a5.
parse:
    mv      t0, a0                      # t0 = &state[0]; orientations start at 7(t0)
    la      t1, pos                     # t1 = &pos[0]; oris[i] is at 8(t1)
    addi    t3, t0, 7                   # end of the loop
    li      t4, 0                       # seen: bit n is set once cubie n appeared
    li      t5, 0                       # sum of the orientations
    li      t6, 7
    li      a3, 3
    li      a4, 1
parse_loop:
    lbu     a1, 0(t0)                   # cubie character
    addi    a1, a1, -49                 # '1' -> 0; below '1' (NUL too) wraps to large
    bgeu    a1, t6, parse_bad
    sll     a2, a4, a1                  # the bit of cubie c
    and     a5, t4, a2
    bnez    a5, parse_bad               # a repeated cubie
    or      t4, t4, a2
    sb      a1, 0(t1)                   # pos[i] = c
    lbu     a1, 7(t0)                   # orientation character
    addi    a1, a1, -49
    bgeu    a1, a3, parse_bad
    add     t5, t5, a1
    sb      a1, 8(t1)                   # oris[i] = d
    addi    t0, t0, 1
    addi    t1, t1, 1
    bne     t0, t3, parse_loop
    lbu     a1, 7(t0)                   # state[14] must be NUL: the length is exactly 14
    bnez    a1, parse_bad
    li      a2, 0x1249                  # bits 0, 3, 6, 9, 12: the multiples of 3 up to 14
    srl     a2, a2, t5
    andi    a2, a2, 1
    beqz    a2, parse_bad               # the orientations must sum to a multiple of 3

    # Permutation rank: replace pos[i] by the count of later cubies smaller than it.
    # NOTE: i only increases and the count reads only pos[j] with j > i, so
    # overwriting pos[i] is safe. The count of i = 6 is always 0, so i stops at 5.
    addi    t1, t1, -7                  # t1 = &pos[0]
    addi    t3, t1, 6                   # i = 0..5
    addi    t6, t1, 7                   # end of the inner loop (&pos[7])
parse_rank:
    lbu     a2, 0(t1)                   # pos[i]
    li      a3, 0                       # smaller
    addi    a4, t1, 1                   # j = i + 1
parse_count:
    lbu     a5, 0(a4)
    sltu    a5, a5, a2                  # 1 if pos[j] < pos[i], else 0
    add     a3, a3, a5
    addi    a4, a4, 1
    bne     a4, t6, parse_count
    sb      a3, 0(t1)                   # pos[i] = smaller
    addi    t1, t1, 1
    bne     t1, t3, parse_rank
    # p = ((((c0 * 6 + c1) * 5 + c2) * 4 + c3) * 3 + c4) * 2 + c5, with shifts and adds
    addi    t1, t1, -6                  # t1 = &pos[0]
    lbu     a1, 0(t1)                   # p = c0
    slli    a2, a1, 2
    slli    a1, a1, 1
    add     a1, a1, a2                  # * 6
    lbu     a2, 1(t1)
    add     a1, a1, a2                  # + c1
    slli    a2, a1, 2
    add     a1, a1, a2                  # * 5
    lbu     a2, 2(t1)
    add     a1, a1, a2                  # + c2
    slli    a1, a1, 2                   # * 4
    lbu     a2, 3(t1)
    add     a1, a1, a2                  # + c3
    slli    a2, a1, 1
    add     a1, a1, a2                  # * 3
    lbu     a2, 4(t1)
    add     a1, a1, a2                  # + c4
    slli    a1, a1, 1                   # * 2
    lbu     a2, 5(t1)
    add     a1, a1, a2                  # + c5

    # Orientation rank: o = o * 3 + oris[i] over the first six orientations.
    li      a2, 0
    addi    t1, t1, 8                   # t1 = &oris[0]
    addi    t3, t1, 6
parse_ori:
    lbu     a5, 0(t1)
    slli    a3, a2, 1
    add     a2, a3, a2                  # o * 3
    add     a2, a2, a5
    addi    t1, t1, 1
    bne     t1, t3, parse_ori
    li      a0, 0
    ret
parse_bad:
    li      a0, 1
    ret

# ---------------------------------------------------------------- load_face
# Load the quarter-turn tables of the face in s7 (0 = R, 1 = B, 2 = D).
# Input: s7 = face. Output: a6 = perm_qt, a7 = ori_qt. Clobbers t0.
load_face:
    bnez    s7, load_face_not_r
    la      a6, perm_qt_R
    la      a7, ori_qt_R
    ret
load_face_not_r:
    li      t0, 1
    bne     s7, t0, load_face_d
    la      a6, perm_qt_B
    la      a7, ori_qt_B
    ret
load_face_d:
    la      a6, perm_qt_D
    la      a7, ori_qt_D
    ret

# ---------------------------------------------------------------- heuristic
# Input: a0 = p, a1 = o. Output: a0 = h, a1 = 1 when h is the exact distance.
# Uses gp = h_perm, tp = h_ori, a4 = bucket_start, a5 = bucket_entry, all set
# by search. Clobbers t0-t5.
heuristic:
    add     t0, gp, a0
    lbu     t0, 0(t0)                   # h_perm[p]
    add     t1, tp, a1
    lbu     t1, 0(t1)                   # h_ori[o]
    bgeu    t0, t1, heuristic_max
    mv      t0, t1
heuristic_max:                          # t0 = h = max(h_perm[p], h_ori[o])
    seqz    t2, t0                      # only the solved state has h = 0: exact
    li      t1, PERIMETER_K
    bltu    t1, t0, heuristic_out       # h > K: cannot be inside the perimeter
    slli    t3, a0, 1
    add     t3, a4, t3                  # &bucket_start[p]
    lhu     t4, 0(t3)                   # first entry of bucket p
    lhu     t5, 2(t3)                   # end of bucket p (start of bucket p + 1)
heuristic_scan:
    bgeu    t4, t5, heuristic_miss      # bucket exhausted: not inside the perimeter
    slli    t3, t4, 1
    add     t3, a5, t3
    lhu     t3, 0(t3)                   # entry = o | distance << 10
    andi    t1, t3, 1023                # the o of the entry
    bltu    t1, a1, heuristic_next      # o increases inside a bucket: not there yet
    bne     t1, a1, heuristic_miss      # passed it: not inside the perimeter
    srli    t0, t3, 10                  # found: the exact distance
    li      t2, 1
    j       heuristic_out
heuristic_next:
    addi    t4, t4, 1
    j       heuristic_scan
heuristic_miss:
    li      t0, PERIMETER_K
    addi    t0, t0, 1                   # outside the perimeter: at least K + 1
    li      t2, 0
heuristic_out:
    mv      a0, t0                      # a0 = h
    mv      a1, t2                      # a1 = 1 if exact, else 0
    ret

# ---------------------------------------------------------------- lookup
# Perimeter lookup, the same scan as heuristic.
# Input: a0 = p, a1 = o; a4 = bucket_start, a5 = bucket_entry.
# Output: a0 = the distance, or -1 outside the perimeter. Clobbers t1, t3-t5.
lookup:
    slli    t3, a0, 1
    add     t3, a4, t3
    lhu     t4, 0(t3)
    lhu     t5, 2(t3)
lookup_scan:
    bgeu    t4, t5, lookup_miss
    slli    t3, t4, 1
    add     t3, a5, t3
    lhu     t3, 0(t3)
    andi    t1, t3, 1023
    bltu    t1, a1, lookup_next
    bne     t1, a1, lookup_miss
    srli    a0, t3, 10
    ret
lookup_next:
    addi    t4, t4, 1
    j       lookup_scan
lookup_miss:
    # NOTE: finish calls lookup only for states inside the perimeter, so this
    # path is not taken there.
    li      a0, -1
    ret

# ---------------------------------------------------------------- search
# IDA* without recursion from (root_p, root_o).
# Output: a0 = length of the solution, left in moves[]; -1 if every node was
# pruned.
# Registers: s0 = d (depth), s1 = limit = bound - g with g = d + 1, s2 = the
# smallest excess of a pruned node in this round, s3 = &frames[d],
# s4 = moves, s5/s6 = p/o of the current node, s7 = face, s8 = turn,
# s9 = last face, s10/s11 = tp/to of the child; a0 = 3, a1 = PERIMETER_K,
# a2 = PERIMETER_K + 1, a3 = bound, a4 = bucket_start, a5 = bucket_entry,
# a6/a7 = perm_qt/ori_qt of the face, gp = h_perm, tp = h_ori,
# ra = face_tables (the return address is saved on the stack); t0-t5 are
# scratch. The caller saved s0-s11, gp and tp.
search:

    addi    sp, sp, -4
    sw      ra, 0(sp)                   # ra is reused as a base below
    la      gp, h_perm
    la      tp, h_ori
    la      a4, bucket_start
    la      a5, bucket_entry
    la      s4, moves
    la      t0, root_p
    lhu     s5, 0(t0)                   # p of the root
    la      t0, root_o
    lhu     s6, 0(t0)                   # o of the root
    mv      a0, s5
    mv      a1, s6
    jal     ra, heuristic               # the first bound = h(root)
    mv      a3, a0                      # a3 = bound
    beqz    a1, search_setup
    li      s0, 0                       # the root is exact: read the rest off, d = 0
    mv      a0, s5
    mv      a1, s6
    mv      a2, s4
    jal     ra, finish
    j       search_return

search_setup:                           # constants of the search loop
    li      a0, 3                       # 3 turns per face, 3 faces
    li      a1, PERIMETER_K
    addi    a2, a1, 1                   # h = K + 1 outside the perimeter
    la      ra, face_tables             # base of the quarter-turn table pointers

search_iteration:                       # each round restarts from the root
    li      s2, 255                     # smallest excess (f - bound) of a pruned node
    addi    s1, a3, -1                  # limit = bound - g; a child of the root has g = 1
    li      s0, 0
    la      s3, frames                  # &frames[0]
                                        # s5, s6 still hold the root: every round
                                        # backs up to it
    li      s9, 3                       # the root has no previous face
    li      s7, -1
    li      s8, 3                       # turn = 3: move on to the first face

search_loop:
    bne     s8, a0, search_turn         # this face has turns left
    addi    s7, s7, 1                   # next face, skipping the face just turned
    bne     s7, s9, search_face_ok
    addi    s7, s7, 1
search_face_ok:
    bgeu    s7, a0, search_pop          # all three faces tried: back up
    mv      s10, s5                     # turn from the state of this node
    mv      s11, s6
    li      s8, 0
    slli    t0, s7, 3                   # face_tables[face]: perm_qt, ori_qt
    add     t0, ra, t0
    lw      a6, 0(t0)
    lw      a7, 4(t0)

search_turn:                            # one more quarter turn: X, X2, X'
    slli    t0, s10, 1
    add     t0, a6, t0
    lhu     s10, 0(t0)                  # tp = perm_qt[face][tp]
    slli    t0, s11, 1
    add     t0, a7, t0
    lhu     s11, 0(t0)                  # to = ori_qt[face][to]
    addi    s8, s8, 1                   # turn++
    # Inlined heuristic: t0 = max(h_perm[tp], h_ori[to]).
    add     t0, gp, s10
    lbu     t0, 0(t0)
    add     t1, tp, s11
    lbu     t1, 0(t1)
    bgeu    t0, t1, search_hmax
    mv      t0, t1
search_hmax:
    bltu    a1, t0, search_plain        # h > K: cannot be inside the perimeter
    slli    t3, s10, 1                  # bucket tp: [bucket_start[tp], bucket_start[tp + 1])
    add     t3, a4, t3
    lhu     t4, 0(t3)
    lhu     t5, 2(t3)
    bgeu    t4, t5, search_miss         # empty bucket
    slli    t4, t4, 1
    add     t4, a5, t4                  # pointer to the first entry
    slli    t5, t5, 1
    add     t5, a5, t5                  # pointer past the last entry
search_scan:
    lhu     t3, 0(t4)                   # entry = o | distance << 10
    andi    t1, t3, 1023
    bltu    t1, s11, search_scan_next   # o increases inside a bucket: not there yet
    bne     t1, s11, search_miss        # passed it: not inside the perimeter
    srli    t0, t3, 10                  # found: the exact distance
    bltu    s1, t0, search_prune        # distance > limit: prune
    j       search_found                # exact and within the bound: a shortest solution
search_scan_next:
    addi    t4, t4, 2
    bltu    t4, t5, search_scan
search_miss:
    mv      t0, a2                      # outside the perimeter: h = K + 1
search_plain:                           # h is a lower bound, not exact
    bltu    s1, t0, search_prune        # h > limit, i.e. g + h > bound: prune
    slli    t1, s7, 1                   # not pruned: record the move, go one deeper
    add     t1, t1, s7
    add     t1, t1, s8
    addi    t1, t1, -1                  # move = face * 3 + turn - 1
    add     t3, s4, s0
    sb      t1, 0(t3)                   # moves[d] = move
    sh      s5, 0(s3)                   # save this level: p, o, face, turn, last_face
    sh      s6, 2(s3)                   # tp, to need no slot: on the way back they
    sb      s7, 4(s3)                   # are the child's p, o, still in s5, s6
    sb      s8, 5(s3)
    sb      s9, 6(s3)
    addi    s3, s3, 8
    addi    s0, s0, 1                   # d++
    addi    s1, s1, -1                  # one level deeper: g + 1, limit - 1
    mv      s5, s10
    mv      s6, s11
    mv      s9, s7
    li      s7, -1
    li      s8, 3
    j       search_loop

search_prune:                           # keep the smallest excess h - limit (= f - bound >= 1)
    sub     t1, t0, s1
    bgeu    t1, s2, search_loop
    mv      s2, t1
    j       search_loop

search_pop:
    beqz    s0, search_iteration_end
    addi    s3, s3, -8                  # back to the previous level, try its next move
    addi    s0, s0, -1                  # d--
    addi    s1, s1, 1                   # one level up: g - 1, limit + 1
    mv      s10, s5                     # tp, to of this level = p, o of the child just left
    mv      s11, s6                     # (must come before s5, s6 are reloaded)
    lhu     s5, 0(s3)                   # p, o, face, turn, last_face of this level
    lhu     s6, 2(s3)
    lbu     s7, 4(s3)
    lbu     s8, 5(s3)
    lbu     s9, 6(s3)
    slli    t0, s7, 3
    add     t0, ra, t0
    lw      a6, 0(t0)
    lw      a7, 4(t0)
    j       search_loop

search_iteration_end:
    # NOTE: unreachable with correct tables, since every valid state is at most
    # 11 moves away. The check keeps a broken table from looping forever.
    li      t0, 255
    beq     s2, t0, search_fail         # no node was pruned
    add     a3, a3, s2                  # next bound = bound + smallest excess
    j       search_iteration

search_found:                           # t0 = the remaining exact distance
    slli    t1, s7, 1
    add     t1, t1, s7
    add     t1, t1, s8
    addi    t1, t1, -1                  # move = face * 3 + turn - 1
    add     t3, s4, s0
    sb      t1, 0(t3)                   # moves[d] = move
    addi    s0, s0, 1                   # g = d + 1
    mv      a0, s10
    mv      a1, s11
    add     a2, s4, s0
    jal     ra, finish                  # read the rest of the solution off the perimeter
    add     a0, a0, s0                  # a0 = g + distance inside the perimeter
    j       search_return
search_fail:
    li      a0, -1
search_return:
    lw      ra, 0(sp)
    addi    sp, sp, 4

    ret

# ---------------------------------------------------------------- finish
# Walk from a state inside the perimeter to the solved state, each step to a
# neighbor one move closer.
# Input: a0 = p, a1 = o, a2 = where to write the moves; a4 = bucket_start and
# a5 = bucket_entry, set by search. Output: a0 = the number of moves.
# Registers: s1 = write pointer, s2 = remaining distance, s3 = total,
# s5/s6 = p/o, s7 = face, s8 = turn, s10/s11 = tp/to, a6/a7 = tables of the
# face (load_face). Called once at the end of the search, so it may clobber
# every s register but s0 (the caller saved them).
finish:
    addi    sp, sp, -4
    sw      ra, 0(sp)
    mv      s5, a0                      # p
    mv      s6, a1                      # o
    mv      s1, a2                      # write pointer into moves[]
    jal     ra, lookup                  # a0 = distance inside the perimeter
    mv      s3, a0                      # total number of moves
    mv      s2, a0                      # moves remaining
finish_step:
    beqz    s2, finish_done
    addi    s2, s2, -1                  # the distance the next move reaches
    li      s7, 0                       # start from face R
finish_face:
    jal     ra, load_face               # a6, a7 = tables of face s7
    mv      s10, s5                     # tp = p
    mv      s11, s6                     # to = o
    li      s8, 0
finish_turn:
    slli    t0, s10, 1
    add     t0, a6, t0                  # a6 = perm_qt of the face
    lhu     s10, 0(t0)
    slli    t0, s11, 1
    add     t0, a7, t0                  # a7 = ori_qt of the face
    lhu     s11, 0(t0)
    addi    s8, s8, 1                   # turn++
    mv      a0, s10                     # lookup(tp, to)
    mv      a1, s11
    jal     ra, lookup
    beq     a0, s2, finish_take         # one move closer: take it
    li      t0, 3
    bne     s8, t0, finish_turn         # turns left on this face
    addi    s7, s7, 1                   # next face; some neighbor is always closer
    j       finish_face
finish_take:
    slli    t0, s7, 1                   # face * 2
    add     t0, t0, s7                  # face * 3
    add     t0, t0, s8                  # face * 3 + turn
    addi    t0, t0, -1                  # move = face * 3 + turn - 1
    sb      t0, 0(s1)
    addi    s1, s1, 1                   # next slot of moves[]
    mv      s5, s10                     # p = tp
    mv      s6, s11                     # o = to
    j       finish_step
finish_done:
    mv      a0, s3                      # a0 = total number of moves
    lw      ra, 0(sp)
    addi    sp, sp, 4
    ret

# ---------------------------------------------------------------- write_solution
# Write the moves as names separated by single spaces, with a NUL, checking
# before each move that it and the NUL still fit in cap bytes.
# Input: a0 = len, a1 = out, a2 = cap. Output: a0 = len, or -2 when the text
# and its NUL need more than cap bytes; then out[0] = '\0' if cap >= 1, and
# nothing is written if cap = 0.
# Registers: a1 = write pointer, a3 = names, a4 = out, a5 = moves,
# a6 = need (bytes so far, NUL included), t0 = i, t1-t4 = scratch,
# t5 = 1 when the name has a second character, t6 = ' '.
write_solution:
    beqz    a2, write_abort             # cap = 0: not even the NUL fits
    beqz    a0, write_end               # no moves: only the NUL

    li      a6, 1                       # need = 1 for the NUL
    la      a5, moves
    mv      a4, a1                      # keep out for write_abort
    la      a3, names
    li      t0, 0                       # i = 0

write_loop:
    add     t1, a5, t0
    lbu     t2, 0(t1)                   # moves[i]
    slli    t2, t2, 2                   # 4 bytes per name
    add     t2, t2, a3
    lbu     t3, 0(t2)                   # first character of the name
    lbu     t4, 1(t2)                   # second character, or 0

    # Count the bytes of this move before writing it.
write_need:
    addi    a6, a6, 1                   # the first character
    sltu    t5, x0, t0                  # t5 = 1 if i > 0: a space before the name
    add     a6, a6, t5
    sltu    t5, x0, t4                  # t5 = 1 if there is a second character ('2' or ''')
    add     a6, a6, t5
    bltu    a2, a6, write_abort         # need > cap: does not fit

    # It fits: write it.
    beq     t0, x0, write_no_blank
    li      t6, 32                      # ' '
    sb      t6, 0(a1)
    addi    a1, a1, 1

write_no_blank:
    sb      t3, 0(a1)
    bne     t5, x0, write_two_symbol
    addi    a1, a1, 1                   # one character
    j       write_next
write_two_symbol:
    sb      t4, 1(a1)
    addi    a1, a1, 2                   # two characters

write_next:

    addi    t0, t0, 1
    blt     t0, a0, write_loop

write_end:
    sb      x0, 0(a1)                   # NUL; a0 still holds len
    ret

write_abort:
    li      a0, -2
    beqz    a2, write_abort_skip        # cap = 0: write nothing
    sb      x0, 0(a4)                   # out[0] = '\0'
write_abort_skip:
    ret

    .size   rubik_solve_asm, .-rubik_solve_asm

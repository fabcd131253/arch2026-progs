# op1, op2, op3 and q_mul: hand-written RV32I.
#
# uint32_t op1_asm(uint32_t a, uint32_t b, int *n)   a0, a1, a2 -> a0
# uint32_t op2_asm(uint32_t a, uint32_t b)           a0, a1 -> a0
# uint32_t op3_asm(uint32_t a, uint32_t b)           a0, a1 -> a0
# int32_t  q_mul_asm(int32_t a, int32_t b)           a0, a1 -> a0
#
# RV32I and Zicsr only: no mul, div, rem, no floating point, no compressed
# instructions. q_mul forms a 64-bit product in a register pair. Tests:
# tests/tfixed.c.

    .text

# op1_asm: a + b by passing the carries until none is left.
# a0 = a (the partial sum), a1 = b (the carries), a2 = n,
# t0 = c, t1 = the pass count.
    .globl  op1_asm
    .type   op1_asm, @function
    .align  2
op1_asm:
    li      t1, 0           # *n = 0
    beqz    a1, loop_end
loop:
    and     t0, a0, a1      # t0 = c
    slli    t0, t0, 1
    xor     a0, a0, a1
    addi    a1, t0, 0       # b = c
    addi    t1, t1, 1       # *n
    bne     a1, x0, loop
loop_end:
    sw      t1, 0(a2)
    ret
    .size   op1_asm, .-op1_asm

# op2_asm: a - b. a0 = a, a1 = b.
    .globl  op2_asm
    .type   op2_asm, @function
    .align  2
op2_asm:
    sub     a0, a0, a1      # a = a - b
    ret
    .size   op2_asm, .-op2_asm

# op3_asm: floor((a + b) / 2). a0 = a, a1 = b,
# t0 = a >> 1, t1 = b >> 1, t2 = a & b & 1.
    .globl  op3_asm
    .type   op3_asm, @function
    .align  2
op3_asm:
    srli    t0, a0, 1
    srli    t1, a1, 1
    and     t2, a0, a1
    andi    t2, t2, 1       # t2 = a & b & 1
    add     t0, t0, t1
    add     a0, t0, t2

    ret
    .size   op3_asm, .-op3_asm

# q_mul_asm: Q11.20 product from four 16-bit by 16-bit partial products.
# a0 = a, a1 = b, t0 = al, t1 = bl, t2 = ah, t3 = bh,
# t4 = al * bl, t5 = ah * bl, a5 = al * bh, a6 = ah * bh,
# a3 to a6 = the shifted multiplicands, t6 = the tested bit or a carry,
# then t0 = the high word and t1 = the low word of the product.
    .globl  q_mul_asm
    .type   q_mul_asm, @function
    .align  2
q_mul_asm:
    # treat a0 and a1 as unsigned
    slli    t0, a0, 16
    srli    t0, t0, 16          # keep lower 16 bits
    slli    t1, a1, 16
    srli    t1, t1, 16          # [t0, t1] = [al, bl]

    srli    t2, a0, 16
    srli    t3, a1, 16          # [t2, t3] = [ah, bh]

    li      t4, 0               # t4 is the result of al * bl
    li      t5, 0               # t5 is the result of ah * bl
    beqz    t1, mul_end_g1
    addi    a5, t0, 0           # a5, a6 are shifted multiplicands
    addi    a6, t2, 0
    # do al * bl and ah * bl at the same time
loop_g1:
    andi    t6, t1, 1               # mask
    beqz    t6, mul_not_taken_g1    # compare
    add     t4, t4, a5              # add
    add     t5, t5, a6              # add
mul_not_taken_g1:
    srli    t1, t1, 1               # multiplier right shift
    slli    a5, a5, 1
    slli    a6, a6, 1
    bne     t1, x0, loop_g1
    # [t4, t5] = [al * bl, ah * bl]; t0, t2, t3 are in use
mul_end_g1:
    li      a5, 0               # a5 is the result of al * bh
    li      a6, 0               # a6 is the result of ah * bh
    beqz    t3, mul_end_g2
    addi    a3, t0, 0
    addi    a4, t2, 0
loop_g2:
    andi    t6, t3, 1               # mask
    beqz    t6, mul_not_taken_g2    # compare
    add     a5, a5, a3              # add
    add     a6, a6, a4              # add
mul_not_taken_g2:
    srli    t3, t3, 1               # shift
    slli    a3, a3, 1
    slli    a4, a4, 1
    bne     t3, x0, loop_g2
mul_end_g2:
    # a0 = a = ua, a1 = b = ub
    # t4 is al * bl, t5 is ah * bl, a5 is al * bh, a6 is ah * bh
    # t2, t3 are temp
    srli    t2, t5, 16          # high 16 bits of ah * bl
    srli    t3, a5, 16          # high 16 bits of al * bh
    add     t0, t2, t3
    add     t0, t0, a6          # (a6 is already high 32 bits of 64-bit product)

    slli    t2, t5, 16          # low 16 bits of ah * bl, putting on bit [31:16]
    slli    t3, a5, 16          # low 16 bits of al * bh, putting on bit [31:16]
    add     t1, t2, t3
    sltu    t6, t1, t2          # carry
    add     t0, t0, t6          # carry to high 32 bit
    add     t1, t1, t4
    sltu    t6, t1, t4
    add     t0, t0, t6
    # t0 is ph, t1 is pl

    bge     a0, x0, pos_a
    sub     t0, t0, a1
pos_a:
    bge     a1, x0, pos_b
    sub     t0, t0, a0
pos_b:
    li      t2, 524288      # 1 << (Q - 1) = 524288
    add     t1, t1, t2
    sltu    t3, t1, t2
    add     t0, t0, t3      # carry

    srli    t1, t1, 20
    slli    t0, t0, 12
    or      a0, t0, t1
    ret
    .size   q_mul_asm, .-q_mul_asm

# gcd_u32_asm: binary GCD, hand-written RV32I.
#
# uint32_t gcd_u32_asm(uint32_t a, uint32_t b)   a in a0, b in a1, result in a0
#
# RV32I and Zicsr only: no mul, div, rem, no floating point, no compressed
# instructions. Tests: tests/tgcd.c.
#
# Registers: a0 = a, a1 = b, a6 = the count of shared twos (shift),
# t0 = the tested bit.

    .text

    .globl  gcd_u32_asm
    .type   gcd_u32_asm, @function
    .align  2
gcd_u32_asm:

    bne     a0, x0, zero_b
# zero_a:
    addi    a0, a1, 0
    ret
zero_b:
    bne     a1, x0, continue_s1
    ret

continue_s1:

    li      a6, 0               # a6 = shift
    or      t0, a0, a1          # (a | b)
    andi    t0, t0, 1           # (a | b) & 1
    bnez    t0, continue_s2
loop_both_even:
    srli    a0, a0, 1
    srli    a1, a1, 1
    addi    a6, a6, 1

    or      t0, a0, a1          # (a | b)
    andi    t0, t0, 1           # (a | b) & 1
    beqz    t0, loop_both_even

continue_s2:

    andi    t0, a0, 1           # a & 1
    bnez    t0, continue_s3
loop_a_even_outer:
    srli    a0, a0, 1
    andi    t0, a0, 1           # a & 1
    beqz    t0, loop_a_even_outer


continue_s3:

    andi    t0, a1, 1           # b & 1
    bnez    t0, continue_s4
loop_b_even_outer:
    srli    a1, a1, 1
    andi    t0, a1, 1           # b & 1
    beqz    t0, loop_b_even_outer

continue_s4:

    bgeu    a1, a0, a_lt_b      # b >= a: subtract a from b
a_sub_b:
    sub     a0, a0, a1          # a = a - b
loop_a_even_inner:
    srli    a0, a0, 1
    andi    t0, a0, 1
    beqz    t0, loop_a_even_inner
    bltu    a1, a0, a_sub_b     # b < a: subtract again, else fall through

a_lt_b:
    sub     a1, a1, a0          # b = b - a
    bnez    a1, loop_b_even_outer   # reuse while (!(b & 1)) loop

    sll     a0, a0, a6
    ret
    .size   gcd_u32_asm, .-gcd_u32_asm

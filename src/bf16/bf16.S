# BFloat16 routines: hand-written RV32I.
#
# uint16_t bf16_from_f32_asm(uint32_t f32bits)   a0 -> a0
# uint16_t bf16_mul_asm(uint16_t a, uint16_t b)  a0, a1 -> a0
# uint16_t bf16_sqrt_asm(uint16_t a)             a0 -> a0
#
# RV32I and Zicsr only: no mul, div, rem, no floating point, no compressed
# instructions. Tests: tests/tbf16.c.

    .text

# bf16_from_f32_asm registers: a0 = f32bits, then the result; t0 = the
# exponent field in place, then the rounding increment; t1 = 0x7F800000 (the
# exponent mask, also its all-ones value), then 0x7FFF.
    .globl  bf16_from_f32_asm
    .type   bf16_from_f32_asm, @function
    .align  2
bf16_from_f32_asm:

    lui     t1, 0x7F800
    and     t0, a0, t1              # t0 = f32bits & 0x7F800000 (exponent field)
    beq     t0, t1, return          # exponent all ones: Inf or NaN, no rounding

    srli    t0, a0, 16
    andi    t0, t0, 1               # (f32bits >> 16) & 1
    li      t1, 0x7FFF
    add     t0, t0, t1              # ((f32bits >> 16) & 1) + 0x7FFF
    add     a0, a0, t0

return:
    srli    a0, a0, 16
    ret
    .size   bf16_from_f32_asm, .-bf16_from_f32_asm

# bf16_mul_asm registers: a0 = a, a1 = b, t0 = the result sign (bit 15),
# t1 = 0x7F80 (the exponent mask and Inf), t2 and t3 = the exponents of a and
# b shifted left by 7 (t2 becomes the result exponent), t4 = 0x7F, t5 and t6 =
# the significands of a and b, a4 = the tested bit, a5 = the product,
# a6 = the exponent adjustment shifted left by 7. a5, a6, t5 and t6 are also
# scratch.
    .globl  bf16_mul_asm
    .type   bf16_mul_asm, @function
    .align  2
bf16_mul_asm:
    li      t0, 0x8000              # sign mask
    and     t1, a0, t0
    and     t2, a1, t0
    xor     t0, t1, t2              # t0 is result_sign

    li      t1, 0x7F80              # exp mask
    and     t2, a0, t1              # exp-masked a
    and     t3, a1, t1              # exp-masked b

    li      t4, 0x007F              # mant mask
    and     t5, a0, t4              # mant-masked a
    and     t6, a1, t4              # mant-masked b

    bne     t2, t1, b_nan_check
    beqz    t5, a_inf_b_zero
    ret
a_inf_b_zero:                       # a is inf, b is zero
    or      a6, t3, t6
    bnez    a6, inf_default
    li      a0, 0x7FC0
    ret
inf_default:
    or      a0, t0, t1              # a0 = result_sign | (BF16_INF == 0x7F80)
    ret

b_nan_check:
    bne     t3, t1, one_zero
    beqz    t6, b_inf_a_zero
    addi    a0, a1, 0
    ret
b_inf_a_zero:
    or      a6, t2, t5
    bnez    a6, inf_default
    li      a0, 0x7FC0
    ret

one_zero:
    or      a5, t2, t5
    or      a6, t3, t6
    beqz    a5, zero_return
    beqz    a6, zero_return
    j       continue_s1
zero_return:
    addi    a0, t0, 0
    ret

continue_s1:

    li      a6, 0                   # a6 = exp_adjust
    bnez    t2, a_small_default     # if condition

    li      t2, 128                 # exp_a = 1 (shifted)
a_small_loop:
    andi    a5, t5, 128             # 128 = 0x80
    bnez    a5, b_small
    slli    t5, t5, 1
    addi    a6, a6, -128
    j       a_small_loop

a_small_default:
    ori     t5, t5, 128             # 128 = 0x80

b_small:
    bnez    t3, b_small_default     # if condition

    li      t3, 128                 # exp_b = 1 (shifted)
b_small_loop:
    andi    a5, t6, 128             # 128 = 0x80
    bnez    a5, continue_s2
    slli    t6, t6, 1
    addi    a6, a6, -128
    j       b_small_loop

b_small_default:
    ori     t6, t6, 128             # 128 = 0x80

continue_s2:

    li      a5, 0                   # a5 is result_mant
    # mant_a * mant_b (t5 * t6)
mul_loop:
    andi    a4, t6, 1               # a4 is masked mant_b
    beqz    a4, mul_not_taken
    add     a5, a5, t5
mul_not_taken:
    srli    t6, t6, 1               # shift mant_b; it is not used again
    slli    t5, t5, 1               # shifted mant_a
    bnez    t6, mul_loop
    # t5, t6 are freed

    add     t2, t2, t3              # exp_a + exp_b (shifted)
    add     t2, t2, a6              # exp_a + exp_b + exp_adjusted (shifted)
    li      t6, 0x3F80              # 127 << 7
    sub     t2, t2, t6              # t2 is result_exp
    # t3 is freed

    li      t6, 0x8000
    and     t5, a5, t6
    beqz    t5, hidden_one_default
    srli    a5, a5, 8
    andi    a5, a5, 127             # 127 = 0x7F
    addi    t2, t2, 128
    j       overflow_check

hidden_one_default:
    srli    a5, a5, 7
    andi    a5, a5, 127

overflow_check:
    bge     t2, t1, inf_default     # t1 = 0x7F80

    li      t6, 127
    blt     t6, t2, no_underflow    # 111_1111 < result_exp
    li      t6, -768                # -6 << 7 = -768
    bge     t2, t6, underflow

    addi    a0, t0, 0
    ret

underflow:
    li      t6, 128
    sub     t2, t6, t2
    srli    t2, t2, 7
    srl     a5, a5, t2
    li      t2, 0

no_underflow:
    add     a0, t0, t2              # result_sign + result_exp
    add     a0, a0, a5
    ret
    .size   bf16_mul_asm, .-bf16_mul_asm

# bf16_sqrt_asm registers: a0 = a, t0 = the sign, t1 = the exponent, t2 = the
# fraction, a6 = m, a5 = the new exponent, t6 = n (the remainder), t5 = res,
# t4 = bit, t3 = t. t3 and t6 are also scratch.
    .globl  bf16_sqrt_asm
    .type   bf16_sqrt_asm, @function
    .align  2
bf16_sqrt_asm:

    srli    t0, a0, 15
    andi    t0, t0, 1               # t0 = sign
    srli    t1, a0, 7
    andi    t1, t1, 255             # t1 = exp
    andi    t2, a0, 127             # t2 = mant

    li      t3, 0xFF
    bne     t1, t3, nan_default

    beqz    t2, inf_check
    ret
inf_check:
    beqz    t0, inf_pos
    li      a0, 0x7FC0
    ret
inf_pos:
    ret

nan_default:
    or      t3, t1, t2
    bnez    t3, not_zero
    li      a0, 0
    ret
not_zero:
    beqz    t0, not_neg
    li      a0, 0x7FC0
    ret
not_neg:
    bnez    t1, continue_sqrt_s1
    li      a0, 0
    ret

continue_sqrt_s1:

    ori     a6, t2, 128             # a6 = m

    andi    t6, t1, 1               # exp & 1
    beqz    t6, exp_even
    addi    a5, t1, 127
    srli    a5, a5, 1
    j       continue_sqrt_s2
exp_even:
    addi    a5, t1, 126
    srli    a5, a5, 1
    slli    a6, a6, 1               # m <<= 1

continue_sqrt_s2:

    slli    t6, a6, 7
    ori     t6, t6, 127             # t6 = n   = 128m + 127 = (m << 7) + 127
    li      t5, 16384               # t5 = res = 1 << 14 = 16384
    li      t4, 4096                # t4 = bit = 1 << 12 = 4096
    sub     t6, t6, t5

loop:
    add     t3, t4, t5
    srli    t5, t5, 1

    bltu    t6, t3, bigger          # n < t
    sub     t6, t6, t3
    add     t5, t5, t4
bigger:
    srli    t4, t4, 2
    bnez    t4, loop

    slli    a5, a5, 7
    andi    t5, t5, 127
    or      a0, a5, t5
    ret
    .size   bf16_sqrt_asm, .-bf16_sqrt_asm

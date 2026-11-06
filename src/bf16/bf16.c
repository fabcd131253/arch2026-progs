#include "bf16.h"

/* BFloat16 routines of Quiz 1 Problem B, adapted to the contract in bf16.h.
 * Every value travels as its bit pattern; no float is used in arithmetic.
 */

#define BF16_MANT_MASK 0x007Fu
#define BF16_EXP_BIAS 127
#define BF16_NAN 0x7FC0u
#define BF16_INF 0x7F80u

uint16_t bf16_from_f32(uint32_t f32bits)
{
    /* Inf and NaN: keep the top half verbatim, so the rounding constant can
     * never carry out of an all-ones exponent.
     */
    if (((f32bits >> 23) & 0xFF) == 0xFF)
        return (uint16_t) (f32bits >> 16);

    /* Round to nearest, ties to even: 0x7FFF plus the lowest kept bit. */
    f32bits += ((f32bits >> 16) & 1) + 0x7FFF;
    return (uint16_t) (f32bits >> 16);
}

uint16_t bf16_mul(uint16_t a, uint16_t b)
{
    uint32_t sign_a = (a >> 15) & 1, sign_b = (b >> 15) & 1;
    int32_t exp_a = (a >> 7) & 0xFF, exp_b = (b >> 7) & 0xFF;
    uint32_t mant_a = a & BF16_MANT_MASK, mant_b = b & BF16_MANT_MASK;
    uint32_t result_sign = (sign_a ^ sign_b) << 15;

    /* a is examined first: a NaN a wins, and an infinite a swallows b. */
    if (exp_a == 0xFF) {
        if (mant_a)
            return a;
        if (!exp_b && !mant_b)
            return BF16_NAN;
        return (uint16_t) (result_sign | BF16_INF);
    }
    if (exp_b == 0xFF) {
        if (mant_b)
            return b;
        if (!exp_a && !mant_a)
            return BF16_NAN;
        return (uint16_t) (result_sign | BF16_INF);
    }

    /* Neither a nor b is NaN or Inf. */
    if ((!exp_a && !mant_a) || (!exp_b && !mant_b))
        return (uint16_t) result_sign;

    /* Restore the hidden one; normalize a denormal by shifting its leading
     * one up to bit 7 and charging the shifts to the exponent.
     */
    int32_t exp_adjust = 0;
    if (!exp_a) {
        while (!(mant_a & 0x80)) {
            mant_a <<= 1;
            exp_adjust--;
        }
        exp_a = 1;
    } else {
        mant_a |= 0x80;
    }
    if (!exp_b) {
        while (!(mant_b & 0x80)) {
            mant_b <<= 1;
            exp_adjust--;
        }
        exp_b = 1;
    } else {
        mant_b |= 0x80;
    }

    /* 8-bit by 8-bit product in [0x4000, 0xFE01]: renormalize by one bit when
     * it reaches bit 15, then truncate to the 7 fraction bits.
     */
    uint32_t result_mant = mant_a * mant_b;
    int32_t result_exp = exp_a + exp_b - BF16_EXP_BIAS + exp_adjust;
    if (result_mant & 0x8000) {
        result_mant = (result_mant >> 8) & 0x7F;
        result_exp++;
    } else {
        result_mant = (result_mant >> 7) & 0x7F;
    }

    if (result_exp >= 0xFF) /* Overflow */
        return (uint16_t) (result_sign | BF16_INF);
    if (result_exp <= 0) { /* Underflow */
        if (result_exp < -6)
            return (uint16_t) result_sign;
        result_mant >>= 1 - result_exp;
        result_exp = 0;
    }
    return (uint16_t) (result_sign | ((uint32_t) result_exp << 7) |
                       result_mant);
}

uint16_t bf16_sqrt(uint16_t a)
{
    uint32_t sign = (a >> 15) & 1;
    uint32_t exp = (a >> 7) & 0xFF;
    uint32_t mant = a & BF16_MANT_MASK;

    if (exp == 0xFF) {
        if (mant)
            return a; /* NaN */
        if (sign)
            return BF16_NAN; /* -Inf */
        return a;            /* +Inf */
    }
    if (!exp && !mant)
        return 0x0000; /* +0 and -0 */
    if (sign)          /* A negative sqrt is not allowed */
        return BF16_NAN;
    if (!exp)
        return 0x0000; /* denormal */

    /* sqrt(2^e * m) = 2^(e/2) * sqrt(m): an odd e is made even by doubling
     * m. e may be negative, and >> on a negative int is implementation-defined
     * (GCC shifts arithmetically), as in the quiz code.
     */
    int32_t e = (int32_t) exp - BF16_EXP_BIAS;
    int32_t new_exp;
    uint32_t m = 0x80 | mant;
    if (e & 1) {
        m <<= 1;
        new_exp = ((e - 1) >> 1) + BF16_EXP_BIAS;
    } else {
        new_exp = (e >> 1) + BF16_EXP_BIAS;
    }

    /* Largest mid in [90, 256] with (mid * mid) / 128 <= m. */
    uint32_t low = 90, high = 256, result = 128;
    while (low <= high) {
        uint32_t mid = (low + high) >> 1;
        uint32_t sq = (mid * mid) >> 7;
        if (sq <= m) {
            result = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    /* Renormalize into [128, 256). */
    if (result >= 256) {
        result >>= 1;
        new_exp++;
    } else {
        while (result < 128 && new_exp > 1) {
            result <<= 1;
            new_exp--;
        }
    }

    if (new_exp >= 0xFF)
        return BF16_INF;
    if (new_exp <= 0)
        return 0x0000;
    return (uint16_t) (((uint32_t) new_exp << 7) | (result & 0x7F));
}

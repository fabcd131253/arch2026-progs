#include "fixed.h"

/* Quiz 3 Problem C: arithmetic built from logic, and Q11.20 fixed point. */

/* a + b from bitwise operators: a ^ b is the sum without carries, and
 * (a & b) << 1 is the carries moved one place up, so a + b is unchanged by
 * every pass. The carry out of bit 31 is shifted out, giving the sum modulo
 * 2^32 in at most 32 passes.
 */
uint32_t op1(uint32_t a, uint32_t b, int *n)
{
    *n = 0;
    while (b) {
        uint32_t c = (a & b) << 1;
        a = a ^ b;
        b = c;
        ++*n;
    }
    return a;
}

/* a - b modulo 2^32. The bitwise model is op1(a, op1(~b, 1)), since
 * -b = ~b + 1 in two's complement; the contract asks for the plain
 * subtraction, which gives the same result.
 */
uint32_t op2(uint32_t a, uint32_t b)
{
    return a - b;
}

/* floor((a + b) / 2) without overflow: halving each operand loses one unit
 * exactly when both are odd, and a & b & 1 puts it back.
 */
uint32_t op3(uint32_t a, uint32_t b)
{
    return (a >> 1) + (b >> 1) + (a & b & 1);
}

/* Q11.20 product: (a * b + 2^19) >> 20, keeping the low 32 bits.
 *
 * The quiz writes ((int64_t) a * b + (1 << (Q - 1))) >> Q, but a 64-bit
 * multiply makes GCC call __muldi3, which the runtime does not supply. The
 * product is built instead from four 16-bit by 16-bit partial products, each
 * a 32-bit multiply (__mulsi3), and corrected for the signs.
 */
int32_t q_mul(int32_t a, int32_t b)
{
    uint32_t ua = (uint32_t) a, ub = (uint32_t) b;
    uint32_t al = ua & 0xFFFF, ah = ua >> 16;
    uint32_t bl = ub & 0xFFFF, bh = ub >> 16;

    /* Unsigned 64-bit product of the two bit patterns. */
    uint64_t p = (uint64_t) (al * bl) + ((uint64_t) (al * bh) << 16) +
                 ((uint64_t) (ah * bl) << 16) + ((uint64_t) (ah * bh) << 32);

    /* Read as signed, a negative a stands for ua - 2^32, so the signed
     * product is the unsigned one minus ub * 2^32, and likewise for b. Only
     * the high word changes, modulo 2^64.
     */
    if (a < 0)
        p -= (uint64_t) ub << 32;
    if (b < 0)
        p -= (uint64_t) ua << 32;

    /* Add half a unit, then shift by Q. The result keeps bits 20 to 51 of
     * the sum, which an arithmetic and a logical shift agree on, so the
     * unsigned shift gives the contract's wrap-around result.
     */
    p += (uint64_t) 1 << (Q - 1);
    return (int32_t) (uint32_t) (p >> Q);
}

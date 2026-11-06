#include "gcd.h"

/* Binary GCD of Quiz 2 Problem A (proc_a), written in C. It rests on four
 * identities for unsigned a and b:
 *   gcd(0, b) = b
 *   gcd(2a, 2b) = 2 * gcd(a, b)
 *   gcd(2a, b) = gcd(a, b)            when b is odd
 *   gcd(a, b) = gcd(a, b - a)         when a, b are both odd and b >= a
 * No multiply, divide or remainder is needed: only shifts, compares and
 * subtractions.
 */
uint32_t gcd_u32(uint32_t a, uint32_t b)
{
    /* gcd(0, b) = b and gcd(a, 0) = a, gcd(0, 0) = 0. */
    if (!a)
        return b;
    if (!b)
        return a;

    /* Strip the twos both operands share and count them. Both are nonzero,
     * so the count stays below 32 and the final shift is defined.
     */
    unsigned shift = 0;
    /* !((a | b) & 1) is !(a & 1) && !(b & 1): both are even. */
    while (!((a | b) & 1)) {
        a >>= 1;
        b >>= 1;
        shift++;
    }

    /* At least one operand is odd now; make a odd on its own. */
    while (!(a & 1)) /* a is even, which means b is odd */
        a >>= 1;
    do {
        /* a is odd now, then make b odd too */
        while (!(b & 1))
            b >>= 1;

        /* Keep b the larger one, comparing as unsigned. */
        if (a > b) {
            uint32_t t = a;
            a = b;
            b = t;
        }
        b -= a; /* b is even */
    } while (b);

    /* Put the shared twos back. */
    return a << shift;
}

/*
 * 64-bit integer division helpers normally supplied by libgcc/compiler-rt.
 *
 * The kernel links with -nostdlib and only pulls in libgcc.a when a 32-bit
 * (i386) variant of it exists. On toolchains without gcc-multilib the
 * archive resolves to the host x86_64 one, so the linker reports undefined
 * references to __udivdi3 & friends for every 64-bit division in the tree.
 * These implementations are plain shift-and-subtract long division, which
 * i386 can do natively (it has no 64-bit divide instruction) and which needs
 * no floating point or libgcc support of its own.
 */

typedef unsigned long long u64;
typedef long long s64;

/* Divide n by d, store the remainder in *rem (when non-NULL) and return the
 * quotient. Bit-by-bit long division: r keeps the partial remainder, which
 * is always < d, so the next step is (r << 1) | next_dividend_bit. When d's
 * high bit is set that shift can carry out of 64 bits; msb records the carry
 * so the subtract still uses the mathematically correct (wrapped) value. */
static u64 udivmod64(u64 n, u64 d, u64 *rem)
{
    u64 q = 0;
    u64 r = 0;
    int i;

    if (d == 0)
    {
        /* libgcc leaves division by zero undefined; never hang the kernel. */
        if (rem)
            *rem = 0;
        return 0;
    }

    for (i = 63; i >= 0; i--)
    {
        u64 msb = r >> 63;
        r = (r << 1) | ((n >> i) & 1ULL);
        if (msb || r >= d)
        {
            r -= d; /* wraps correctly when msb was set */
            q |= 1ULL << i;
        }
    }

    if (rem)
        *rem = r;
    return q;
}

u64 __udivmoddi4(u64 n, u64 d, u64 *rem)
{
    return udivmod64(n, d, rem);
}

u64 __udivdi3(u64 n, u64 d)
{
    return udivmod64(n, d, 0);
}

u64 __umoddi3(u64 n, u64 d)
{
    u64 r = 0;
    udivmod64(n, d, &r);
    return r;
}

/* C99 semantics: the quotient truncates toward zero and the remainder takes
 * the sign of the dividend. Negate through unsigned math so INT64_MIN stays
 * well defined. */
s64 __divdi3(s64 n, s64 d)
{
    int negative = (n < 0) != (d < 0);
    u64 un = (n < 0) ? (0ULL - (u64)n) : (u64)n;
    u64 ud = (d < 0) ? (0ULL - (u64)d) : (u64)d;
    u64 q = udivmod64(un, ud, 0);

    return negative ? (s64)(0ULL - q) : (s64)q;
}

s64 __moddi3(s64 n, s64 d)
{
    u64 un = (n < 0) ? (0ULL - (u64)n) : (u64)n;
    u64 ud = (d < 0) ? (0ULL - (u64)d) : (u64)d;
    u64 r = 0;

    udivmod64(un, ud, &r);
    return (n < 0) ? (s64)(0ULL - r) : (s64)r;
}

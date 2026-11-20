/* SPDX-License-Identifier: MIT */
#include "sltypes.h"

u32 sl_udiv(u32 n, u32 d)
{
    u32 q = 0, r = 0;
    int i;
    if (!d)
        return 0;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= 1u << i;
        }
    }
    return q;
}

int sl_sdiv(int a, int b)
{
    u32 ua = a < 0 ? 0u - (u32)a : (u32)a, ub = b < 0 ? 0u - (u32)b : (u32)b, q;
    if (!ub)
        return 0;
    q = sl_udiv(ua, ub);
    return (a < 0) != (b < 0) ? -(int)q : (int)q;
}

u32 sl_hash(const u8 *p, u32 n)
{
    u32 h = 2166136261u, i;
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

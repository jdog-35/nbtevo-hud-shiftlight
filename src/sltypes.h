/* SPDX-License-Identifier: MIT */
/* Types and integer helpers shared by every module. Pure C: also built on the Mac for the host tests. */
#ifndef SL_TYPES_H
#define SL_TYPES_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

/* n / d by shift and subtract (the head unit's libc has no __aeabi_uidiv); 0 when d is 0 */
u32 sl_udiv(u32 n, u32 d);
/* a / b rounded toward zero; 0 when b is 0 */
int sl_sdiv(int a, int b);
/* FNV-1a over n bytes */
u32 sl_hash(const u8 *p, u32 n);
/* now - t in microseconds (now_us() values, which wrap every 71 minutes); 0 when t is up to ~1 s after now: a time
   another thread stamped (a message's arrival, the main loop's progress) a moment after `now` was read */
u32 us_since(u32 t, u32 now);

#endif

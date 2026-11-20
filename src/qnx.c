/* SPDX-License-Identifier: MIT */
#include "qnx.h"

/* clang's EABI block helpers; QNX's libc.so.3 does not export them (st1ng, probe.cpp) */
#define HIDDEN __attribute__((visibility("hidden")))
HIDDEN void __aeabi_memcpy(void *d, const void *s, unsigned long n) { memcpy(d, s, n); }
HIDDEN void __aeabi_memcpy4(void *d, const void *s, unsigned long n) { memcpy(d, s, n); }
HIDDEN void __aeabi_memcpy8(void *d, const void *s, unsigned long n) { memcpy(d, s, n); }
HIDDEN void __aeabi_memset(void *d, unsigned long n, int c) { memset(d, c, n); }
HIDDEN void __aeabi_memset4(void *d, unsigned long n, int c) { memset(d, c, n); }
HIDDEN void __aeabi_memset8(void *d, unsigned long n, int c) { memset(d, c, n); }
HIDDEN void __aeabi_memclr(void *d, unsigned long n) { memset(d, 0, n); }
HIDDEN void __aeabi_memclr4(void *d, unsigned long n) { memset(d, 0, n); }
HIDDEN void __aeabi_memclr8(void *d, unsigned long n) { memset(d, 0, n); }

void out(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    if (n)
        write(1, s, n);
}

u32 now_us(void)
{
    struct sl_timespec t;
    clock_gettime(QNX_CLOCK_MONOTONIC, &t);
    return (u32)t.sec * 1000000u + (u32)t.nsec / 1000u;
}

void ms1(char *b, u32 us)
{
    snprintf(b, 16, "%u.%u", us / 1000u, (us % 1000u) / 100u);
}

void sl_lock(volatile int *l)
{
    while (__sync_lock_test_and_set(l, 1))
        usleep(1000);
}

void sl_unlock(volatile int *l) { __sync_lock_release(l); }

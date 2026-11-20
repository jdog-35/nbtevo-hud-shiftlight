/* SPDX-License-Identifier: MIT */
/* The QNX libc.so.3 functions the head-unit modules use, declared here (no QNX headers in this build: clang and ld.lld
 * only, freestanding, like st1ng's build), the QNX constants they need, and small helpers (qnx.c). */
#ifndef SL_QNX_H
#define SL_QNX_H

#include "sltypes.h"

void *dlopen(const char *, int);
void *dlsym(void *, const char *);
char *dlerror(void);
int pthread_create(int *, const void *, void *(*)(void *), void *);
int usleep(unsigned); /* less than 1 s */
struct sl_timespec {
    long sec;
    long nsec;
};
int clock_gettime(int, struct sl_timespec *);
long write(int, const void *, unsigned long);
long read(int, void *, unsigned long);
int open(const char *, int, ...);
int close(int);
int snprintf(char *, unsigned long, const char *, ...);
char *getenv(const char *);
void *memcpy(void *, const void *, unsigned long);
void *memset(void *, int, unsigned long);
void _exit(int);
typedef void (*sl_sighandler)(int);
sl_sighandler signal(int, sl_sighandler);
int kill(int, int);
int getpid(void);
int unlink(const char *);

#define QNX_CLOCK_MONOTONIC 2
#define QNX_RTLD_NOW_GLOBAL 0x102 /* RTLD_NOW | RTLD_GLOBAL, as ID6's shift-deck-qnx passes them */
#define QNX_O_RDONLY 0
#define QNX_O_WRONLY 1
#define QNX_O_CREAT 0x100
#define QNX_O_TRUNC 0x200
#define QNX_O_APPEND 8
#define QNX_SIGHUP 1
#define QNX_SIGINT 2
#define QNX_SIGPIPE 13
#define QNX_SIGTERM 15

void out(const char *s); /* to stdout, one write */
#define OUTF(...)                                \
    do {                                         \
        char l_[384];                            \
        snprintf(l_, sizeof l_, __VA_ARGS__);    \
        out(l_);                                 \
    } while (0)
u32 now_us(void);                    /* monotonic; wraps every 71 minutes, only differences are used */
void ms1(char *b, u32 us);           /* "12.3" (ms, one decimal) into b[16] */
void sl_lock(volatile int *l);       /* sleeping spin lock (QNX: a pure spin can starve the owner) */
void sl_unlock(volatile int *l);
#define SL_BARRIER() __sync_synchronize()

#endif

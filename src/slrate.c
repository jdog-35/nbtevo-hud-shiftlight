/* SPDX-License-Identifier: MIT
 *
 * libslrate.so: how fast engine speed comes back over the head unit's MOST UDS tunnel (no ENET cable needed).
 * HUD shift light, step 3. Runs on hu-omap (NBT Evo ID6, QNX 6.5) as an LD_PRELOAD of /bin/cat: the constructor does the
 * whole test and _exit()s, so cat itself never runs (st1ng's freestanding LD_PRELOAD method: no QNX SDK, no crt).
 *
 *   LD_LIBRARY_PATH=<the HMI's> DSI_IP_ADDRESS=<the HMI's> LD_PRELOAD=/var/opt/shiftlight-dev/libslrate.so /bin/cat
 *
 * Environment:
 *   SL_N=200         requests to send (1..2000)
 *   SL_GAP=0         ms to wait between requests (0..999)
 *   SL_TIMEOUT=1000  ms to wait for each reply (100..5000); a "pending" reply extends it, to 5 s at most
 *   SL_DID=F40C      send UDS 22 <DID> (ReadDataByIdentifier) instead of OBD 01 0C
 *   SL_QUIET=1       the summary only
 *   SL_TRACE=1       every DSI record as well
 *
 * Read-only by construction: the only requests it can send to the DME (0x12) are OBD 01 0C and UDS 22 <DID>.
 *
 * The DSI client and the tunnel framing follow ID6 Apps (MIT License, Copyright (c) 2026 ID6 Apps contributors),
 * source/native/src/most/dsi_abi.h, dsi_client.c, most_link.c, most_proto.c. See ../NOTICE.
 * Changes: one file, no pthread mutex/cond (a spin lock and polling instead, so no QNX struct layouts are needed),
 * timing and statistics added.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

/* QNX libc.so.3, declared here (no QNX headers in this build) */
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
char *dlerror(void);
int pthread_create(int *, const void *, void *(*)(void *), void *);
int usleep(unsigned);
struct sl_timespec {
    long sec;
    long nsec;
};
int clock_gettime(int, struct sl_timespec *);
long write(int, const void *, unsigned long);
int snprintf(char *, unsigned long, const char *, ...);
char *getenv(const char *);
void *memcpy(void *, const void *, unsigned long);
void *memset(void *, int, unsigned long);
void _exit(int);

#define QNX_CLOCK_MONOTONIC 2
#define QNX_RTLD_NOW_GLOBAL 0x102 /* RTLD_NOW | RTLD_GLOBAL, as ID6's shift-deck-qnx passes them */

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

/* ---- output, time, small helpers ---------------------------------------------------------------------------------- */

static void out(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    if (n)
        write(1, s, n);
}
#define OUTF(...)                                \
    do {                                         \
        char l_[256];                            \
        snprintf(l_, sizeof l_, __VA_ARGS__);    \
        out(l_);                                 \
    } while (0)

static u32 now_us(void) /* wraps every 71 minutes; only differences are used */
{
    struct sl_timespec t;
    clock_gettime(QNX_CLOCK_MONOTONIC, &t);
    return (u32)t.sec * 1000000u + (u32)t.nsec / 1000u;
}

static u32 udiv(u32 n, u32 d) /* no __aeabi_uidiv in libc.so.3 either */
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

static void ms1(char *b, u32 us) /* "12.3" (ms, one decimal) */
{
    snprintf(b, 16, "%u.%u", us / 1000u, (us % 1000u) / 100u);
}

static volatile int g_lock;
static void lock(void)
{
    while (__sync_lock_test_and_set(&g_lock, 1))
        usleep(1000);
}
static void unlock(void) { __sync_lock_release(&g_lock); }

/* ---- the DSI library (libsys_dsi_base), as ID6's dsi_abi.h reconstructs it ---------------------------------------- */

#define DSI_APP_BYTES 460
#define DSI_APP_ON_CONNECTED 0xCC    /* void (*)(void *app) */
#define DSI_APP_ON_DISCONNECTED 0xD0 /* void (*)(void *app) */
#define DSI_APP_ON_RESPONSE 0xD4     /* void (*)(void *app, void *stream, const void *event) */
#define DSI_APP_ON_EXIT_LOOP 0xDC    /* void (*)(void *app) */
#define DSI_EVENT_TYPE 4             /* u32; 0x201 invalid; 0x204, 0x205 rejected */
#define DSI_EVENT_ID 8               /* u32 */
#define DSI_STREAM_ERROR 0x14        /* int; non-zero after a failed read or write */
#define STREAM_WORDS 64              /* 256 bytes for a stream object (ID6 uses 64; extra room costs nothing) */
#define MAX_MSG_LEN 0x1000

struct dsi_fns {
    void (*Init)(void);
    int (*ClientInit)(void *app, const char *ifname, const char *role, int major, int minor);
    int (*ClientRun)(void *app);
    int (*ClientStop)(void *app, int unused);
    void (*ClientFree)(void *app);
    int (*ClientSendRegister)(void *app, const u32 *ids, int n, u32 session);
    int (*ClientSetNotification)(void *app, u32 attr);
    int (*ClientSendRequest)(void *app, u32 id, u32 session, void *stream);
    u32 (*CreateId)(void);
    void (*Open)(void *stream);
    void (*Close)(void *stream);
    int (*Write)(void *stream, const void *p, int size, int align);
    int (*Read)(void *stream, void *p, int size, int align);
    int (*WriteString2)(void *stream, const char **s);
};
static struct dsi_fns D;
static const char *const k_dsi_names[] = {
    "DSIInit", "DSIClientInit", "DSIClientRun", "DSIClientStop", "DSIClientFree", "DSIClientSendRegister",
    "DSIClientSetNotification", "DSIClientSendRequest", "DSICreateId", "DSIOpen", "DSIClose", "DSIWrite",
    "DSIRead", "DSIWriteString2"
};
typedef char sl_table_matches_names[sizeof(struct dsi_fns) == sizeof k_dsi_names ? 1 : -1];

static int dsi_bind(void)
{
    void *lib, *f;
    unsigned i;
    lib = dlopen("libsys_dsi_base.cpp-ne.so.4", QNX_RTLD_NOW_GLOBAL);
    if (!lib)
        lib = dlopen("/opt/sys/lib/libsys_dsi_base.cpp-ne.so.4", QNX_RTLD_NOW_GLOBAL);
    if (!lib) {
        const char *e = dlerror();
        OUTF("slrate: MOST unavailable: %s\n", e ? e : "dlopen failed");
        return -1;
    }
    for (i = 0; i < sizeof k_dsi_names / sizeof k_dsi_names[0]; i++) {
        f = dlsym(lib, k_dsi_names[i]);
        if (!f) {
            OUTF("slrate: MOST unavailable: no %s\n", k_dsi_names[i]);
            return -1;
        }
        memcpy((char *)&D + i * sizeof f, &f, sizeof f);
    }
    return 0;
}

/* ---- the DSYSMOSTServer client (ID6 dsi_client.c) ------------------------------------------------------------------ */

#define TUNNEL_FB 0x06
#define TUNNEL_INST 0xF0
#define TUNNEL_KEY 0x06F0 /* FBlock 06, instance F0: the UDS tunnel */
#define HUD_KEY 0x8200    /* FBlock 82, instance 00: the HUDTerminal (only reported, never used here) */
#define TUNNEL_FKT 0x050
#define TUNNEL_ROUTINE 0x0F0B
#define DIAG_SRC 0x63
#define DME 0x12

static u32 g_app_mem[(DSI_APP_BYTES + 3) / 4 + 16] __attribute__((aligned(8)));
#define APP ((void *)g_app_mem)

static int g_trace;
static volatile int g_running, g_connected, g_have_client, g_term_up, g_disconnected, g_thread_done;
static volatile int g_init_rc, g_run_rc;
static volatile u32 g_client;
static volatile u16 g_device, g_term_dev;
static volatile int g_registry_seen, g_key_listed, g_hud_listed;
static volatile u16 g_key_dev, g_hud_dev;

/* the request in flight: set up by the main thread, answered on the DSI thread */
static volatile int g_waiting, g_reply_ready, g_pending, g_refused, g_negative;
static volatile u8 g_refused_nrc, g_negative_nrc;
static u8 g_req[3];
static volatile unsigned g_req_len;
static u8 g_reply[32];
static volatile unsigned g_reply_len;
static volatile u32 g_reply_us;

static int stream_failed(const void *stream)
{
    int e;
    memcpy(&e, (const char *)stream + DSI_STREAM_ERROR, sizeof e);
    return e != 0;
}
static void w32(void *s, u32 v) { D.Write(s, &v, 4, 4); }
static void w16(void *s, u16 v) { D.Write(s, &v, 2, 2); }
static void w8(void *s, u8 v) { D.Write(s, &v, 1, 1); }

/* Sends the open stream as request id, then closes it. Caller holds the lock. */
static int finish(void *stream, u32 id, u32 session)
{
    int rc = -1;
    if (!g_running) {
        if (g_trace)
            OUTF("dsi: request 0x%02x: the client is gone\n", id);
    } else if (stream_failed(stream)) {
        OUTF("dsi: request 0x%02x: stream write failed\n", id);
    } else if (D.ClientSendRequest(APP, id, session, stream) == -1) {
        OUTF("dsi: request 0x%02x: DSIClientSendRequest failed\n", id);
    } else {
        rc = 0;
    }
    D.Close(stream);
    return rc;
}

static int key_request(u32 id, u16 key) /* 0x01 register target, 0x05 unregister */
{
    u32 stream[STREAM_WORDS] __attribute__((aligned(8)));
    int rc;
    if (!g_running || !g_have_client)
        return -1;
    lock();
    D.Open(stream);
    w32(stream, g_client);
    w32(stream, 1);
    w16(stream, key);
    rc = finish(stream, id, D.CreateId());
    unlock();
    return rc;
}

static int unregister_client(void) /* request 0x10 */
{
    u32 stream[STREAM_WORDS] __attribute__((aligned(8)));
    int rc;
    if (!g_running || !g_have_client)
        return -1;
    lock();
    D.Open(stream);
    rc = finish(stream, 0x10, D.CreateId());
    unlock();
    return rc;
}

/* Request 0x02 on the control channel, to the tunnel's device. */
static int send_fn(u8 fb, u8 inst, u16 fkt, u8 op, const u8 *payload, u32 n)
{
    u32 stream[STREAM_WORDS] __attribute__((aligned(8)));
    int rc;
    if (!g_running || !g_have_client || !g_term_up || n > MAX_MSG_LEN)
        return -1;
    lock();
    D.Open(stream);
    w32(stream, g_client);
    w32(stream, 0); /* transport: control channel */
    w32(stream, 1); /* kind: message */
    w32(stream, 0); /* flags */
    w16(stream, g_term_dev);
    w16(stream, g_device);
    w8(stream, fb);
    w8(stream, inst);
    w16(stream, fkt);
    w8(stream, op);
    w32(stream, g_client & 0xFFFFu);
    w32(stream, n);
    if (n)
        D.Write(stream, payload, (int)n, 1);
    rc = finish(stream, 0x02, D.CreateId());
    unlock();
    return rc;
}

/* Callbacks, on the DSI thread. */

static void on_connected(void *app)
{
    static const u32 ids[12] = { 0x80000000u, 0x80000001u, 0x80000002u, 0x80000003u, 0x80000004u, 0x80000005u,
                                 0x80000006u, 0x80000007u, 0xC0000000u, 0xC0000001u, 0xC0000002u, 0xC0000004u };
    static const u32 attrs[4] = { 0xC0000002u, 0xC0000004u, 0xC0000001u, 0xC0000000u };
    u32 stream[STREAM_WORDS] __attribute__((aligned(8)));
    const char *name = "shiftlight";
    u32 session = D.CreateId();
    int i, r;
    (void)app;
    if (g_trace)
        out("dsi: connected\n");
    lock();
    r = D.ClientSendRegister(APP, ids, 12, session);
    if (r < 0)
        OUTF("dsi: DSIClientSendRegister failed: %d\n", r);
    D.Open(stream);
    D.WriteString2(stream, &name);
    finish(stream, 0x0F, session);
    for (i = 0; i < 4; i++) {
        r = D.ClientSetNotification(APP, attrs[i]);
        if (r < 0)
            OUTF("dsi: DSIClientSetNotification(0x%08x) failed: %d\n", attrs[i], r);
    }
    unlock();
    g_connected = 1;
}

static void on_disconnected(void *app)
{
    (void)app;
    out("dsi: disconnected from DSYSMOSTServer\n");
    g_have_client = 0;
    g_term_up = 0;
    g_disconnected = 1;
}

static void on_exit_loop(void *app)
{
    (void)app;
    if (g_trace)
        out("dsi: loop exit\n");
}

#define RD(v, n) (D.Read(stream, (void *)&(v), (n), (n)), !stream_failed(stream))

/* A FktID 0x050 report on 06/F0: [src][tgt], then 71 01 0F0B <ecu> <len:2> <inner> (the ECU's reply) or 7F 31 <nrc> (the
   tunnel routine itself refused). 7F 31 78 and an inner 7F xx 78 mean pending: keep waiting. */
static void on_report(u16 fkt, const u8 *p, u32 n)
{
    u32 t = now_us(), m, len, i, k;
    const u8 *q, *in;
    if (!g_waiting || g_reply_ready || fkt != TUNNEL_FKT || n < 2)
        return;
    if (n >= 5 && p[2] == 0x7F && p[3] == 0x31 && p[4] == 0x78) {
        g_pending = 1;
        return;
    }
    if (n >= 4 && p[2] == 0x7F && p[3] == 0x31) {
        g_refused_nrc = n >= 5 ? p[4] : 0;
        __sync_synchronize();
        g_refused = 1;
        return;
    }
    q = p + 2;
    m = n - 2;
    if (m < 7 || q[0] != 0x71 || q[1] != 0x01 || q[2] != (u8)(TUNNEL_ROUTINE >> 8) || q[3] != (u8)TUNNEL_ROUTINE)
        return;
    len = (u32)q[5] << 8 | q[6];
    if (len < 1 || len > m - 7)
        return;
    in = q + 7;
    if (in[0] == 0x7F) { /* negative reply to our service? */
        if (len < 3 || in[1] != g_req[0])
            return;
        if (in[2] == 0x78) {
            g_pending = 1;
            return;
        }
        g_negative_nrc = in[2];
        g_reply_len = 0;
        g_reply_us = t;
        __sync_synchronize();
        g_negative = 1;
        g_reply_ready = 1;
        return;
    }
    if (in[0] != (u8)(g_req[0] + 0x40))
        return; /* a reply to some other request */
    if (g_req[0] == 0x22 && (len < 3 || in[1] != g_req[1] || in[2] != g_req[2]))
        return; /* 22 echoes the DID */
    if (g_req[0] == 0x01 && (len < 2 || in[1] != g_req[1]))
        return; /* 01 echoes the PID */
    k = len < sizeof g_reply ? len : sizeof g_reply;
    for (i = 0; i < k; i++)
        g_reply[i] = in[i];
    g_reply_len = k;
    g_reply_us = t;
    __sync_synchronize();
    g_reply_ready = 1;
}

static void trace_record(u32 kind, u32 transport, u32 flags, u16 target, u16 source, u8 fb, u8 inst, u16 fkt, u8 op,
                         u32 client, const u8 *buf, u32 len)
{
    char hexs[2 * 16 + 1];
    u32 i;
    for (i = 0; i < len && i < 16; i++)
        snprintf(hexs + 2 * i, 3, "%02x", buf[i]);
    hexs[2 * i] = 0;
    for (i = 0; i + 3 <= len; i++) /* a VIN reply (62 F1 90) is never shown */
        if (buf[i] == 0x62 && buf[i + 1] == 0xF1 && buf[i + 2] == 0x90) {
            hexs[0] = 0;
            break;
        }
    OUTF("dsi.rx kind=%u tr=%u flags=0x%x tgt=%04x src=%04x %02x.%02x fkt=%03x op=%x client=%x len=%u p=%s\n", kind,
         transport, flags, target, source, fb, inst, fkt, op, client, len, hexs);
}

static void read_message(void *stream)
{
    u32 transport = 0, kind = 0, flags = 0, client = 0, len = 0;
    u16 target = 0, source = 0, fkt = 0;
    u8 fb = 0, inst = 0, op = 0;
    static u8 buf[MAX_MSG_LEN]; /* static: the DSI thread's stack is small; callbacks are serialized */
    if (!RD(transport, 4) || !RD(kind, 4) || ((kind == 1 || kind == 2) && !RD(flags, 4)) || !RD(target, 2) ||
        !RD(source, 2) || !RD(fb, 1) || !RD(inst, 1) || !RD(fkt, 2) || !RD(op, 1) || !RD(client, 4) || !RD(len, 4)) {
        if (g_trace)
            out("dsi.rx: record unreadable\n");
        return;
    }
    if (len > MAX_MSG_LEN)
        return;
    if (len) {
        D.Read(stream, buf, (int)len, 1);
        if (stream_failed(stream))
            return;
    }
    if (g_trace)
        trace_record(kind, transport, flags, target, source, fb, inst, fkt, op, client, buf, len);
    /* Inbound records are kind 2. Flags 4..9 (transmission status) and 0x13..0x15 (segment skipped) are not reports. */
    if (kind == 2 && ((flags >= 4 && flags <= 9) || (flags >= 0x13 && flags <= 0x15)))
        return;
    if ((kind == 1 || kind == 2) && fb == TUNNEL_FB && inst == TUNNEL_INST)
        on_report(fkt, buf, len);
}

static void read_connect(void *stream)
{
    u32 n, i;
    u16 dev, key;
    if (!RD(n, 4))
        return;
    if (n > 16)
        n = 16;
    for (i = 0; i < n; i++) {
        if (!RD(dev, 2) || !RD(key, 2))
            return;
        if (g_trace)
            OUTF("dsi: informationConnect device 0x%04x key 0x%04x\n", dev, key);
        if (key == TUNNEL_KEY) {
            g_term_dev = dev;
            __sync_synchronize();
            g_term_up = 1;
            return;
        }
    }
}

static void read_disconnect(void *stream)
{
    u32 n, i;
    u16 key;
    if (!RD(n, 4))
        return;
    if (n > 16)
        n = 16;
    for (i = 0; i < n; i++) {
        if (!RD(key, 2))
            return;
        if (key == TUNNEL_KEY) {
            out("dsi: the tunnel target went down\n");
            g_term_up = 0;
            g_disconnected = 1;
            return;
        }
    }
}

static void read_registry(void *stream)
{
    u32 n, i;
    u16 dev, key;
    if (!RD(n, 4) || n > 4096)
        return;
    if (n > 512)
        n = 512;
    for (i = 0; i < n; i++) {
        if (!RD(dev, 2) || !RD(key, 2))
            return;
        if (key == TUNNEL_KEY) {
            g_key_dev = dev;
            g_key_listed = 1;
        } else if (key == HUD_KEY) {
            g_hud_dev = dev;
            g_hud_listed = 1;
        }
    }
    g_registry_seen = 1;
}

static void on_response(void *app, void *stream, const void *event)
{
    u32 type, id;
    (void)app;
    memcpy(&type, (const char *)event + DSI_EVENT_TYPE, 4);
    memcpy(&id, (const char *)event + DSI_EVENT_ID, 4);
    if (type == 0x201 || type == 0x204 || type == 0x205) {
        OUTF("dsi: %s id=0x%08x\n", type == 0x201 ? "response invalid" : "request rejected", id);
        return;
    }
    switch (id) {
    case 0x80000000u: { /* responseMOSTClient */
        u32 client;
        u16 dev;
        if (!RD(client, 4) || !RD(dev, 2))
            return;
        g_client = client;
        g_device = dev;
        __sync_synchronize();
        g_have_client = 1;
        break;
    }
    case 0x80000001u: read_message(stream); break;
    case 0x80000003u: read_connect(stream); break;
    case 0x80000004u: read_disconnect(stream); break;
    case 0xC0000001u: read_registry(stream); break;
    default:
        if (g_trace)
            OUTF("dsi: event 0x%08x ignored\n", id);
    }
}

static void set_cb(unsigned off, void *fn) { memcpy((char *)APP + off, &fn, sizeof fn); }

static void *dsi_thread(void *arg)
{
    int rc;
    (void)arg;
    D.Init();
    rc = D.ClientInit(APP, "DSYSMOSTServer", "MOSTSubSystem", 4, 5);
    if (rc != 0) {
        g_init_rc = rc;
        __sync_synchronize();
        g_thread_done = 1;
        return 0;
    }
    set_cb(DSI_APP_ON_CONNECTED, (void *)on_connected);
    set_cb(DSI_APP_ON_DISCONNECTED, (void *)on_disconnected);
    set_cb(DSI_APP_ON_RESPONSE, (void *)on_response);
    set_cb(DSI_APP_ON_EXIT_LOOP, (void *)on_exit_loop);
    g_running = 1;
    rc = D.ClientRun(APP);
    g_run_rc = rc;
    g_running = 0;
    g_have_client = 0;
    g_term_up = 0;
    lock(); /* wait out any send still using the client */
    D.ClientFree(APP);
    unlock();
    __sync_synchronize();
    g_thread_done = 1;
    return 0;
}

/* ---- the test ------------------------------------------------------------------------------------------------------ */

#define MAX_SAMPLES 2000
static u32 g_rt[MAX_SAMPLES]; /* round trips, us */
static volatile u32 g_alive_us;

static void *watchdog(void *arg) /* a DSI send has no timeout: never hang on the head unit */
{
    (void)arg;
    for (;;) {
        usleep(250000);
        if (now_us() - g_alive_us > 20000000u) {
            out("slrate: watchdog: no progress for 20 s, exiting\n");
            _exit(3);
        }
    }
    return 0;
}

static int wait_for(volatile int *flag, u32 ms)
{
    u32 t0 = now_us();
    while (!*flag) {
        if (g_thread_done || g_disconnected || now_us() - t0 >= ms * 1000u)
            return 0;
        usleep(5000);
    }
    return 1;
}

static void stop_client(void)
{
    u32 t0;
    g_alive_us = now_us();
    if (g_have_client) {
        key_request(0x05, TUNNEL_KEY);
        unregister_client();
    }
    usleep(100000);
    t0 = now_us();
    while (!g_thread_done && now_us() - t0 < 3000000u) {
        if (g_running)
            D.ClientStop(APP, 0);
        usleep(50000);
    }
    if (!g_thread_done)
        out("slrate: the DSI thread did not stop; exiting anyway\n");
}

static int bad_env;
static u32 env_num(const char *name, u32 def, u32 lo, u32 hi)
{
    const char *s = getenv(name);
    u32 v = 0;
    if (!s || !*s)
        return def;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000u) {
            OUTF("slrate: %s must be a number from %u to %u\n", name, lo, hi);
            bad_env = 1;
            return def;
        }
        v = v * 10u + (u32)(*s - '0');
    }
    if (v < lo || v > hi) {
        OUTF("slrate: %s must be a number from %u to %u\n", name, lo, hi);
        bad_env = 1;
        return def;
    }
    return v;
}

static int env_did(u16 *did) /* 1 when SL_DID is set (1 to 4 hex digits) */
{
    const char *s = getenv("SL_DID");
    u32 v = 0, k = 0;
    if (!s || !*s)
        return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (; *s; s++, k++) {
        int h = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (h < 0 || k >= 4) {
            out("slrate: SL_DID must be 1 to 4 hex digits\n");
            bad_env = 1;
            return 0;
        }
        v = v << 4 | (u32)h;
    }
    *did = (u16)v;
    return 1;
}

static void sort_u32(u32 *a, u32 n)
{
    u32 i, j, v;
    for (i = 1; i < n; i++) {
        v = a[i];
        for (j = i; j > 0 && a[j - 1] > v; j--)
            a[j] = a[j - 1];
        a[j] = v;
    }
}

enum { R_OK, R_NEG, R_TIMEOUT, R_REFUSED, R_CLOSED };

__attribute__((constructor)) static void slrate_main(void)
{
    u32 n, gap, tmo, i, k, flen, t0, t, dl, rt, t_start, elapsed;
    u32 cnt_ok = 0, cnt_neg = 0, cnt_tmo = 0, cnt_ref = 0, cnt_pend = 0, sent = 0, streak = 0;
    int quiet, did_mode, res, closed = 0, tid;
    int rpm, rpm_min = -1, rpm_max = -1, rpm_last = -1;
    u16 did = 0;
    u8 req[3], frame[16];
    u32 req_len;
    char a[16], b[16], c[16], d[16], e[16];

    g_alive_us = now_us();
    n = env_num("SL_N", 200, 1, MAX_SAMPLES);
    gap = env_num("SL_GAP", 0, 0, 999); /* usleep() takes less than 1 s */
    tmo = env_num("SL_TIMEOUT", 1000, 100, 5000);
    quiet = (int)env_num("SL_QUIET", 0, 0, 1);
    g_trace = (int)env_num("SL_TRACE", 0, 0, 1);
    did_mode = env_did(&did);
    if (bad_env)
        _exit(2);
    if (did_mode) {
        req[0] = 0x22;
        req[1] = (u8)(did >> 8);
        req[2] = (u8)did;
        req_len = 3;
    } else {
        req[0] = 0x01;
        req[1] = 0x0C;
        req_len = 2;
    }
    /* the tunnel datagram: [src][tgt] 31 01 <routine> <ecu> <len:2> <inner> */
    frame[0] = DIAG_SRC;
    frame[1] = TUNNEL_INST;
    frame[2] = 0x31;
    frame[3] = 0x01;
    frame[4] = (u8)(TUNNEL_ROUTINE >> 8);
    frame[5] = (u8)TUNNEL_ROUTINE;
    frame[6] = DME;
    frame[7] = 0;
    frame[8] = (u8)req_len;
    for (k = 0; k < req_len; k++)
        frame[9 + k] = req[k];
    flen = 9 + req_len;

    if (did_mode)
        OUTF("slrate: %u requests of 22 %04X to the DME over MOST, gap %u ms, timeout %u ms\n", n, did, gap, tmo);
    else
        OUTF("slrate: %u requests of OBD 01 0C (rpm) to the DME over MOST, gap %u ms, timeout %u ms\n", n, gap, tmo);
    if (pthread_create(&tid, 0, watchdog, 0) != 0) {
        out("slrate: could not start the watchdog\n");
        _exit(1);
    }
    if (dsi_bind() != 0)
        _exit(1);
    if (pthread_create(&tid, 0, dsi_thread, 0) != 0) {
        out("slrate: could not start the DSI thread\n");
        _exit(1);
    }
    t0 = now_us();
    if (!wait_for(&g_have_client, 3000)) {
        if (g_init_rc)
            OUTF("slrate: DSIClientInit failed: %d\n", g_init_rc);
        else if (g_thread_done)
            OUTF("slrate: DSIClientRun ended: %d\n", g_run_rc);
        else
            OUTF("slrate: no answer from DSYSMOSTServer (%s)\n", g_connected ? "connected, not registered" : "not connected");
        stop_client();
        _exit(1);
    }
    ms1(a, now_us() - t0);
    OUTF("slrate: MOST client 0x%08x, device 0x%04x (%s ms)\n", g_client, g_device, a);
    t0 = now_us();
    key_request(0x01, TUNNEL_KEY);
    if (!wait_for(&g_term_up, 3000)) {
        if (g_registry_seen && !g_key_listed)
            out("slrate: no tunnel: 06/F0 is not in the MOST registry\n");
        else
            out("slrate: no tunnel: no informationConnect for 06/F0\n");
        stop_client();
        _exit(1);
    }
    ms1(a, now_us() - t0);
    OUTF("slrate: tunnel 06/F0 up at device 0x%04x (%s ms)\n", g_term_dev, a);

    t_start = now_us();
    for (i = 0; i < n; i++) {
        g_alive_us = now_us();
        if (g_disconnected || !g_term_up || g_thread_done) {
            closed = 1;
            break;
        }
        g_reply_ready = g_pending = g_refused = g_negative = 0;
        for (k = 0; k < req_len; k++)
            g_req[k] = req[k];
        g_req_len = req_len;
        __sync_synchronize();
        g_waiting = 1;
        __sync_synchronize();
        t0 = now_us();
        if (send_fn(TUNNEL_FB, TUNNEL_INST, TUNNEL_FKT, 0, frame, flen) != 0) {
            g_waiting = 0;
            closed = 1;
            out("slrate: send failed: the link is gone\n");
            break;
        }
        sent++;
        dl = tmo * 1000u;
        res = R_TIMEOUT;
        for (;;) {
            if (g_reply_ready) {
                res = g_negative ? R_NEG : R_OK;
                break;
            }
            if (g_refused) {
                res = R_REFUSED;
                break;
            }
            if (g_disconnected || !g_term_up || g_thread_done) {
                res = R_CLOSED;
                break;
            }
            t = now_us() - t0;
            if (g_pending) {
                g_pending = 0;
                cnt_pend++;
                dl = t + tmo * 1000u;
                if (dl > 5000000u)
                    dl = 5000000u;
            }
            if (t >= dl)
                break;
            usleep(500);
        }
        g_waiting = 0;
        __sync_synchronize();
        t = now_us() - t_start;
        ms1(a, t);
        if (res == R_OK) {
            rt = g_reply_us - t0;
            g_rt[cnt_ok++] = rt;
            streak = 0;
            rpm = -1;
            if (!did_mode && g_reply_len >= 4)
                rpm = (g_reply[2] << 8 | g_reply[3]) >> 2;
            else if (did_mode && did == 0xF40C && g_reply_len >= 5)
                rpm = (g_reply[3] << 8 | g_reply[4]) >> 2;
            if (rpm >= 0) {
                if (rpm_min < 0 || rpm < rpm_min)
                    rpm_min = rpm;
                if (rpm > rpm_max)
                    rpm_max = rpm;
                rpm_last = rpm;
            }
            if (!quiet) {
                ms1(b, rt);
                if (rpm >= 0) {
                    OUTF("%4u  t %8s ms  rt %6s ms  rpm %d\n", i + 1, a, b, rpm);
                } else {
                    char hx[2 * 8 + 1];
                    u32 m0 = req_len, m;
                    for (m = m0; m < g_reply_len && m < m0 + 8; m++)
                        snprintf(hx + 2 * (m - m0), 3, "%02x", g_reply[m]);
                    hx[2 * (m - m0)] = 0;
                    OUTF("%4u  t %8s ms  rt %6s ms  data %s\n", i + 1, a, b, hx);
                }
            }
        } else if (res == R_NEG) {
            cnt_neg++;
            streak++;
            OUTF("%4u  t %8s ms  negative reply 7F %02X %02X\n", i + 1, a, req[0], g_negative_nrc);
        } else if (res == R_REFUSED) {
            cnt_ref++;
            streak++;
            OUTF("%4u  t %8s ms  the tunnel refused it (7F 31 %02X)\n", i + 1, a, g_refused_nrc);
        } else if (res == R_TIMEOUT) {
            cnt_tmo++;
            streak++;
            OUTF("%4u  t %8s ms  no reply within %u ms\n", i + 1, a, tmo);
            usleep(300000); /* let a late reply pass before the next request */
        } else {
            closed = 1;
            out("slrate: the link went down\n");
            break;
        }
        if (streak >= 5) {
            out("slrate: 5 failures in a row, stopping\n");
            break;
        }
        if (gap)
            usleep(gap * 1000u);
    }
    elapsed = now_us() - t_start;

    OUTF("slrate: done: %u sent, %u replies, %u negative, %u timeouts, %u refused, %u pending notices%s\n", sent,
         cnt_ok, cnt_neg, cnt_tmo, cnt_ref, cnt_pend, closed ? ", link lost" : "");
    if (cnt_ok) {
        u32 sum = 0, p90;
        for (k = 0; k < cnt_ok; k++)
            sum += g_rt[k];
        sort_u32(g_rt, cnt_ok);
        p90 = (cnt_ok * 9u) / 10u;
        if (p90 >= cnt_ok)
            p90 = cnt_ok - 1;
        ms1(a, g_rt[0]);
        ms1(b, g_rt[cnt_ok / 2u]);
        ms1(c, g_rt[p90]);
        ms1(d, g_rt[cnt_ok - 1]);
        ms1(e, udiv(sum, cnt_ok));
        OUTF("slrate: round trip ms: min %s, median %s, p90 %s, max %s, mean %s\n", a, b, c, d, e);
        {
            u32 el_ms = elapsed / 1000u, rate10 = udiv(cnt_ok * 10000u, el_ms ? el_ms : 1);
            ms1(a, elapsed);
            OUTF("slrate: %u.%u replies/s over %s ms\n", rate10 / 10u, rate10 % 10u, a);
        }
        if (rpm_min >= 0)
            OUTF("slrate: rpm min %d, max %d, last %d\n", rpm_min, rpm_max, rpm_last);
    }
    if (g_hud_listed)
        OUTF("slrate: registry: HUDTerminal 82/00 at device 0x%04x\n", g_hud_dev);
    else
        OUTF("slrate: registry: %s\n", g_registry_seen ? "no HUDTerminal 82/00 listed" : "not received");
    stop_client();
    _exit(cnt_ok ? 0 : 1);
}

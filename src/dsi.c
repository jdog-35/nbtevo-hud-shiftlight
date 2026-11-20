/* SPDX-License-Identifier: MIT
 *
 * dsi.c: one DSYSMOSTServer client per process (libsys_dsi_base, opened at run time), for the head-unit tools:
 *   - the UDS tunnel to the DME: RoutineControl 0F0B on FBlock 06, instance F0, FktID 050, one request in flight;
 *   - MOST messages to any device (the HUDTerminal 82/00 at the cluster);
 *   - every received message record to a hook (on the DSI thread), and the MOST registry kept.
 *
 * The client and the tunnel follow ID6 Apps (MIT License, Copyright (c) 2026 ID6 Apps contributors), source/native/src/
 * most/dsi_abi.h, dsi_client.c, most_link.c and most_proto.c, as slrate.c does (run on the car 2026-11-18); see ../NOTICE.
 * Changes: no pthread mutex/cond (a sleeping spin lock and polling, so no QNX struct layouts), a message hook, the
 * registry kept, any destination device.
 */
#include "qnx.h"
#include "dsi.h"

/* ---- the DSI library, as ID6's dsi_abi.h reconstructs it --------------------------------------------------------- */

#define DSI_APP_BYTES 460
#define DSI_APP_ON_CONNECTED 0xCC    /* void (*)(void *app) */
#define DSI_APP_ON_DISCONNECTED 0xD0 /* void (*)(void *app) */
#define DSI_APP_ON_RESPONSE 0xD4     /* void (*)(void *app, void *stream, const void *event) */
#define DSI_APP_ON_EXIT_LOOP 0xDC    /* void (*)(void *app) */
#define DSI_EVENT_TYPE 4             /* u32; 0x201 invalid; 0x204, 0x205 rejected */
#define DSI_EVENT_ID 8               /* u32 */
#define DSI_STREAM_ERROR 0x14        /* int; non-zero after a failed read or write */
#define STREAM_WORDS 64              /* 256 bytes for a stream object (ID6 uses 64) */
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

static char g_why[96];

static int dsi_bind(void)
{
    void *lib, *f;
    unsigned i;
    lib = dlopen("libsys_dsi_base.cpp-ne.so.4", QNX_RTLD_NOW_GLOBAL);
    if (!lib)
        lib = dlopen("/opt/sys/lib/libsys_dsi_base.cpp-ne.so.4", QNX_RTLD_NOW_GLOBAL);
    if (!lib) {
        const char *e = dlerror();
        snprintf(g_why, sizeof g_why, "MOST unavailable: %s", e ? e : "dlopen failed");
        return -1;
    }
    for (i = 0; i < sizeof k_dsi_names / sizeof k_dsi_names[0]; i++) {
        f = dlsym(lib, k_dsi_names[i]);
        if (!f) {
            snprintf(g_why, sizeof g_why, "MOST unavailable: no %s", k_dsi_names[i]);
            return -1;
        }
        memcpy((char *)&D + i * sizeof f, &f, sizeof f);
    }
    return 0;
}

/* ---- client state ------------------------------------------------------------------------------------------------ */

#define TUNNEL_FB 0x06
#define TUNNEL_INST 0xF0
#define TUNNEL_KEY 0x06F0
#define TUNNEL_FKT 0x050
#define TUNNEL_ROUTINE 0x0F0B
#define DIAG_SRC 0x63
#define DME 0x12

static u32 g_app_mem[(DSI_APP_BYTES + 3) / 4 + 16] __attribute__((aligned(8)));
#define APP ((void *)g_app_mem)

static char g_name[32] = "shiftlight";
static dsi_hook_fn g_hook;
static int g_listen_all;
static volatile int g_lock;
static volatile int g_running, g_connected, g_have_client, g_term_up, g_disconnected, g_thread_done, g_closing;
static volatile int g_init_rc, g_run_rc;
static volatile u32 g_client;
static volatile u16 g_device, g_term_dev;
static int g_started, g_tunnel_registered;

#define REG_MAX 512
static volatile u16 g_reg_dev[REG_MAX], g_reg_key[REG_MAX];
static volatile int g_reg_n;

/* the tunnel request in flight: set up by the caller's thread, answered on the DSI thread */
static volatile int g_waiting, g_reply_ready, g_pending, g_refused, g_negative;
static volatile u8 g_refused_nrc, g_negative_nrc;
static u8 g_req[16];
static volatile u32 g_req_len;
static u8 g_reply[64];
static volatile u32 g_reply_len;
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
        /* the client is gone */
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
    sl_lock(&g_lock);
    D.Open(stream);
    w32(stream, g_client);
    w32(stream, 1);
    w16(stream, key);
    rc = finish(stream, id, D.CreateId());
    sl_unlock(&g_lock);
    return rc;
}

static int unregister_client(void) /* request 0x10 */
{
    u32 stream[STREAM_WORDS] __attribute__((aligned(8)));
    int rc;
    if (!g_running || !g_have_client)
        return -1;
    sl_lock(&g_lock);
    D.Open(stream);
    rc = finish(stream, 0x10, D.CreateId());
    sl_unlock(&g_lock);
    return rc;
}

int dsi_send(u16 dst, u8 fb, u8 inst, u16 fkt, u8 op, const u8 *payload, u32 n)
{
    u32 stream[STREAM_WORDS] __attribute__((aligned(8)));
    int rc;
    if (!g_running || !g_have_client || !dst || n > MAX_MSG_LEN)
        return -1;
    sl_lock(&g_lock);
    D.Open(stream);
    w32(stream, g_client);
    w32(stream, 0); /* transport: control channel */
    w32(stream, 1); /* kind: message */
    w32(stream, 0); /* flags */
    w16(stream, dst);
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
    sl_unlock(&g_lock);
    return rc;
}

/* ---- callbacks, on the DSI thread -------------------------------------------------------------------------------- */

static void on_connected(void *app)
{
    static const u32 ids[12] = { 0x80000000u, 0x80000001u, 0x80000002u, 0x80000003u, 0x80000004u, 0x80000005u,
                                 0x80000006u, 0x80000007u, 0xC0000000u, 0xC0000001u, 0xC0000002u, 0xC0000004u };
    static const u32 attrs[4] = { 0xC0000002u, 0xC0000004u, 0xC0000001u, 0xC0000000u };
    u32 stream[STREAM_WORDS] __attribute__((aligned(8)));
    const char *name = g_name;
    u32 session = D.CreateId();
    int i, r;
    (void)app;
    sl_lock(&g_lock);
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
    if (g_listen_all) { /* st1ng's hud.h kSubs beyond the above: "what the HMI's MOST client subscribes to" */
        static const u32 more[8] = { 0xC0000003u, 0x80000001u, 0x80000002u, 0x80000003u,
                                     0x80000004u, 0x80000005u, 0x80000006u, 0x80000007u };
        for (i = 0; i < 8; i++) {
            r = D.ClientSetNotification(APP, more[i]);
            if (r < 0)
                OUTF("dsi: DSIClientSetNotification(0x%08x) failed: %d\n", more[i], r);
        }
    }
    sl_unlock(&g_lock);
    g_connected = 1;
}

static void on_disconnected(void *app)
{
    (void)app;
    if (!g_closing)
        out("dsi: disconnected from DSYSMOSTServer\n");
    g_have_client = 0;
    g_term_up = 0;
    g_disconnected = 1;
}

static void on_exit_loop(void *app) { (void)app; }

#define RD(v, n) (D.Read(stream, (void *)&(v), (n), (n)), !stream_failed(stream))

/* A FktID 050 report on 06/F0: [src][tgt], then 71 01 0F0B <ecu> <len:2> <inner> (the ECU's reply) or 7F 31 <nrc> (the
   tunnel routine itself refused). 7F 31 78 and an inner 7F xx 78 mean pending: keep waiting. */
static void on_report(u32 t, u16 fkt, const u8 *p, u32 n)
{
    u32 m, len, i, k;
    const u8 *q, *in;
    if (!g_waiting || g_reply_ready || fkt != TUNNEL_FKT || n < 2)
        return;
    if (n >= 5 && p[2] == 0x7F && p[3] == 0x31 && p[4] == 0x78) {
        g_pending = 1;
        return;
    }
    if (n >= 4 && p[2] == 0x7F && p[3] == 0x31) {
        g_refused_nrc = n >= 5 ? p[4] : 0;
        SL_BARRIER();
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
    if (in[0] == 0x7F) { /* a negative reply to our service? */
        if (len < 3 || in[1] != g_req[0])
            return;
        if (in[2] == 0x78) {
            g_pending = 1;
            return;
        }
        g_negative_nrc = in[2];
        g_reply_len = 0;
        g_reply_us = t;
        SL_BARRIER();
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
    SL_BARRIER();
    g_reply_ready = 1;
}

static void read_message(void *stream)
{
    u32 t = now_us(), transport = 0, kind = 0, flags = 0, client = 0, len = 0;
    u16 target = 0, source = 0, fkt = 0;
    u8 fb = 0, inst = 0, op = 0;
    static u8 buf[MAX_MSG_LEN]; /* static: the DSI thread's stack is small; callbacks are serialized */
    if (!RD(transport, 4) || !RD(kind, 4) || ((kind == 1 || kind == 2) && !RD(flags, 4)) || !RD(target, 2) ||
        !RD(source, 2) || !RD(fb, 1) || !RD(inst, 1) || !RD(fkt, 2) || !RD(op, 1) || !RD(client, 4) || !RD(len, 4))
        return;
    if (len > MAX_MSG_LEN)
        return;
    if (len) {
        D.Read(stream, buf, (int)len, 1);
        if (stream_failed(stream))
            return;
    }
    if (g_hook) {
        struct dsi_msg m;
        m.t_us = t;
        m.kind = kind;
        m.transport = transport;
        m.flags = flags;
        m.client = client;
        m.len = len;
        m.tgt = target;
        m.src = source;
        m.fkt = fkt;
        m.fb = fb;
        m.inst = inst;
        m.op = op;
        m.p = buf;
        g_hook(&m);
    }
    /* transmission status (flags 4..9) and skipped segments (0x13..0x15) are not reports */
    if (kind == 2 && ((flags >= 4 && flags <= 9) || (flags >= 0x13 && flags <= 0x15)))
        return;
    if ((kind == 1 || kind == 2) && fb == TUNNEL_FB && inst == TUNNEL_INST)
        on_report(t, fkt, buf, len);
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
        if (key == TUNNEL_KEY) {
            g_term_dev = dev;
            SL_BARRIER();
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
            if (!g_closing)
                out("dsi: the tunnel target went down\n");
            g_term_up = 0;
            return;
        }
    }
}

static void reg_put(u16 dev, u16 key)
{
    int i, n = g_reg_n;
    for (i = 0; i < n; i++)
        if (g_reg_key[i] == key) {
            g_reg_dev[i] = dev;
            return;
        }
    if (n < REG_MAX) {
        g_reg_dev[n] = dev;
        g_reg_key[n] = key;
        SL_BARRIER();
        g_reg_n = n + 1;
    }
}

/* Registries can arrive in pieces: pairs are merged, never dropped. */
static void read_registry(void *stream)
{
    u32 n, i;
    u16 dev, key;
    if (!RD(n, 4) || n > 4096)
        return;
    if (n > REG_MAX)
        n = REG_MAX;
    for (i = 0; i < n; i++) {
        if (!RD(dev, 2) || !RD(key, 2))
            return;
        reg_put(dev, key);
    }
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
        SL_BARRIER();
        g_have_client = 1;
        break;
    }
    case 0x80000001u: read_message(stream); break;
    case 0x80000003u: read_connect(stream); break;
    case 0x80000004u: read_disconnect(stream); break;
    case 0xC0000001u: read_registry(stream); break;
    default: break;
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
        SL_BARRIER();
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
    sl_lock(&g_lock); /* wait out any send still using the client */
    D.ClientFree(APP);
    sl_unlock(&g_lock);
    SL_BARRIER();
    g_thread_done = 1;
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

/* ---- the API ----------------------------------------------------------------------------------------------------- */

void dsi_set_hook(dsi_hook_fn h) { g_hook = h; }
void dsi_set_listen_all(int on) { g_listen_all = on; }
const char *dsi_why(void) { return g_why; }
u16 dsi_self(void) { return g_device; }
int dsi_up(void) { return g_have_client && g_running && !g_disconnected && !g_thread_done; }
int dsi_tunnel_up(void) { return g_term_up && dsi_up(); }

int dsi_start(const char *name, u32 wait_ms)
{
    int tid;
    u32 k;
    if (g_started)
        return dsi_up() ? DSI_UP : DSI_NO_SERVER;
    for (k = 0; name[k] && k + 1 < sizeof g_name; k++)
        g_name[k] = name[k];
    g_name[k] = 0;
    if (dsi_bind() != 0)
        return DSI_NO_LIBRARY;
    g_started = 1;
    if (pthread_create(&tid, 0, dsi_thread, 0) != 0) {
        snprintf(g_why, sizeof g_why, "could not start the DSI thread");
        return DSI_NO_SERVER;
    }
    if (!wait_for(&g_have_client, wait_ms)) {
        if (g_init_rc)
            snprintf(g_why, sizeof g_why, "DSIClientInit failed: %d", g_init_rc);
        else if (g_thread_done)
            snprintf(g_why, sizeof g_why, "DSIClientRun ended: %d", g_run_rc);
        else
            snprintf(g_why, sizeof g_why, "no answer from DSYSMOSTServer (%s)",
                     g_connected ? "connected, not registered" : "not connected");
        return DSI_NO_SERVER;
    }
    return DSI_UP;
}

u16 dsi_host_of(u16 key)
{
    int i, n = g_reg_n;
    for (i = 0; i < n; i++)
        if (g_reg_key[i] == key)
            return g_reg_dev[i];
    return 0;
}

int dsi_registry(u16 *dev, u16 *key, int max)
{
    int i, n = g_reg_n;
    if (n > max)
        n = max;
    for (i = 0; i < n; i++) {
        dev[i] = g_reg_dev[i];
        key[i] = g_reg_key[i];
    }
    return n;
}

int dsi_tunnel_open(u32 wait_ms)
{
    if (!dsi_up()) {
        snprintf(g_why, sizeof g_why, "no MOST client");
        return DSI_NO_SERVER;
    }
    if (g_term_up)
        return DSI_UP;
    key_request(0x01, TUNNEL_KEY);
    g_tunnel_registered = 1;
    if (!wait_for(&g_term_up, wait_ms)) {
        snprintf(g_why, sizeof g_why, "%s", g_reg_n && !dsi_host_of(TUNNEL_KEY) ? "06/F0 is not in the MOST registry"
                                                                                : "no informationConnect for 06/F0");
        return DSI_NO_TARGET;
    }
    return DSI_UP;
}

int dsi_tunnel_request(const u8 *uds, u32 n, u8 *reply, u32 cap, u32 *len, u32 timeout_ms, u32 *rt_us, u8 *nrc)
{
    u8 frame[9 + sizeof g_req];
    u32 k, t0, t, dl;
    int res;
    if (len)
        *len = 0;
    if (nrc)
        *nrc = 0;
    if (n < 1 || n > sizeof g_req || !dsi_tunnel_up())
        return DSI_CLOSED;
    /* the tunnel datagram: [src][tgt] 31 01 <routine> <ecu> <len:2> <inner> */
    frame[0] = DIAG_SRC;
    frame[1] = TUNNEL_INST;
    frame[2] = 0x31;
    frame[3] = 0x01;
    frame[4] = (u8)(TUNNEL_ROUTINE >> 8);
    frame[5] = (u8)TUNNEL_ROUTINE;
    frame[6] = DME;
    frame[7] = 0;
    frame[8] = (u8)n;
    for (k = 0; k < n; k++)
        frame[9 + k] = uds[k];
    g_reply_ready = g_pending = g_refused = g_negative = 0;
    for (k = 0; k < n; k++)
        g_req[k] = uds[k];
    g_req_len = n;
    SL_BARRIER();
    g_waiting = 1;
    SL_BARRIER();
    t0 = now_us();
    if (dsi_send(g_term_dev, TUNNEL_FB, TUNNEL_INST, TUNNEL_FKT, 0, frame, 9 + n) != 0) {
        g_waiting = 0;
        return DSI_CLOSED;
    }
    dl = timeout_ms * 1000u;
    res = DSI_TIMEOUT;
    for (;;) {
        if (g_reply_ready) {
            res = g_negative ? DSI_NEGATIVE : DSI_REPLY;
            break;
        }
        if (g_refused) {
            res = DSI_REFUSED;
            break;
        }
        if (!dsi_tunnel_up()) {
            res = DSI_CLOSED;
            break;
        }
        t = now_us() - t0;
        if (g_pending) { /* responsePending: re-arm, never past 5 s */
            g_pending = 0;
            dl = t + timeout_ms * 1000u;
            if (dl > 5000000u)
                dl = 5000000u;
        }
        if (t >= dl)
            break;
        usleep(500);
    }
    g_waiting = 0;
    SL_BARRIER();
    if (rt_us)
        *rt_us = res == DSI_REPLY || res == DSI_NEGATIVE ? g_reply_us - t0 : now_us() - t0;
    if (res == DSI_REPLY) {
        k = g_reply_len < cap ? g_reply_len : cap;
        memcpy(reply, g_reply, k);
        if (len)
            *len = k;
    } else if (nrc) {
        *nrc = res == DSI_NEGATIVE ? g_negative_nrc : res == DSI_REFUSED ? g_refused_nrc : 0;
    }
    if (res == DSI_TIMEOUT)
        usleep(300000); /* let a late reply pass before the next request */
    return res;
}

void dsi_stop(void)
{
    u32 t0;
    if (!g_started)
        return;
    g_closing = 1;
    if (g_have_client) {
        if (g_tunnel_registered)
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
        out("dsi: the DSI thread did not stop; exiting anyway\n");
}

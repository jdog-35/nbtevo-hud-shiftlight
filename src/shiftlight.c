/* SPDX-License-Identifier: MIT
 *
 * libshiftlight.so: the HUD shift light (steps 4 and 5), with its undo and a MOST trace. Runs on hu-omap (NBT Evo ID6,
 * QNX 6.5) as an LD_PRELOAD of /bin/cat: the constructor does the work and _exit()s, so cat itself never runs.
 * One MOST client (dsi.c): engine speed from the vehicle's 05.00 CD0 broadcast (or the DME's OBD 01 0C through the head
 * unit's tunnel while it is missing), and the bars on the HUD (HUDTerminal 82/00 at the cluster). Needs the HMI's library
 * path and DSI address (tools/run.sh sets them):
 *
 *   LD_LIBRARY_PATH=... DSI_IP_ADDRESS=... SL_SHIFT=3000 LD_PRELOAD=/var/opt/shiftlight-dev/libshiftlight.so /bin/cat
 *
 * SL_MODE=stages (default) the three-stage shift light (stages.h): white bars at both ends, then longer white bars, then
 *                        one orange bar across at SL_SHIFT, the only rpm setting. The white stages come SL_TIME1 and
 *                        SL_TIME2 before the shift point at the current climb rate, never more than 1,000 and 500 rpm
 *                        before it; an upshift goes straight to where the revs land (logic.h). Drawn ahead into pictures
 *                        5 and 6: the shift itself is one call.
 *    SL_SHIFT=3000 rpm  SL_PCT1=20  SL_PCT2=35 (% per side)  SL_WHITE=6  SL_SECONDS=120
 *    tuning: SL_TIME1=700 SL_TIME2=350 ms (0 0: fixed at 1,000 and 500 below)  SL_FALL=3000 rpm/s (a shift; 0 off)
 *            SL_RATE=300 ms (climb rate window)  SL_HYST=250 rpm  SL_LEAD=150 ms  SL_MINON=300 ms
 * SL_MODE=bar            the step 4 proof of concept: one bar, shown or hidden as the rpm crosses SL_ON.
 *    SL_ON=3000 rpm  SL_TOGGLE=frame (panel 1 on/off) | buffer (picture 6 on/off) | switch (SwitchVisibility 5 <-> 6)
 * both:  SL_SOURCE=auto (the 05.00 CD0 broadcast, OBD 01 0C while it is missing) | broadcast | obd
 *        SL_COLOUR=auto (SL_DAY=8 by day, SL_NIGHT=4 at night, from 05.00 C31) | a palette index (the orange)
 *        SL_X0=21 SL_X1=95 SL_Y0=0 SL_H=6  SL_VERBOSE=1 (every reading)
 *        They stop after SL_SECONDS, on Enter, or on SIGHUP/INT/TERM/PIPE, then leave the HUD as the BMW navigation does
 *        after a route.
 * SL_MODE=clear          only leaves the HUD as the BMW navigation does after a route (the undo), and exits.
 * SL_MODE=trace          logs every MOST message record the client receives, with times, for SL_SECONDS (default 60);
 *                        SL_RPM=1 (default) polls and logs rpm alongside, SL_TUNNEL=1 also logs the tunnel's own records.
 * SL_ALL=1               subscribe to every message notification (default on; needed for the broadcasts).
 * SL_DETACH=1            a drive run (tools/drive.sh): SIGHUP and SIGPIPE are ignored and stdin is not read, so it goes on
 *                        when the SSH session ends; SIGTERM, SL_SECONDS or a route ends it.
 * SL_FORCE=1             skips the checks for a route on the HUD.
 *
 * Read-only towards the car's computers: it listens to the rpm and day/night broadcasts; the only requests it can send
 * are OBD 01 0C to the DME (fallback) and one property Get of 05.00 C31. The only HUD calls are the ones
 * hud_call_ok() allows. It never draws while a CarPlay route guides (/var/iap/pps/routeguidance exists) or while another
 * client draws on 82/00 (the BMW navigation: drawing calls that are not ours, as st1ng's hud.h detects them); it then
 * stops at once without touching the HUD.
 */
#include "qnx.h"
#include "dsi.h"
#include "hud.h"
#include "logic.h"
#include "stages.h"
#include "gate.h"
#include "slplugin_ids.h"

#define ROUTE_FILE "/var/iap/pps/routeguidance"
#define HUD_KEY 0x8200

static volatile int g_stop, g_stop_sig;
static volatile u32 g_alive_us;
static u32 g_t0;
static int g_force;

/* ---- environment -------------------------------------------------------------------------------------------------- */

static int g_bad_env;

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static const char *env_str(const char *name, const char *def)
{
    const char *s = getenv(name);
    return s && *s ? s : def;
}

static u32 env_num(const char *name, u32 def, u32 lo, u32 hi)
{
    const char *s = getenv(name);
    u32 v = 0;
    if (!s || !*s)
        return def;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000u) {
            OUTF("shiftlight: %s must be a number from %u to %u\n", name, lo, hi);
            g_bad_env = 1;
            return def;
        }
        v = v * 10u + (u32)(*s - '0');
    }
    if (v < lo || v > hi) {
        OUTF("shiftlight: %s must be a number from %u to %u\n", name, lo, hi);
        g_bad_env = 1;
        return def;
    }
    return v;
}

static void tstamp(char *b, u32 t_us) { ms1(b, t_us - g_t0); }

/* ---- stopping: signals, Enter, the watchdog ----------------------------------------------------------------------- */

static void on_signal(int sig)
{
    g_stop_sig = sig;
    g_stop = 1;
}

/* SL_DETACH: the SSH session ending (the cable pulled) must not stop a drive run */
static void on_ignored_signal(int sig) { (void)sig; }

/* Enter stops it. End of input stops it too, unless it came at once (stdin is /dev/null). */
static void *stdin_thread(void *arg)
{
    char c;
    u32 t = now_us();
    long r;
    (void)arg;
    r = read(0, &c, 1);
    if (r > 0 || now_us() - t > 500000u)
        g_stop = 1;
    return 0;
}

/* a DSI send has no timeout: never hang on the head unit */
static void *watchdog(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(250000);
        if (us_since(g_alive_us, now_us()) > 20000000u) {
            out("shiftlight: watchdog: no progress for 20 s, exiting. If a bar stays on the HUD: SL_MODE=clear\n");
            _exit(3);
        }
    }
    return 0;
}

/* ---- the trace ring (filled on the DSI thread, printed on the main thread) --------------------------------------- */

#define TR_N 4096
struct tr_rec {
    u32 t_us, kind, flags, len;
    u16 tgt, src, fkt;
    u8 fb, inst, op, n;
    u8 p[24];
};
static struct tr_rec g_tr[TR_N];
static volatile u32 g_tr_w, g_tr_r, g_tr_lost;
static volatile int g_trace_on, g_trace_tunnel;

static void trace_push(const struct dsi_msg *m)
{
    struct tr_rec *r;
    u32 i, k;
    if (!g_trace_tunnel && m->fb == 0x06 && m->inst == 0xF0)
        return;
    if (g_tr_w - g_tr_r >= TR_N) {
        g_tr_lost++;
        return;
    }
    r = &g_tr[g_tr_w & (TR_N - 1)];
    r->t_us = m->t_us;
    r->kind = m->kind;
    r->flags = m->flags;
    r->len = m->len;
    r->tgt = m->tgt;
    r->src = m->src;
    r->fkt = m->fkt;
    r->fb = m->fb;
    r->inst = m->inst;
    r->op = m->op;
    k = m->len < sizeof r->p ? m->len : sizeof r->p;
    for (i = 0; i < k; i++)
        r->p[i] = m->p[i];
    r->n = (u8)k;
    SL_BARRIER();
    g_tr_w++;
}

/* ---- HUD calls (main thread) and their answers (DSI thread) ------------------------------------------------------- */

static u16 g_hud, g_self;

/* Our calls come back to every client with the head unit's node address as the source (st1ng): a drawing call equal
   (function, length, payload hash) to one of our last 32 within 5 s is ours, not the BMW navigation. */
struct recent {
    u16 fkt;
    u32 n, h, t_us;
};
static struct recent g_recent[32];
static volatile u32 g_recent_n;

static volatile int g_call_wait, g_call_done, g_call_op;
static volatile u16 g_call_fkt;
static volatile u32 g_call_us;
static volatile u32 g_answers, g_errors, g_err_fkt, g_err_op, g_err_n;
static volatile u8 g_err_p[4];
static volatile int g_foreign;
static volatile u32 g_foreign_last_us, g_foreign_calls; /* the last other client's call, how many */
static volatile u16 g_foreign_src, g_foreign_tgt, g_foreign_fkt;
static volatile u8 g_foreign_op;
static volatile u32 g_dn_seen;
static volatile u8 g_foreign_p[8], g_foreign_n;
static u32 g_last_call_us;
static u32 g_gap_us = 20000u; /* between calls: st1ng's 20 ms ("like the navigation"); SL_GAP */
static int g_no_answers;

/* Vehicle data from FBlock 05.00 (device 0x5000 on this car), seen with SL_ALL (found 2026-11-19):
   CD0 Status: engine speed, u16 big-endian / 4, every ~100 ms, ~100 ms ahead of the OBD reading;
   C31 Status: day/night, 01 day, 02 night (follows the headlights; sent on a change, or when asked with a Get). */
#define VEH_KEY 0x0500
static u16 g_veh;
static volatile u32 g_bc_seq, g_bc_us;
static volatile int g_bc_rpm;
static volatile int g_night = -1; /* -1 unknown, 0 day, 1 night */
static volatile u32 g_night_seq;

static void note_sent(const struct hud_call *c)
{
    struct recent *r = &g_recent[g_recent_n & 31u];
    r->fkt = c->fkt;
    r->n = c->n;
    r->h = sl_hash(c->p, c->n);
    r->t_us = now_us() | 1u;
    SL_BARRIER();
    g_recent_n++;
}

static int is_ours(u16 fkt, const u8 *p, u32 n, u32 now)
{
    u32 h = sl_hash(p, n), i;
    for (i = 0; i < 32; i++) {
        const struct recent *r = &g_recent[i];
        if (r->t_us && r->fkt == fkt && r->n == n && r->h == h && now - r->t_us < 5000000u)
            return 1;
    }
    return 0;
}

static void on_msg(const struct dsi_msg *m)
{
    int normal = m->kind == 2 && !(m->flags >= 4 && m->flags <= 9); /* not the send status of a message of ours */
    if (g_trace_on)
        trace_push(m);
    /* day/night as the HMI tells CarPlay (24.70 CD7, 00 day, 01 night) and as the vehicle sends it (05.00 C31) */
    if ((m->fb == 0x24 && m->inst == 0x70 && m->fkt == 0xCD7) || (m->fb == 0x05 && m->fkt == 0xC31))
        g_dn_seen++;
    if (m->fb == 0x05 && m->inst == 0x00 && m->op == 0xC && normal && g_veh && m->src == g_veh) {
        if (m->fkt == 0xCD0 && m->len >= 2) {
            g_bc_rpm = (m->p[0] << 8 | m->p[1]) >> 2;
            g_bc_us = m->t_us;
            SL_BARRIER();
            g_bc_seq++;
        } else if (m->fkt == 0xC31 && m->len >= 1 && (m->p[0] == 1 || m->p[0] == 2)) {
            int n = m->p[0] == 2;
            if (n != g_night) {
                g_night = n;
                SL_BARRIER();
                g_night_seq++;
            }
        }
        return;
    }
    if (m->fb != 0x82 || m->inst != 0x00)
        return;
    if (g_hud && m->src == g_hud && m->tgt == g_self && m->op >= HUD_OP_ERRORACK) { /* an answer to us */
        if (m->op != HUD_OP_RESULTACK && m->op != HUD_OP_ERRORACK && m->op != HUD_OP_ERROR)
            return; /* ProcessingAck and the like: the result follows */
        if (m->op == HUD_OP_RESULTACK) {
            g_answers++;
        } else {
            u32 i;
            g_err_fkt = m->fkt;
            g_err_op = m->op;
            g_err_n = m->len < 4 ? m->len : 4;
            for (i = 0; i < g_err_n; i++)
                g_err_p[i] = m->p[i];
            g_errors++;
        }
        if (g_call_wait && !g_call_done && m->fkt == g_call_fkt) {
            g_call_op = m->op;
            g_call_us = m->t_us;
            SL_BARRIER();
            g_call_done = 1;
        }
        return;
    }
    /* another client's drawing (not ImageGetSize C04: a read-only question; st1ng's CarPlay client asks it at every
       start, without a route) */
    if (m->op <= 8 && m->src != g_self && normal &&
        ((m->fkt >= 0x200 && m->fkt <= 0x22B) || (m->fkt >= 0xC03 && m->fkt <= 0xC09 && m->fkt != 0xC04)) &&
        !is_ours(m->fkt, m->p, m->len, m->t_us)) {
        g_foreign_last_us = m->t_us | 1u;
        g_foreign_calls++;
        if (!g_foreign) {
            u32 i;
            g_foreign_src = m->src;
            g_foreign_tgt = m->tgt;
            g_foreign_fkt = m->fkt;
            g_foreign_op = m->op;
            g_foreign_n = (u8)(m->len < 8 ? m->len : 8);
            for (i = 0; i < g_foreign_n; i++)
                g_foreign_p[i] = m->p[i];
            SL_BARRIER();
            g_foreign = 1;
        }
    }
}

/* One call to the HUDTerminal, at least 20 ms after the last one (like the navigation: one at a time, no bursts).
   wait_ms 0: don't wait (the hook stamps the answer: g_call_done, g_call_us). Returns the answer's op (D: done), 0 when not
   waited, -1 no answer in time, -2 not allowed, -3 not sent. */
static int hud_send(const struct hud_call *c, u32 wait_ms, u32 *sent_us, u32 *lat_us)
{
    u32 t, gap;
    if (!hud_call_ok(c)) {
        char b[128];
        hud_format(c, b);
        OUTF("shiftlight: refused to send %s\n", b);
        return -2;
    }
    t = now_us();
    gap = t - g_last_call_us;
    if (g_last_call_us && gap < g_gap_us)
        usleep(g_gap_us - gap);
    note_sent(c);
    g_call_done = 0;
    g_call_fkt = c->fkt;
    SL_BARRIER();
    g_call_wait = 1;
    t = now_us();
    if (dsi_send(g_hud, 0x82, 0x00, c->fkt, HUD_OP_CALL, c->p, c->n) != 0) {
        g_call_wait = 0;
        return -3;
    }
    g_last_call_us = now_us();
    g_alive_us = g_last_call_us;
    if (sent_us)
        *sent_us = t;
    if (!wait_ms || g_no_answers)
        return 0;
    while (!g_call_done && now_us() - t < wait_ms * 1000u)
        usleep(1000);
    g_call_wait = 0;
    if (!g_call_done)
        return -1;
    if (lat_us)
        *lat_us = g_call_us - t;
    return g_call_op;
}

static u32 g_setup_lat[24];
static u32 g_setup_n;

/* Calls in order, each waiting for its answer (1 s). Without answers: once, then on by time only. */
static int hud_seq(const struct hud_call *c, int n, const char *what)
{
    int i, op;
    u32 lat;
    char b[128];
    for (i = 0; i < n; i++) {
        op = hud_send(&c[i], 1000, 0, &lat);
        if (op == -1) {
            if (!g_no_answers)
                out("shiftlight: no answer from the cluster within 1 s: going on without waiting for answers\n");
            g_no_answers = 1;
            continue;
        }
        if (op < -1)
            return -1;
        if (op == HUD_OP_ERRORACK || op == HUD_OP_ERROR) {
            hud_format(&c[i], b);
            OUTF("shiftlight: %s: the cluster refused %s (op %x, %u bytes: %02x %02x %02x %02x)\n", what, b, op,
                 g_err_n, g_err_p[0], g_err_p[1], g_err_p[2], g_err_p[3]);
            return -1;
        }
        if (op == HUD_OP_RESULTACK && g_setup_n < sizeof g_setup_lat / sizeof g_setup_lat[0])
            g_setup_lat[g_setup_n++] = lat;
    }
    return 0;
}

/* The HUD as the BMW navigation leaves it after a route: panel off first (nothing can flash up), both pictures emptied,
   picture 6 the visible one, the texts emptied, the street text hidden. */
static int clear_hud(void)
{
    struct hud_call s[8], c[10];
    int n = hud_clear_seq(s), k = 0, i;
    c[k++] = s[0]; /* panel off */
    c[k++] = s[1]; /* 5 emptied */
    c[k++] = s[2]; /* 6 emptied */
    hud_vis(&c[k++], HUD_BUF_A, 0);
    hud_vis(&c[k++], HUD_BUF_B, 1);
    for (i = 3; i < n; i++)
        c[k++] = s[i];
    return hud_seq(c, k, "clear");
}

/* A CarPlay route guiding, as st1ng's HUD client decides it: librghook's file says route_state 1 (route active),
   5 (recalculating) or 6 (heading to the route). The file also exists with route_state 0 while a guidance app such as
   Waze is open without a route (car, 2026-11-19). There but unreadable: treated as guiding. */
static int route_active(void)
{
    static const char key[] = "route_state:n:";
    char b[512];
    long n, i, k;
    int fd = open(ROUTE_FILE, QNX_O_RDONLY), rs = -1;
    if (fd < 0)
        return 0;
    n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0)
        return 1;
    b[n] = 0;
    for (i = 0; i + (long)sizeof key - 1 <= n && rs < 0; i++) {
        for (k = 0; key[k] && b[i + k] == key[k]; k++)
            ;
        if (!key[k]) {
            rs = 0;
            for (k = i + (long)sizeof key - 1; b[k] >= '0' && b[k] <= '9' && rs < 1000; k++)
                rs = rs * 10 + (b[k] - '0');
        }
    }
    return rs < 0 || rs == 1 || rs == 5 || rs == 6;
}

static int start_client(int need_hud)
{
    int rc = dsi_start("shiftlight", 3000);
    u32 t;
    char a[16];
    if (rc != DSI_UP) {
        OUTF("shiftlight: %s\n", dsi_why());
        return -1;
    }
    g_self = dsi_self();
    t = now_us();
    while (!(g_hud = dsi_host_of(HUD_KEY)) && need_hud && now_us() - t < 3000000u) {
        g_alive_us = now_us();
        usleep(20000);
    }
    g_veh = dsi_host_of(VEH_KEY);
    tstamp(a, now_us());
    if (g_hud)
        OUTF("shiftlight: %s ms: MOST client device 0x%04x, HUDTerminal 82/00 at device 0x%04x\n", a, g_self, g_hud);
    else
        OUTF("shiftlight: %s ms: MOST client device 0x%04x, no HUDTerminal 82/00 in the registry\n", a, g_self);
    if (need_hud && !g_hud) {
        dsi_stop();
        return -1;
    }
    return 0;
}

/* 2 s quiet on 82/00 from other clients, and no CarPlay route: the HUD is ours to draw on */
static int hud_is_free(const char *what)
{
    u32 t = now_us();
    if (g_force)
        return 1;
    if (route_active()) {
        OUTF("shiftlight: a CarPlay route is guiding: no %s (SL_FORCE=1 overrides)\n", what);
        return 0;
    }
    while (now_us() - t < 2000000u && !g_foreign && !g_stop) {
        g_alive_us = now_us();
        usleep(50000);
    }
    if (g_foreign) {
        OUTF("shiftlight: another client is drawing on the HUD: no %s\n", what);
        return 0;
    }
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

static void print_lat(const char *what, u32 *v, u32 n)
{
    char a[16], b[16], c[16];
    if (!n) {
        OUTF("shiftlight: %s: no answers\n", what);
        return;
    }
    sort_u32(v, n);
    ms1(a, v[0]);
    ms1(b, v[n / 2u]);
    ms1(c, v[n - 1]);
    OUTF("shiftlight: %s: %u answers, min %s ms, median %s ms, max %s ms\n", what, n, a, b, c);
}

/* ---- SL_MODE=bar --------------------------------------------------------------------------------------------------- */

enum { T_FRAME, T_BUFFER, T_SWITCH };
static int g_toggle;

static void toggle_call(struct hud_call *c, int show)
{
    if (g_toggle == T_FRAME)
        hud_vis(c, HUD_FRAME, show);
    else if (g_toggle == T_BUFFER)
        hud_vis(c, HUD_BUF_B, show);
    else if (show)
        hud_switch(c, HUD_BUF_A, HUD_BUF_B);
    else
        hud_switch(c, HUD_BUF_B, HUD_BUF_A);
}

static void print_foreign(void)
{
    char hx[2 * 8 + 1];
    u32 i;
    for (i = 0; i < g_foreign_n && i < 8; i++)
        snprintf(hx + 2 * i, 3, "%02x", g_foreign_p[i]);
    hx[2 * i] = 0;
    OUTF("shiftlight: other client on the HUD: %04x -> %04x fkt %03x op %x data %s\n", g_foreign_src, g_foreign_tgt,
         g_foreign_fkt, g_foreign_op, hx);
}

static u32 g_x0, g_x1, g_y0, g_h;

/* the bar into picture 6 in a colour: emptied, colour, no outline, the rectangle */
static int bar_calls(struct hud_call *c, u8 colour)
{
    int k = 0;
    hud_remove_all(&c[k++], HUD_BUF_B);
    hud_fill(&c[k++], HUD_BUF_B, colour);
    hud_line_width(&c[k++], HUD_BUF_B, 0);
    hud_rect(&c[k++], HUD_BUF_B, 0, (int)g_x0, (int)g_y0, (int)g_x1, (int)(g_y0 + g_h - 1));
    return k;
}

/* Asks the vehicle FBlock for day/night once (a property Get: op 1, no data; a read). The Status answer reaches on_msg. */
static void ask_day_night(void)
{
    if (g_veh)
        dsi_send(g_veh, 0x05, 0x00, 0xC31, 1, 0, 0);
}

enum { S_AUTO, S_BROADCAST, S_OBD };

static int run_bar(void)
{
    const char *tg = env_str("SL_TOGGLE", "frame"), *ss = env_str("SL_SOURCE", "auto");
    const char *cs = env_str("SL_COLOUR", "auto");
    u32 on = env_num("SL_ON", 3000, 1000, 9000), hyst = env_num("SL_HYST", 250, 0, 2000);
    u32 lead = env_num("SL_LEAD", 150, 0, 400), minon = env_num("SL_MINON", 300, 0, 2000);
    u32 secs = env_num("SL_SECONDS", 120, 5, 3600), verbose = env_num("SL_VERBOSE", 0, 0, 1);
    u32 c_day = env_num("SL_DAY", 8, 0, 15), c_night = env_num("SL_NIGHT", 4, 0, 15);
    int c_auto = streq(cs, "auto");
    u32 c_fixed = c_auto ? 8 : env_num("SL_COLOUR", 8, 0, 15), colour;
    int src = streq(ss, "auto") ? S_AUTO : streq(ss, "broadcast") ? S_BROADCAST : streq(ss, "obd") ? S_OBD : -1;
    struct hud_call c[16];
    struct sl_logic L;
    static const u8 req[2] = { 0x01, 0x0C };
    u8 rep[8], nrc;
    u32 len, rt, k, t_start, t_route = 0, n_read = 0, n_fail = 0, n_bc = 0, streak = 0, toggles = 0, t_toggle = 0;
    u32 lat[128], n_lat = 0, rt_sum = 0, seen_bc, seen_night, recolours = 0, t_warn = 0, i;
    int rc, rpm = 0, yielded = 0, lost = 0, waiting_answer = 0;
    const char *why = "";
    char a[16], b[16];

    g_x0 = env_num("SL_X0", 21, 0, 125);
    g_x1 = env_num("SL_X1", 95, 0, 125);
    g_y0 = env_num("SL_Y0", 0, 0, 76);
    g_h = env_num("SL_H", 6, 1, 20);
    dsi_set_listen_all((int)env_num("SL_ALL", 1, 0, 1)); /* the vehicle broadcasts and other clients' HUD calls */
    g_toggle = streq(tg, "frame") ? T_FRAME : streq(tg, "buffer") ? T_BUFFER : streq(tg, "switch") ? T_SWITCH : -1;
    if (g_toggle < 0) {
        out("shiftlight: SL_TOGGLE must be frame, buffer or switch\n");
        g_bad_env = 1;
    }
    if (src < 0) {
        out("shiftlight: SL_SOURCE must be auto, broadcast or obd\n");
        g_bad_env = 1;
    }
    if (g_x1 <= g_x0 || g_y0 + g_h > 77) {
        out("shiftlight: the bar must have SL_X0 < SL_X1 and SL_Y0 + SL_H <= 77\n");
        g_bad_env = 1;
    }
    if (g_bad_env)
        return 2;
    OUTF("shiftlight: bar at %u rpm (off below %u), lead %u ms, min on %u ms, toggle %s, rpm source %s, colour %s, "
         "x %u-%u y %u-%u, %u s\n", on, on - hyst, lead, minon, tg, ss, cs, g_x0, g_x1, g_y0, g_y0 + g_h - 1, secs);
    if (start_client(1) != 0)
        return 1;
    if (src != S_BROADCAST && dsi_tunnel_open(3000) != DSI_UP) {
        OUTF("shiftlight: no tunnel: %s\n", dsi_why());
        if (src == S_OBD) {
            dsi_stop();
            return 1;
        }
    }
    if (!hud_is_free("bar")) {
        if (g_foreign)
            print_foreign();
        dsi_stop();
        return 1;
    }
    if (c_auto) {
        u32 t = now_us();
        ask_day_night();
        while (g_night < 0 && now_us() - t < 1000000u)
            usleep(10000);
        ms1(a, now_us() - t);
        if (g_night < 0)
            OUTF("shiftlight: day/night unknown (no answer from 05.00 C31 in %s ms): day colour until it changes\n", a);
        else
            OUTF("shiftlight: %s (05.00 C31 answered in %s ms)\n", g_night ? "night" : "day", a);
    }
    colour = c_auto ? (g_night == 1 ? c_night : c_day) : c_fixed;

    /* draw once: the HUD to its resting state, the bar into picture 6, then what the toggle needs */
    t_start = now_us();
    if (clear_hud() != 0) {
        dsi_stop();
        return 1;
    }
    k = (u32)bar_calls(c, (u8)colour);
    if (g_toggle == T_BUFFER) {
        hud_vis(&c[k++], HUD_BUF_B, 0);
        hud_vis(&c[k++], HUD_FRAME, 1);
    } else if (g_toggle == T_SWITCH) {
        hud_switch(&c[k++], HUD_BUF_B, HUD_BUF_A); /* the empty picture 5 up */
        hud_vis(&c[k++], HUD_FRAME, 1);
    } /* frame: picture 6 is the visible one already (clear_hud); the panel stays off until shown */
    if (hud_seq(c, (int)k, "set-up") != 0) {
        clear_hud();
        dsi_stop();
        return 1;
    }
    ms1(a, now_us() - t_start);
    OUTF("shiftlight: bar drawn in colour %u and hidden in %s ms (%u calls)\n", colour, a, 9 + k);
    if (src != S_OBD) {
        u32 t = now_us(), seq = g_bc_seq;
        while (g_bc_seq == seq && now_us() - t < 500000u)
            usleep(5000);
        out(g_bc_seq != seq ? "shiftlight: rpm from the 05.00 CD0 broadcast\n"
                            : (src == S_AUTO ? "shiftlight: no 05.00 CD0 broadcast: rpm by OBD polling until it comes\n"
                                             : "shiftlight: no 05.00 CD0 broadcast yet\n"));
    }
    out("shiftlight: running; Enter stops\n");

    sl_logic_init(&L, (int)on, (int)hyst, lead, minon);
    t_start = now_us();
    SL_BARRIER();
    seen_bc = g_bc_seq;
    seen_night = g_night_seq;
    while (!g_stop) {
        u32 now = now_us(), t_ms, t_sample;
        char tag;
        g_alive_us = now;
        if (now - t_start >= secs * 1000000u) {
            why = "time is up";
            break;
        }
        if (g_foreign && !g_force) {
            yielded = 1;
            why = "another client drew on the HUD";
            break;
        }
        if (!g_force && now - t_route >= 500000u) {
            t_route = now;
            if (route_active()) {
                yielded = 1;
                why = "a CarPlay route started";
                break;
            }
        }
        /* day/night changed: the bar again, in the other colour (no waiting: ~80 ms) */
        if (c_auto && g_night_seq != seen_night) {
            u32 want;
            seen_night = g_night_seq;
            want = g_night == 1 ? c_night : c_day;
            if (want != colour) {
                colour = want;
                k = (u32)bar_calls(c, (u8)colour);
                for (i = 0; i < k; i++)
                    hud_send(&c[i], 0, 0, 0);
                waiting_answer = 0;
                g_call_wait = 0;
                recolours++;
                tstamp(a, now_us());
                OUTF("%9s ms  %s: bar colour %u\n", a, g_night ? "night" : "day", colour);
            }
        }
        /* the next reading: the broadcast when it comes, else (auto, or obd) the tunnel */
        if (src != S_OBD && g_bc_seq != seen_bc) {
            seen_bc = g_bc_seq;
            SL_BARRIER();
            rpm = g_bc_rpm;
            t_sample = g_bc_us;
            n_bc++;
            tag = 'b';
            rt = 0;
        } else if (src == S_OBD || (src == S_AUTO && (!g_bc_seq || us_since(g_bc_us, now) > 500000u))) {
            rc = dsi_tunnel_request(req, 2, rep, sizeof rep, &len, 1000, &rt, &nrc);
            if (rc == DSI_CLOSED) {
                lost = 1;
                why = "the MOST tunnel closed";
                break;
            }
            if (rc != DSI_REPLY || len < 4) {
                n_fail++;
                if (++streak >= 5) {
                    why = "5 failed readings in a row";
                    break;
                }
                continue;
            }
            streak = 0;
            n_read++;
            rt_sum += rt / 100u; /* 0.1 ms units: no overflow in an hour */
            rpm = (rep[2] << 8 | rep[3]) >> 2;
            t_sample = now_us();
            tag = 'o';
        } else {
            if (src == S_BROADCAST && now - t_warn > 5000000u && (!g_bc_seq || us_since(g_bc_us, now) > 2000000u)) {
                t_warn = now;
                out("shiftlight: no 05.00 CD0 broadcast for 2 s\n");
            }
            usleep(2000);
            continue;
        }
        now = now_us();
        t_ms = (int)(t_sample - t_start) > 0 ? (t_sample - t_start) / 1000u : 0;
        if (waiting_answer && g_call_done) {
            if (n_lat < sizeof lat / sizeof lat[0])
                lat[n_lat++] = g_call_us - t_toggle;
            ms1(a, g_call_us - t_toggle);
            OUTF("              the cluster answered op %x after %s ms\n", g_call_op, a);
            waiting_answer = 0;
            g_call_wait = 0;
        }
        if (sl_logic_feed(&L, t_ms, rpm)) {
            toggle_call(&c[0], L.shown);
            if (hud_send(&c[0], 0, &t_toggle, 0) == 0) {
                toggles++;
                waiting_answer = !g_no_answers;
            }
            tstamp(a, now);
            OUTF("%9s ms  %s  rpm %d (%c)  pred %d  slope %d rpm/s\n", a, L.shown ? "SHOW" : "hide", rpm, tag, L.pred,
                 L.slope);
        } else if (verbose) {
            tstamp(a, now);
            ms1(b, rt);
            OUTF("%9s ms  rpm %d (%c)  pred %d  rt %s ms%s\n", a, rpm, tag, L.pred, b, L.shown ? "  [shown]" : "");
        }
    }
    if (g_stop)
        why = g_stop_sig ? "a signal" : "Enter";

    if (yielded) {
        OUTF("shiftlight: stepping aside (%s): the HUD is left to it\n", why);
        if (g_foreign)
            print_foreign();
    } else if (lost || !dsi_up()) {
        OUTF("shiftlight: stopped (%s); the MOST link is gone: if the bar stays up, run SL_MODE=clear\n", why);
    } else {
        OUTF("shiftlight: stopping (%s); clearing the HUD\n", why);
        if (clear_hud() == 0)
            out("shiftlight: HUD cleared\n");
    }

    {
        u32 el_ms = (now_us() - t_start) / 1000u, d = el_ms ? el_ms : 1;
        u32 bc10 = sl_udiv(n_bc * 10000u, d), obd10 = sl_udiv(n_read * 10000u, d);
        ms1(a, el_ms * 1000u);
        OUTF("shiftlight: in %s ms: %u broadcast samples (%u.%u/s), %u OBD readings (%u.%u/s, %u failed)", a, n_bc,
             bc10 / 10u, bc10 % 10u, n_read, obd10 / 10u, obd10 % 10u, n_fail);
        if (n_read) {
            ms1(b, sl_udiv(rt_sum, n_read) * 100u);
            OUTF(", OBD mean round trip %s ms", b);
        }
        out("\n");
    }
    OUTF("shiftlight: %u toggles, %u colour changes; cluster answers %u, errors %u%s\n", toggles, recolours, g_answers,
         g_errors, g_no_answers ? " (answers did not reach us)" : "");
    print_lat("set-up calls", g_setup_lat, g_setup_n);
    print_lat("toggle calls", lat, n_lat);
    OUTF("shiftlight: day/night at the end: %s; day/night messages seen: %u\n",
         g_night < 0 ? "unknown" : g_night ? "night" : "day", g_dn_seen);
    dsi_stop();
    return 0;
}

/* ---- SL_MODE=stages (step 5) --------------------------------------------------------------------------------------- */

static int run_stages(void)
{
    const char *ss = env_str("SL_SOURCE", "auto"), *cs = env_str("SL_COLOUR", "auto");
    u32 shift = env_num("SL_SHIFT", 3000, 1500, 9000), hyst = env_num("SL_HYST", 250, 0, 1000);
    u32 time1 = env_num("SL_TIME1", 700, 0, 3000), time2 = env_num("SL_TIME2", 350, 0, 3000);
    u32 fall = env_num("SL_FALL", 3000, 0, 20000), rate_ms = env_num("SL_RATE", 300, 100, 1000);
    u32 lead = env_num("SL_LEAD", 150, 0, 400), minon = env_num("SL_MINON", 300, 0, 2000);
    u32 secs = env_num("SL_SECONDS", 120, 5, 3600), verbose = env_num("SL_VERBOSE", 0, 0, 1);
    u32 c_day = env_num("SL_DAY", 8, 0, 15), c_night = env_num("SL_NIGHT", 4, 0, 15);
    u32 white = env_num("SL_WHITE", 6, 0, 15), pct1 = env_num("SL_PCT1", 20, 1, 49), pct2 = env_num("SL_PCT2", 35, 2, 50);
    u32 t1 = shift - 1000, t2 = shift - 500, t3 = shift; /* the earliest the white stages come, and the shift point */
    int c_auto = streq(cs, "auto");
    u32 c_fixed = c_auto ? 8 : env_num("SL_COLOUR", 8, 0, 15), orange;
    int src = streq(ss, "auto") ? S_AUTO : streq(ss, "broadcast") ? S_BROADCAST : streq(ss, "obd") ? S_OBD : -1;
    struct hud_call c[SL_STAGE_MAX_CALLS];
    struct sl_stage_pics S;
    struct sl_logic L;
    static const u8 req[2] = { 0x01, 0x0C };
    u8 rep[8], nrc;
    u32 len, rt, n, i, t_start, t_route = 0, n_read = 0, n_fail = 0, n_bc = 0, streak = 0, changes = 0, t_change = 0;
    u32 lat[128], n_lat = 0, rt_sum = 0, seen_bc, seen_night, recolours = 0, t_warn = 0, reached[4] = { 0 };
    int rc, rpm = 0, yielded = 0, lost = 0, waiting_answer = 0;
    const char *why = "";
    char a[16], b[16];

    g_x0 = env_num("SL_X0", 21, 0, 125);
    g_x1 = env_num("SL_X1", 95, 0, 125);
    g_y0 = env_num("SL_Y0", 0, 0, 76);
    g_h = env_num("SL_H", 6, 1, 20);
    g_gap_us = env_num("SL_GAP", 20, 0, 100) * 1000u;
    dsi_set_listen_all((int)env_num("SL_ALL", 1, 0, 1)); /* the vehicle broadcasts and other clients' HUD calls */
    if (src < 0) {
        out("shiftlight: SL_SOURCE must be auto, broadcast or obd\n");
        g_bad_env = 1;
    }
    if (time1 && !(time2 && time2 < time1)) {
        out("shiftlight: needs SL_TIME1 > SL_TIME2 > 0 (or SL_TIME1=0)\n");
        g_bad_env = 1;
    }
    if (pct2 <= pct1 || g_x1 <= g_x0 + 10 || g_y0 + g_h > 77) {
        out("shiftlight: needs SL_PCT1 < SL_PCT2, SL_X0 + 10 < SL_X1 and SL_Y0 + SL_H <= 77\n");
        g_bad_env = 1;
    }
    if (g_bad_env)
        return 2;
    OUTF("shiftlight: shift point %u rpm; white stages %u / %u ms before it (no earlier than %u / %u), shift falls from "
         "%u rpm/s, rate over %u ms; off %u below, lead %u ms, min %u ms, rpm source %s, colour %s, x %u-%u y %u-%u, "
         "%u s, gap %u ms\n", t3, time1, time2, t1, t2, fall, rate_ms, hyst, lead, minon, ss, cs, g_x0, g_x1, g_y0,
         g_y0 + g_h - 1, secs, g_gap_us / 1000u);
    if (start_client(1) != 0)
        return 1;
    if (src != S_BROADCAST && dsi_tunnel_open(3000) != DSI_UP) {
        OUTF("shiftlight: no tunnel: %s\n", dsi_why());
        if (src == S_OBD) {
            dsi_stop();
            return 1;
        }
    }
    if (!hud_is_free("shift light")) {
        if (g_foreign)
            print_foreign();
        dsi_stop();
        return 1;
    }
    if (c_auto) {
        u32 t = now_us();
        ask_day_night();
        while (g_night < 0 && now_us() - t < 1000000u)
            usleep(10000);
        ms1(a, now_us() - t);
        if (g_night < 0)
            OUTF("shiftlight: day/night unknown (no answer from 05.00 C31 in %s ms): day colour until it changes\n", a);
        else
            OUTF("shiftlight: %s (05.00 C31 answered in %s ms)\n", g_night ? "night" : "day", a);
    }
    orange = c_auto ? (g_night == 1 ? c_night : c_day) : c_fixed;

    /* draw ahead: the HUD to rest, then the orange bar into 6 and the stage 1 bars into 5, 5 the visible one */
    t_start = now_us();
    if (clear_hud() != 0) {
        dsi_stop();
        return 1;
    }
    sl_stage_init(&S, (int)g_x0, (int)g_x1, (int)g_y0, (int)g_h, (int)pct1, (int)pct2, (u8)white, (u8)orange);
    n = (u32)sl_stage_setup(&S, c);
    if (hud_seq(c, (int)n, "set-up") != 0) {
        clear_hud();
        dsi_stop();
        return 1;
    }
    ms1(a, now_us() - t_start);
    OUTF("shiftlight: stages drawn (white %u, orange %u) and hidden in %s ms (%u calls)\n", white, orange, a, 9 + n);
    if (src != S_OBD) {
        u32 t = now_us(), seq = g_bc_seq;
        while (g_bc_seq == seq && now_us() - t < 500000u)
            usleep(5000);
        out(g_bc_seq != seq ? "shiftlight: rpm from the 05.00 CD0 broadcast\n"
                            : (src == S_AUTO ? "shiftlight: no 05.00 CD0 broadcast: rpm by OBD polling until it comes\n"
                                             : "shiftlight: no 05.00 CD0 broadcast yet\n"));
    }
    out("shiftlight: running; Enter stops\n");

    sl_stages_init(&L, (int)t1, (int)t2, (int)t3, (int)hyst, lead, minon);
    sl_stages_tune(&L, time1, time2, (int)fall);
    L.rate_ms = rate_ms;
    t_start = now_us();
    SL_BARRIER();
    seen_bc = g_bc_seq;
    seen_night = g_night_seq;
    while (!g_stop) {
        u32 now = now_us(), t_ms, t_sample;
        char tag;
        g_alive_us = now;
        if (now - t_start >= secs * 1000000u) {
            why = "time is up";
            break;
        }
        if (g_foreign && !g_force) {
            yielded = 1;
            why = "another client drew on the HUD";
            break;
        }
        if (!g_force && now - t_route >= 500000u) {
            t_route = now;
            if (route_active()) {
                yielded = 1;
                why = "a CarPlay route started";
                break;
            }
        }
        /* day/night changed: the orange bar again in the other colour (no waiting: ~80 ms) */
        if (c_auto && g_night_seq != seen_night) {
            u32 want;
            seen_night = g_night_seq;
            want = g_night == 1 ? c_night : c_day;
            if (want != orange) {
                orange = want;
                n = (u32)sl_stage_recolour(&S, (u8)orange, c);
                for (i = 0; i < n; i++)
                    hud_send(&c[i], 0, 0, 0);
                waiting_answer = 0;
                g_call_wait = 0;
                recolours++;
                tstamp(a, now_us());
                OUTF("%9s ms  %s: orange %u\n", a, g_night ? "night" : "day", orange);
            }
        }
        /* the next reading: the broadcast when it comes, else (auto, or obd) the tunnel */
        if (src != S_OBD && g_bc_seq != seen_bc) {
            seen_bc = g_bc_seq;
            SL_BARRIER();
            rpm = g_bc_rpm;
            t_sample = g_bc_us;
            n_bc++;
            tag = 'b';
            rt = 0;
        } else if (src == S_OBD || (src == S_AUTO && (!g_bc_seq || us_since(g_bc_us, now) > 500000u))) {
            rc = dsi_tunnel_request(req, 2, rep, sizeof rep, &len, 1000, &rt, &nrc);
            if (rc == DSI_CLOSED) {
                lost = 1;
                why = "the MOST tunnel closed";
                break;
            }
            if (rc != DSI_REPLY || len < 4) {
                n_fail++;
                if (++streak >= 5) {
                    why = "5 failed readings in a row";
                    break;
                }
                continue;
            }
            streak = 0;
            n_read++;
            rt_sum += rt / 100u;
            rpm = (rep[2] << 8 | rep[3]) >> 2;
            t_sample = now_us();
            tag = 'o';
        } else {
            if (src == S_BROADCAST && now - t_warn > 5000000u && (!g_bc_seq || us_since(g_bc_us, now) > 2000000u)) {
                t_warn = now;
                out("shiftlight: no 05.00 CD0 broadcast for 2 s\n");
            }
            usleep(2000);
            continue;
        }
        now = now_us();
        t_ms = (int)(t_sample - t_start) > 0 ? (t_sample - t_start) / 1000u : 0;
        if (waiting_answer && g_call_done) {
            if (n_lat < sizeof lat / sizeof lat[0])
                lat[n_lat++] = g_call_us - t_change;
            ms1(a, g_call_us - t_change);
            OUTF("              confirmed after %s ms\n", a);
            waiting_answer = 0;
            g_call_wait = 0;
        }
        if (sl_stages_feed(&L, t_ms, rpm)) {
            n = (u32)sl_stage_to(&S, L.stage, c);
            for (i = 0; i < n; i++)
                hud_send(&c[i], 0, i == 0 ? &t_change : 0, 0);
            waiting_answer = n && !g_no_answers; /* the answer to the last call: the transition is through */
            changes++;
            reached[L.stage & 3]++;
            tstamp(a, now);
            OUTF("%9s ms  stage %d  rpm %d (%c)  pred %d  slope %d rpm/s  rate %d  on %d/%d  (%u call%s)%s\n", a,
                 L.stage, rpm, tag, L.pred, L.slope, L.rate, L.on[1], L.on[2], n, n == 1 ? "" : "s",
                 L.landed == 2 ? "  shift" : L.landed ? "  landed" : "");
        } else if (verbose) {
            tstamp(a, now);
            ms1(b, rt);
            OUTF("%9s ms  rpm %d (%c)  pred %d  rate %d  on %d/%d  rt %s ms  [stage %d]%s\n", a, rpm, tag, L.pred,
                 L.rate, L.on[1], L.on[2], b, L.stage, L.falling == 1 ? "  falling" : "");
        }
    }
    if (g_stop)
        why = g_stop_sig ? "a signal" : "Enter";

    if (yielded) {
        OUTF("shiftlight: stepping aside (%s): the HUD is left to it\n", why);
        if (g_foreign)
            print_foreign();
    } else if (lost || !dsi_up()) {
        OUTF("shiftlight: stopped (%s); the MOST link is gone: if bars stay up, run SL_MODE=clear\n", why);
    } else {
        OUTF("shiftlight: stopping (%s); clearing the HUD\n", why);
        if (clear_hud() == 0)
            out("shiftlight: HUD cleared\n");
    }
    {
        u32 el_ms = (now_us() - t_start) / 1000u, d = el_ms ? el_ms : 1;
        u32 bc10 = sl_udiv(n_bc * 10000u, d), obd10 = sl_udiv(n_read * 10000u, d);
        ms1(a, el_ms * 1000u);
        OUTF("shiftlight: in %s ms: %u broadcast samples (%u.%u/s), %u OBD readings (%u.%u/s, %u failed)", a, n_bc,
             bc10 / 10u, bc10 % 10u, n_read, obd10 / 10u, obd10 % 10u, n_fail);
        if (n_read) {
            ms1(b, sl_udiv(rt_sum, n_read) * 100u);
            OUTF(", OBD mean round trip %s ms", b);
        }
        out("\n");
    }
    OUTF("shiftlight: %u stage changes (to 0: %u, 1: %u, 2: %u, 3: %u), %u colour changes; cluster answers %u, errors "
         "%u%s\n", changes, reached[0], reached[1], reached[2], reached[3], recolours, g_answers, g_errors,
         g_no_answers ? " (answers did not reach us)" : "");
    print_lat("set-up calls", g_setup_lat, g_setup_n);
    print_lat("stage changes (first call sent to last call confirmed)", lat, n_lat);
    if (g_errors)
        OUTF("shiftlight: last error: fkt %03x op %x, %u bytes: %02x %02x %02x %02x\n", g_err_fkt, g_err_op, g_err_n,
             g_err_p[0], g_err_p[1], g_err_p[2], g_err_p[3]);
    OUTF("shiftlight: day/night at the end: %s\n", g_night < 0 ? "unknown" : g_night ? "night" : "day");
    dsi_stop();
    return 0;
}

/* ---- SL_MODE=service (step 6): the shift light for good, started by the Shift Light plugin ------------------------- *
 * One process for as long as "Shift light" is ticked (the file SL_DIR/on), started by libShiftLight.so at every iDrive
 * start and from the menu. It reads the switches every second (SL_DIR: shift-rpm, sport-only, manual-only, car-map) and
 * the plugin's car data (SLP_CAR), and the stages as on the drives (timed, cut at the upshift; logic.h).
 *   a CarPlay route    it steps aside (no HUD calls) and comes back 5 s after the route ends
 *   BMW guidance       (navigation.guidanceStatus 1, from the plugin): the same. The BMW navigation's own HUD calls are
 *                      not seen by this client (car, 2026-11-19: arrows and map on the HUD, no call seen), and after a
 *                      route it clears the HUD, so the pictures are drawn again when it ends
 *   another client     draws on the HUD (the BMW navigation, or a one-off like st1ng's clear at mm-ipod's start): it
 *                      steps aside (no HUD calls, SLP_ASIDE) and comes back once navigation.guidanceStatus is 0 and the
 *                      HUD has been quiet for 5 s; with no guidance status after 60 s quiet, and in any case after 300 s
 *                      quiet (before 2026-11-19 evening it ended and waited for the next iDrive start or tick)
 *   "on" gone, SIGTERM it clears the HUD and ends; the MOST link lost or the set-up refused: it ends (the plugin starts it
 *                      again after 20 s)
 * Broadcast rpm only (05.00 CD0, every ~100 ms on this car): the OBD tunnel is never opened, so BoostDeck and the other
 * apps' MOST fallback keep it to themselves. Rare events (start, end, stepping aside and back) also go to SL_DIR/events.log
 * on flash (at most ~32 KB), so a hiccup can be looked at after the next reboot. */

struct svc_cfg {
    int on, rpm, sport, manual;
    struct sl_car car;
    struct sl_map map;
};
static const char *g_dir;

enum { SV_STOP, SV_OFF, SV_ROUTE, SV_NAV, SV_FOREIGN, SV_LOST, SV_FAIL };

static void svc_log(const char *s)
{
    struct sl_timespec t;
    clock_gettime(QNX_CLOCK_MONOTONIC, &t);
    OUTF("%7ld s  shiftlight: %s\n", t.sec, s);
}

/* "2026-01-31 12:34:56" (UTC) from seconds since 1970, into b[24] (days to a date: Howard Hinnant's civil_from_days) */
static void utc_text(char *b, long secs)
{
    u32 s = secs > 0 ? (u32)secs : 0u, days = sl_udiv(s, 86400u), sod = s - days * 86400u;
    u32 z = days + 719468u, era = sl_udiv(z, 146097u), doe = z - era * 146097u;
    u32 yoe = sl_udiv(doe - sl_udiv(doe, 1460u) + sl_udiv(doe, 36524u) - sl_udiv(doe, 146096u), 365u);
    u32 doy = doe - (365u * yoe + sl_udiv(yoe, 4u) - sl_udiv(yoe, 100u)), mp = sl_udiv(5u * doy + 2u, 153u);
    u32 d = doy - sl_udiv(153u * mp + 2u, 5u) + 1u, m = mp < 10u ? mp + 3u : mp - 9u, y = yoe + era * 400u + (m <= 2u);
    snprintf(b, 24, "%04u-%02u-%02u %02u:%02u:%02u", y, m, d, sl_udiv(sod, 3600u), sl_udiv(sod % 3600u, 60u), sod % 60u);
}

/* a rare event: the RAM log, and SL_DIR/events.log on flash with the head unit's clock (UTC). Past ~30 KB it is renamed
   events.old.log and a new one begins, so the last drives are always there (the plugin empties a file over 32 KB). */
static u32 g_events_bytes = 0xffffffffu; /* unknown until the first event */
static u32 g_redraws;                    /* idle redraws of the pictures */
static void svc_event(const char *s)
{
    struct sl_timespec t;
    char p[200], b[360], w[24];
    int fd, n, flags = QNX_O_WRONLY | QNX_O_CREAT | QNX_O_APPEND;
    svc_log(s);
    snprintf(p, sizeof p, "%s/%s", g_dir, SLP_F_EVENTS);
    if (g_events_bytes == 0xffffffffu) { /* its size now */
        char x[1024];
        long r;
        g_events_bytes = 0;
        fd = open(p, QNX_O_RDONLY);
        if (fd >= 0) {
            while ((r = read(fd, x, sizeof x)) > 0 && g_events_bytes < 64000u)
                g_events_bytes += (u32)r;
            close(fd);
        }
    }
    if (g_events_bytes > 30000u) {
        char old[200];
        snprintf(old, sizeof old, "%s/events.old.log", g_dir);
        if (rename(p, old) != 0)
            flags = QNX_O_WRONLY | QNX_O_CREAT | QNX_O_TRUNC;
        g_events_bytes = 0;
    }
    clock_gettime(QNX_CLOCK_MONOTONIC, &t);
    utc_text(w, time(0));
    n = snprintf(b, sizeof b, "%s UTC %6ld s  worker %d: %s\n", w, t.sec, getpid(), s);
    if (n < 0)
        return;
    if (n > (int)sizeof b - 1)
        n = (int)sizeof b - 1;
    fd = open(p, flags, 0666);
    if (fd < 0)
        return;
    write(fd, b, (unsigned long)n);
    close(fd);
    g_events_bytes += (u32)n;
}

/* the first call of the other client's drawing, for the log */
static void foreign_desc(char *b, int cap)
{
    char hx[2 * 8 + 1];
    u32 i;
    for (i = 0; i < g_foreign_n && i < 8; i++)
        snprintf(hx + 2 * i, 3, "%02x", g_foreign_p[i]);
    hx[2 * i] = 0;
    snprintf(b, (unsigned long)cap, "from %04x, fkt %03x op %x data %s", g_foreign_src, g_foreign_fkt, g_foreign_op, hx);
}

/* the text of a small file, 0-terminated; -1 when there is none */
static int read_small(const char *path, char *b, int cap)
{
    int fd = open(path, QNX_O_RDONLY);
    long n;
    b[0] = 0;
    if (fd < 0)
        return -1;
    n = read(fd, b, (unsigned long)(cap - 1));
    close(fd);
    if (n < 0)
        n = 0;
    b[n] = 0;
    return (int)n;
}

static void svc_read(struct svc_cfg *c)
{
    char p[200], b[512];
    snprintf(p, sizeof p, "%s/%s", g_dir, SLP_F_ON);
    c->on = read_small(p, b, sizeof b) >= 0;
    snprintf(p, sizeof p, "%s/%s", g_dir, SLP_F_SPORT);
    c->sport = read_small(p, b, sizeof b) >= 0;
    snprintf(p, sizeof p, "%s/%s", g_dir, SLP_F_MANUAL);
    c->manual = read_small(p, b, sizeof b) >= 0;
    snprintf(p, sizeof p, "%s/%s", g_dir, SLP_F_RPM);
    c->rpm = read_small(p, b, sizeof b) >= 0 ? sl_text_int(b, SLP_RPM_DEFAULT) : SLP_RPM_DEFAULT;
    if (c->rpm < 2500 || c->rpm > 9000)
        c->rpm = SLP_RPM_DEFAULT;
    snprintf(p, sizeof p, "%s/%s", g_dir, SLP_F_MAP);
    read_small(p, b, sizeof b);
    sl_map_parse(&c->map, b);
    read_small(SLP_CAR, b, sizeof b);
    sl_car_parse(&c->car, b);
}

/* one worker at a time: the pid file names a live other one -> 0 */
static int svc_claim(void)
{
    char b[24];
    int other = read_small(SLP_PID, b, sizeof b) > 0 ? sl_text_int(b, 0) : 0, me = getpid(), fd, n;
    if (other > 0 && other != me && kill(other, 0) == 0)
        return 0;
    fd = open(SLP_PID, QNX_O_WRONLY | QNX_O_CREAT | QNX_O_TRUNC, 0666);
    if (fd >= 0) {
        n = snprintf(b, sizeof b, "%d\n", me);
        write(fd, b, (unsigned long)n);
        close(fd);
    }
    return 1;
}

static void svc_logic(struct sl_logic *L, int rpm)
{
    sl_stages_init(L, rpm - 1000, rpm - 500, rpm, 250, 150, 300); /* timed stages, cut at the upshift: the defaults */
}

/* waits ms while keeping the watchdog fed; 0 when stopped */
static int svc_sleep(u32 ms)
{
    u32 t = now_us();
    while (now_us() - t < ms * 1000u) {
        g_alive_us = now_us();
        if (g_stop)
            return 0;
        usleep(50000);
    }
    return 1;
}

/* Another client drew on the HUD: no HUD calls until it is ours again. Back (1) once the BMW navigation does not guide
   (navigation.guidanceStatus 0) and the HUD has been quiet 5 s; without a guidance status, after 60 s quiet; in any case
   after 300 s quiet. A CarPlay route meanwhile keeps it waiting. 0: switched off or stopped. */
static int svc_aside(struct svc_cfg *cfg)
{
    char b[260], d[120];
    u32 quiet;
    foreign_desc(d, sizeof d);
    snprintf(b, sizeof b, "another client draws on the HUD (%s), guidance %d: stepping aside", d, cfg->car.v[4]);
    svc_event(b);
    {
        int fd = open(SLP_ASIDE, QNX_O_WRONLY | QNX_O_CREAT | QNX_O_TRUNC, 0666);
        if (fd >= 0)
            close(fd);
    }
    for (;;) {
        if (!svc_sleep(500))
            break;
        svc_read(cfg);
        if (!cfg->on)
            break;
        quiet = us_since(g_foreign_last_us, now_us());
        if (route_active())
            continue;
        if ((cfg->car.v[4] == 0 && quiet >= 5000000u) || (cfg->car.v[4] < 0 && quiet >= 60000000u) || quiet >= 300000000u) {
            unlink(SLP_ASIDE);
            g_foreign = 0;
            snprintf(b, sizeof b, "the HUD is free again (guidance %d, quiet %u s, %u calls from other clients so far)",
                     cfg->car.v[4], quiet / 1000000u, g_foreign_calls);
            svc_event(b);
            return 1;
        }
    }
    unlink(SLP_ASIDE);
    return 0;
}

/* Owns the HUD from the set-up until a route, another client, "off", a signal or a lost link. */
static int svc_session(struct svc_cfg *cfg, u8 white, u32 c_day, u32 c_night, u32 *changes)
{
    struct hud_call c[SL_STAGE_MAX_CALLS];
    struct sl_stage_pics S;
    struct sl_logic L;
    u32 n, i, t_poll = 0, t_route = 0, seen_bc, seen_night, t_last = 0, t_ms = 0, orange, t_drawn, t_start, gap_seq = 0;
    int rpm_set = cfg->rpm, gate = 1, was_gate = -1, target, last_rpm = 0, no_rpm = 0;
    /* one line per pull into events.log: from the earliest stage 1 point until it falls well below, or an upshift */
    int pull = 0, pull_max = 0, pull_shown = 0, pull_logic = 0, pull_hidden = 0, was_falling = 0;
    const char *why = "on";
    char b[200];

    orange = g_night == 1 ? c_night : c_day;
    if (clear_hud() != 0)
        return SV_FAIL;
    sl_stage_init(&S, (int)g_x0, (int)g_x1, (int)g_y0, (int)g_h, 20, 35, white, (u8)orange);
    n = (u32)sl_stage_setup(&S, c);
    if (hud_seq(c, (int)n, "set-up") != 0) {
        clear_hud();
        return SV_FAIL;
    }
    svc_logic(&L, rpm_set);
    seen_bc = g_bc_seq;
    seen_night = g_night_seq;
    t_drawn = now_us();
    t_start = t_drawn;
    snprintf(b, sizeof b, "drawn (shift point %d, orange %u)", rpm_set, orange);
    svc_log(b);
    for (;;) {
        u32 now = now_us();
        g_alive_us = now;
        if (g_stop) {
            clear_hud();
            return SV_STOP;
        }
        if (g_foreign) {
            /* st1ng's CarPlay client may draw a moment before its route file says so: a route within 1.5 s is a route */
            u32 t = now_us();
            while (now_us() - t < 1500000u && !route_active() && svc_sleep(100))
                ;
            return route_active() ? SV_ROUTE : SV_FOREIGN; /* no HUD calls: it is someone else's now */
        }
        if (!dsi_up())
            return SV_LOST;
        if (now - t_route >= 500000u) {
            t_route = now;
            if (route_active())
                return SV_ROUTE;
        }
        if (now - t_poll >= 1000000u) {
            t_poll = now;
            svc_read(cfg);
            if (!cfg->on) {
                clear_hud();
                return SV_OFF;
            }
            if (cfg->car.v[4] > 0) { /* BMW guidance: our stage off (one call), then nothing until it ends */
                if (S.stage) {
                    n = (u32)sl_stage_to(&S, 0, c);
                    if (n)
                        hud_send(&c[0], 0, 0, 0); /* the panel off only; the rest would draw into its pictures */
                }
                return SV_NAV;
            }
            if (cfg->rpm != rpm_set) {
                rpm_set = cfg->rpm;
                svc_logic(&L, rpm_set);
                snprintf(b, sizeof b, "shift point %d", rpm_set);
                svc_log(b);
            }
            /* for the log: 5 s without a reading (from the start of this session if none came yet), and "back" at the
               next reading. Until 2026-11-24 a reading that came in just after `now` was read looked 71 minutes old
               here: 231 of the first 252 logged gaps were false ones. The bars were not affected. */
            if (!no_rpm && us_since(g_bc_seq ? g_bc_us : t_start, now) > 5000000u) {
                no_rpm = 1;
                gap_seq = g_bc_seq;
                svc_event("no rpm broadcast for 5 s");
            } else if (no_rpm && g_bc_seq != gap_seq) {
                no_rpm = 0;
                svc_event("the rpm broadcast is back");
            }
            /* Idle, at low revs: the pictures drawn again, unseen (panel off), every 2 minutes. Something can wipe them
               without a call we see (after BMW guidance it is the navigation; on 2026-11-20 the bars did not show until
               "Shift light" was ticked again); a pull starting meanwhile waits behind ~10 calls, ~1 s at most. */
            if (S.stage == 0 && last_rpm < 3000 && (L.rate < 500 && L.rate > -500) && now - t_drawn >= 120000000u) {
                struct hud_call r[SL_STAGE_MAX_CALLS + 1];
                hud_vis(&r[0], HUD_FRAME, 0);
                sl_stage_init(&S, (int)g_x0, (int)g_x1, (int)g_y0, (int)g_h, 20, 35, white, (u8)orange);
                n = (u32)sl_stage_setup(&S, r + 1);
                for (i = 0; i <= n; i++)
                    hud_send(&r[i], 0, 0, 0);
                t_drawn = now_us();
                g_redraws++;
            }
            gate = sl_gate(&cfg->car, &cfg->map, cfg->sport, cfg->manual, &why);
            if (gate != was_gate) {
                snprintf(b, sizeof b, "%s %s (mode %d, gear %d, guidance %d)", gate ? "showing:" : "hidden:", why,
                         cfg->car.v[0], cfg->car.v[2], cfg->car.v[4]);
                svc_event(b);
                was_gate = gate;
            }
        }
        if (g_night_seq != seen_night) { /* day/night: the orange bar again in the other colour */
            u32 want;
            seen_night = g_night_seq;
            want = g_night == 1 ? c_night : c_day;
            if (want != orange) {
                orange = want;
                n = (u32)sl_stage_recolour(&S, (u8)orange, c);
                for (i = 0; i < n; i++)
                    hud_send(&c[i], 0, 0, 0);
                svc_log(g_night ? "night colour" : "day colour");
            }
        }
        target = S.stage;
        if (g_bc_seq != seen_bc) {
            u32 ts;
            int rpm;
            seen_bc = g_bc_seq;
            SL_BARRIER();
            rpm = g_bc_rpm;
            ts = g_bc_us;
            if (t_last)
                t_ms += (ts - t_last) / 1000u; /* a running ms clock: now_us() wraps every 71 minutes */
            t_last = ts;
            sl_stages_feed(&L, t_ms, rpm);
            target = gate ? L.stage : 0;
            last_rpm = rpm;
            if (!pull && rpm >= rpm_set - 1000) {
                pull = 1;
                pull_max = pull_shown = pull_logic = pull_hidden = 0;
            }
            if (pull) {
                if (rpm > pull_max)
                    pull_max = rpm;
                if (L.stage > pull_logic)
                    pull_logic = L.stage;
                if (target > pull_shown)
                    pull_shown = target;
                if (!gate)
                    pull_hidden = 1;
                if (rpm < rpm_set - 1500 || (L.falling == 1 && !was_falling)) {
                    if (pull_max >= rpm_set - 500) { /* near the shift point: worth a line */
                        snprintf(b, sizeof b, "pull to %d rpm: shown stage %d (logic %d)%s, mode %d, %s", pull_max,
                                 pull_shown, pull_logic, pull_hidden ? ", hidden by Only in SPORT" : "",
                                 cfg->car.v[0], L.falling == 1 ? "ended by an upshift" : "revs dropped");
                        svc_event(b);
                    }
                    pull = 0;
                }
            }
            was_falling = L.falling == 1;
        } else if (!gate || us_since(g_bc_us, now) > 1000000u) {
            target = 0; /* hidden, or no rpm for a second */
        }
        if (target != S.stage) {
            n = (u32)sl_stage_to(&S, target, c);
            for (i = 0; i < n; i++)
                hud_send(&c[i], 0, 0, 0);
            (*changes)++;
        }
        usleep(2000);
    }
}

static int run_service(void)
{
    struct svc_cfg cfg;
    u32 white = env_num("SL_WHITE", 6, 0, 15), c_day = env_num("SL_DAY", 8, 0, 15), c_night = env_num("SL_NIGHT", 4, 0, 15);
    u32 changes = 0, t;
    int r, rc = 0;
    char b[200];

    g_dir = env_str("SL_DIR", SLP_DIR);
    g_x0 = 21;
    g_x1 = 95;
    g_y0 = 0;
    g_h = 6;
    if (!svc_claim()) {
        svc_log("service: another worker is running, this one ends");
        return 0;
    }
    svc_read(&cfg);
    snprintf(b, sizeof b, "service start: shift point %d, only sport %d, only manual %d, guidance %d, pwf %d", cfg.rpm,
             cfg.sport, cfg.manual, cfg.car.v[4], cfg.car.v[5]);
    svc_event(b);
    if (!cfg.on) {
        svc_log("the shift light is off: nothing to do");
        unlink(SLP_PID);
        return 0;
    }
    dsi_set_listen_all(1); /* the broadcasts and the other clients' HUD calls */
    if (start_client(1) != 0) {
        unlink(SLP_PID);
        return 10;
    }
    ask_day_night();
    t = now_us();
    while (g_night < 0 && now_us() - t < 1000000u)
        usleep(10000);
    for (;;) {
        /* the HUD must be free: no CarPlay route, no BMW guidance, nobody else drawing for 2 s */
        while (route_active() || cfg.car.v[4] > 0) {
            if (!svc_sleep(1000))
                break;
            svc_read(&cfg);
            if (!cfg.on)
                break;
        }
        if (g_stop || !cfg.on) {
            svc_log(g_stop ? "stopped" : "switched off");
            break;
        }
        if (!hud_is_free("shift light")) {
            if (g_foreign && !svc_aside(&cfg)) {
                svc_log(g_stop ? "stopped" : "switched off");
                break;
            }
            continue; /* back, or a route started meanwhile */
        }
        if (g_stop) {
            svc_log("stopped");
            break;
        }
        r = svc_session(&cfg, (u8)white, c_day, c_night, &changes);
        if (r == SV_ROUTE) {
            svc_event("a CarPlay route is guiding: stepping aside until it ends");
            t = now_us();
            while (!g_stop) { /* 5 s without a route */
                if (route_active())
                    t = now_us();
                else if (now_us() - t >= 5000000u)
                    break;
                if (!svc_sleep(500))
                    break;
            }
            g_foreign = 0; /* the route's own drawing; the 2 s quiet check comes next */
            svc_event("the route ended");
            continue;
        }
        if (r == SV_NAV) {
            svc_event("the BMW navigation is guiding: stepping aside until it ends");
            t = now_us();
            while (!g_stop) { /* 5 s without guidance (and without a CarPlay route) */
                svc_read(&cfg);
                if (!cfg.on)
                    break;
                if (cfg.car.v[4] > 0 || route_active())
                    t = now_us();
                else if (now_us() - t >= 5000000u)
                    break;
                if (!svc_sleep(500))
                    break;
            }
            g_foreign = 0;
            svc_event("the BMW guidance ended: drawing again");
            continue;
        }
        if (r == SV_FOREIGN) {
            if (svc_aside(&cfg))
                continue;
            svc_log(g_stop ? "stopped" : "switched off");
            break;
        }
        svc_event(r == SV_OFF ? "switched off: HUD cleared" : r == SV_STOP ? "stopped: HUD cleared"
                  : r == SV_LOST ? "the MOST link is gone" : "the cluster refused the set-up");
        rc = r == SV_LOST ? 11 : r == SV_FAIL ? 12 : 0;
        break;
    }
    snprintf(b, sizeof b, "service end (%d), %u stage changes, %u idle redraws, cluster answers %u, errors %u", rc, changes,
             g_redraws, g_answers, g_errors);
    svc_event(b);
    dsi_stop();
    unlink(SLP_PID);
    return rc;
}

/* ---- SL_MODE=clear ------------------------------------------------------------------------------------------------- */

static int run_clear(void)
{
    if (g_bad_env)
        return 2;
    dsi_set_listen_all((int)env_num("SL_ALL", 1, 0, 1)); /* to see another client drawing */
    if (start_client(1) != 0)
        return 1;
    if (!hud_is_free("clear")) {
        if (g_foreign)
            print_foreign();
        dsi_stop();
        return 1;
    }
    if (clear_hud() == 0)
        out("shiftlight: HUD cleared (the end-of-route state)\n");
    print_lat("calls", g_setup_lat, g_setup_n);
    dsi_stop();
    return 0;
}

/* ---- SL_MODE=trace ------------------------------------------------------------------------------------------------- */

struct key_count {
    u16 src, tgt, fkt;
    u8 fb, inst, op;
    u32 n;
};
static struct key_count g_keys[256];
static u32 g_nkeys, g_keys_full;

static void count_key(const struct tr_rec *r)
{
    u32 i;
    for (i = 0; i < g_nkeys; i++) {
        struct key_count *k = &g_keys[i];
        if (k->src == r->src && k->tgt == r->tgt && k->fkt == r->fkt && k->fb == r->fb && k->inst == r->inst &&
            k->op == r->op) {
            k->n++;
            return;
        }
    }
    if (g_nkeys < 256) {
        struct key_count *k = &g_keys[g_nkeys++];
        k->src = r->src;
        k->tgt = r->tgt;
        k->fkt = r->fkt;
        k->fb = r->fb;
        k->inst = r->inst;
        k->op = r->op;
        k->n = 1;
    } else {
        g_keys_full++;
    }
}

static void trace_drain(void)
{
    char a[16], hx[2 * 24 + 2];
    while (g_tr_r != g_tr_w) {
        const struct tr_rec *r = &g_tr[g_tr_r & (TR_N - 1)];
        u32 i;
        for (i = 0; i < r->n; i++)
            snprintf(hx + 2 * i, 3, "%02x", r->p[i]);
        hx[2 * i] = r->len > r->n ? '+' : 0;
        hx[2 * i + (r->len > r->n ? 1 : 0)] = 0;
        tstamp(a, r->t_us);
        OUTF("%9s  k%u f%x  %04x>%04x  %02x.%02x  %03x op %x  n%u  %s%s\n", a, r->kind, r->flags, r->src, r->tgt, r->fb,
             r->inst, r->fkt, r->op, r->len, hx,
             (r->fb == 0x24 && r->inst == 0x70 && r->fkt == 0xCD7) || (r->fb == 0x05 && r->fkt == 0xC31)
                 ? "  <- day/night?"
                 : "");
        count_key(r);
        SL_BARRIER();
        g_tr_r++;
    }
}

static int run_trace(void)
{
    u32 secs = env_num("SL_SECONDS", 60, 5, 3600), rpm_on = env_num("SL_RPM", 1, 0, 1);
    static const u8 req[2] = { 0x01, 0x0C };
    u8 rep[8], nrc;
    u32 len, rt, t_start, n_reg = 0, i, best, bi, shown;
    u16 dev[512], key[512];
    int rc, reg_printed = 0;
    char a[16];
    g_trace_tunnel = (int)env_num("SL_TUNNEL", 0, 0, 1);
    dsi_set_listen_all((int)env_num("SL_ALL", 1, 0, 1));
    if (g_bad_env)
        return 2;
    OUTF("shiftlight: trace for %u s%s\n", secs, rpm_on ? ", with rpm" : "");
    g_trace_on = 1;
    if (start_client(0) != 0)
        return 1;
    if (rpm_on && dsi_tunnel_open(3000) != DSI_UP) {
        OUTF("shiftlight: no tunnel (%s): tracing without rpm\n", dsi_why());
        rpm_on = 0;
    }
    t_start = now_us();
    while (!g_stop && now_us() - t_start < secs * 1000000u && dsi_up()) {
        g_alive_us = now_us();
        if (!reg_printed && now_us() - t_start > 1000000u) {
            reg_printed = 1;
            n_reg = (u32)dsi_registry(dev, key, 512);
            OUTF("shiftlight: registry, %u entries (device:fblock.inst):", n_reg);
            for (i = 0; i < n_reg; i++)
                OUTF("%s %04x:%02x.%02x", i % 8 ? "" : "\n ", dev[i], key[i] >> 8, key[i] & 0xff);
            out("\n");
        }
        if (rpm_on) {
            rc = dsi_tunnel_request(req, 2, rep, sizeof rep, &len, 1000, &rt, &nrc);
            if (rc == DSI_REPLY && len >= 4) {
                tstamp(a, now_us());
                OUTF("%9s  rpm %d\n", a, (rep[2] << 8 | rep[3]) >> 2);
            } else if (rc == DSI_CLOSED) {
                out("shiftlight: the tunnel closed: tracing without rpm\n");
                rpm_on = 0;
            }
        } else {
            usleep(50000);
        }
        trace_drain();
    }
    usleep(200000);
    trace_drain();
    OUTF("shiftlight: trace done: %u records kept, %u lost (ring full)\n", g_tr_w, g_tr_lost);
    out("shiftlight: most frequent (count  src>tgt  fblock.inst  fkt op):\n");
    for (shown = 0; shown < 40; shown++) { /* the 40 most frequent, by selection */
        best = 0;
        bi = g_nkeys;
        for (i = 0; i < g_nkeys; i++)
            if (g_keys[i].n > best) {
                best = g_keys[i].n;
                bi = i;
            }
        if (bi == g_nkeys)
            break;
        OUTF("  %6u  %04x>%04x  %02x.%02x  %03x op %x\n", g_keys[bi].n, g_keys[bi].src, g_keys[bi].tgt, g_keys[bi].fb,
             g_keys[bi].inst, g_keys[bi].fkt, g_keys[bi].op);
        g_keys[bi].n = 0;
    }
    if (g_keys_full)
        OUTF("  (%u records of further kinds not counted)\n", g_keys_full);
    dsi_stop();
    return 0;
}

/* ---- entry --------------------------------------------------------------------------------------------------------- */

__attribute__((constructor)) static void shiftlight_main(void)
{
    const char *mode = env_str("SL_MODE", "stages");
    int tid, rc, detach = (int)env_num("SL_DETACH", 0, 0, 1) || streq(mode, "service");
    g_t0 = now_us();
    g_alive_us = g_t0;
    /* Detached (a drive run, output to a file): the SSH session ending must not stop it; only SIGINT/SIGTERM (or time,
       or a route) do. Attached: the session ending, a closed output or Enter stop it, and it clears the HUD. */
    signal(QNX_SIGHUP, detach ? on_ignored_signal : on_signal);
    signal(QNX_SIGPIPE, detach ? on_ignored_signal : on_signal);
    signal(QNX_SIGINT, on_signal);
    signal(QNX_SIGTERM, on_signal);
    g_force = (int)env_num("SL_FORCE", 0, 0, 1);
    if (pthread_create(&tid, 0, watchdog, 0) != 0 || (!detach && pthread_create(&tid, 0, stdin_thread, 0) != 0)) {
        out("shiftlight: could not start a thread\n");
        _exit(1);
    }
    if (detach && !streq(mode, "service"))
        out("shiftlight: detached: SSH ending does not stop this run; SIGTERM, time or a route does\n");
    dsi_set_hook(on_msg);
    if (streq(mode, "stages"))
        rc = run_stages();
    else if (streq(mode, "bar"))
        rc = run_bar();
    else if (streq(mode, "clear"))
        rc = run_clear();
    else if (streq(mode, "trace"))
        rc = run_trace();
    else if (streq(mode, "service"))
        rc = run_service();
    else {
        out("shiftlight: SL_MODE must be stages, bar, clear, trace or service\n");
        rc = 2;
    }
    _exit(rc);
}

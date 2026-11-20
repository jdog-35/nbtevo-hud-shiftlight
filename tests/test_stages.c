/* SPDX-License-Identifier: MIT */
/* Host tests (Mac) for step 5: the stage renderer against a model of the HUDTerminal, and the stage logic on a simulated
 * pull and on the car's 05.00 CD0 broadcast log (2026-11-19).
 *   cc -Isrc tests/test_stages.c src/stages.c src/hud.c src/logic.c src/util.c -o out/test_stages
 *   out/test_stages tests/data/cd0-broadcast.txt tests/data/drive-2026-11-19.txt */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stages.h"
#include "logic.h"

static int fails, checks;
#define CHECK(c, ...)                                     \
    do {                                                  \
        checks++;                                         \
        if (!(c)) {                                       \
            fails++;                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

/* ---- a model of the HUDTerminal's panel and two pictures, as the car showed them (2026-11-19) ---------------------
 * Shapes are kept in send order, each drawn over the ones before (a shape with an item number already used is added,
 * not replaced). Black (7) covers what is under it and gives no light. An 8-point shape from hud_two_bars is two bars. */

#define BLACK 7
#define MAX_ITEMS 64
struct span {
    int x0, x1;
};
struct item {
    int y0, y1, colour, nspans;
    struct span sp[2];
};
struct hud_model {
    int vis[8]; /* 1 = the panel */
    int fill[8];
    struct item it[8][MAX_ITEMS];
    int n[8];
    int visible_ops; /* calls that changed what can be seen, since the last reset of the counter */
    int resets_seen; /* RemoveAll on a picture that could be seen */
};

static int be(const u8 *p) { return (short)(p[0] << 8 | p[1]); }

static void model_rest(struct hud_model *m)
{
    memset(m, 0, sizeof *m);
    m->vis[6] = 1; /* the BMW navigation leaves picture 6 visible, the panel off */
    m->fill[5] = m->fill[6] = -1;
}

static int seen(const struct hud_model *m, int p) { return m->vis[1] && m->vis[p]; }

static void model_apply(struct hud_model *m, const struct hud_call *c)
{
    int h = be(c->p);
    CHECK(hud_call_ok(c), "a call hud_call_ok() refuses: fkt %03x", c->fkt);
    switch (c->fkt) {
    case HUD_SET_VISIBILITY:
        if (m->vis[h] != c->p[2] && (h == 1 || m->vis[1]))
            m->visible_ops++;
        m->vis[h] = c->p[2];
        break;
    case HUD_SWITCH_VISIBILITY:
        if (m->vis[1])
            m->visible_ops++;
        m->vis[h] = 0;
        m->vis[be(c->p + 2)] = 1;
        break;
    case HUD_REMOVE_ALL:
        if (seen(m, h)) {
            m->visible_ops++;
            m->resets_seen++;
        }
        m->n[h] = 0;
        break;
    case HUD_SET_FILL_COLOR: m->fill[h] = c->p[2]; break;
    case HUD_ADD_POLYGON: {
        struct item *it;
        int count = c->p[5];
        CHECK(m->fill[h] >= 0, "a polygon on %d before any fill colour", h);
        CHECK(m->n[h] < MAX_ITEMS, "picture %d holds %d shapes", h, m->n[h]);
        if (m->n[h] >= MAX_ITEMS)
            return;
        it = &m->it[h][m->n[h]++];
        it->colour = m->fill[h];
        it->y0 = be(c->p + 8);
        it->y1 = be(c->p + 16); /* point 3 (x1, y1) */
        if (count == 4) {
            it->nspans = 1;
            it->sp[0].x0 = be(c->p + 6);
            it->sp[0].x1 = be(c->p + 10);
        } else { /* hud_two_bars: (x0,y0) (x1,y0) (x1,y1) (rx0,y1) (rx0,y0) (lx1,y0) (lx1,y1) (x0,y1) */
            it->nspans = 2;
            it->sp[0].x0 = be(c->p + 6);
            it->sp[0].x1 = be(c->p + 26);
            it->sp[1].x0 = be(c->p + 18);
            it->sp[1].x1 = be(c->p + 10);
        }
        if (seen(m, h))
            m->visible_ops++;
        break;
    }
    default: break;
    }
}

/* what the HUD shows at (x, y): a colour index, or -1 (nothing, or black) */
static int model_at(const struct hud_model *m, int x, int y)
{
    int p, i, j, got = -1;
    if (!m->vis[1])
        return -1;
    for (p = 5; p <= 6; p++) {
        if (!m->vis[p])
            continue;
        for (i = 0; i < m->n[p]; i++) { /* in send order: later shapes cover earlier ones */
            const struct item *it = &m->it[p][i];
            if (y < it->y0 || y > it->y1)
                continue;
            for (j = 0; j < it->nspans; j++)
                if (x >= it->sp[j].x0 && x <= it->sp[j].x1)
                    got = it->colour;
        }
    }
    return got == BLACK ? -1 : got;
}

/* the picture stage t should give, everywhere in the picture */
static int check_shows(const struct hud_model *m, const struct sl_stage_pics *s, int t, const char *when)
{
    int x, y, bad = 0;
    for (y = 0; y < 77; y++)
        for (x = 0; x < 126; x++) {
            int in_rows = y >= s->y0 && y <= s->y0 + s->h - 1, want = -1, got = model_at(m, x, y);
            if (in_rows && x >= s->x0 && x <= s->x1) {
                if (t == 1 && (x <= s->s1_lx1 || x >= s->s1_rx0))
                    want = s->white;
                if (t == 2 && (x <= s->s2_lx1 || x >= s->s2_rx0))
                    want = s->white;
                if (t == 3)
                    want = s->orange;
            }
            if (got != want && !bad++)
                CHECK(0, "%s: stage %d shows colour %d at (%d,%d), want %d", when, t, got, x, y, want);
        }
    if (m->vis[1])
        CHECK(m->vis[5] + m->vis[6] == 1, "%s: pictures 5 and 6 both %s", when, m->vis[5] ? "visible" : "hidden");
    return !bad;
}

static int apply_transition(struct hud_model *m, struct sl_stage_pics *s, int t, int *ncalls)
{
    struct hud_call c[SL_STAGE_MAX_CALLS];
    int n = sl_stage_to(s, t, c), k;
    CHECK(n <= SL_STAGE_MAX_CALLS, "-> %d: %d calls", t, n);
    m->visible_ops = 0;
    for (k = 0; k < n; k++)
        model_apply(m, &c[k]);
    *ncalls = n;
    return m->visible_ops;
}

static void test_renderer(void)
{
    struct sl_stage_pics s;
    struct hud_model m;
    struct hud_call c[SL_STAGE_MAX_CALLS];
    int n, i, step, t, k, prev, vis_ops, resets_before;
    int n01 = -1, n12 = -1, n23 = -1, n32 = -1, n21 = -1, n12b = -1, max_items = 0, stage_resets = 0;
    char when[80];

    sl_stage_init(&s, 21, 95, 0, 6, 20, 35, 6, 8);
    CHECK(s.s1_lx1 == 35 && s.s1_rx0 == 81 && s.s2_lx1 == 46 && s.s2_rx0 == 70,
          "bar ends %d %d %d %d, want 35 81 46 70 (the previews of 2026-11-18)", s.s1_lx1, s.s1_rx0, s.s2_lx1, s.s2_rx0);
    model_rest(&m);
    n = sl_stage_setup(&s, c);
    CHECK(n <= SL_STAGE_MAX_CALLS, "set-up has %d calls", n);
    for (i = 0; i < n; i++)
        model_apply(&m, &c[i]);
    check_shows(&m, &s, 0, "after set-up");
    CHECK(m.vis[5] && !m.vis[6] && !m.vis[1], "after set-up: 5 visible, 6 hidden, panel off");

    /* the usual pull: 0 1 2 3, then down after the shift, and back and forth */
    {
        static const int seq[] = { 1, 2, 3, 2, 1, 2, 3, 1, 0, 1, 2, 3, 0, 2, 0, 3, 1, 2, 1, 0 };
        prev = 0;
        for (i = 0; i < (int)(sizeof seq / sizeof seq[0]); i++) {
            t = seq[i];
            vis_ops = apply_transition(&m, &s, t, &n);
            snprintf(when, sizeof when, "sequence step %d (%d -> %d)", i, prev, t);
            check_shows(&m, &s, t, when);
            CHECK(vis_ops == 1, "%s: %d visible changes, want 1", when, vis_ops);
            if (prev == 0 && t == 1 && n01 < 0)
                n01 = n;
            if (prev == 1 && t == 2 && n12 < 0)
                n12 = n;
            else if (prev == 1 && t == 2 && n12b < 0)
                n12b = n;
            if (prev == 2 && t == 3 && n23 < 0)
                n23 = n;
            if (prev == 3 && t == 2 && n32 < 0)
                n32 = n;
            if (prev == 2 && t == 1 && n21 < 0)
                n21 = n;
            prev = t;
        }
    }
    printf("calls per transition: off->1 %d, 1->2 %d (after a mask %d), 2->3 %d, 3->2 %d, 2->1 %d\n", n01, n12, n12b,
           n23, n32, n21);
    CHECK(n01 == 1, "off -> 1 takes %d calls, want 1 (the panel)", n01);
    CHECK(n12 == 1, "1 -> 2 takes %d calls, want 1 (the stage 2 shape)", n12);
    CHECK(n12b == 2, "1 -> 2 after a mask takes %d calls, want 2 (white again, the shape)", n12b);
    CHECK(n23 == 1, "2 -> 3 takes %d calls, want 1 (SwitchVisibility)", n23);
    CHECK(n32 == 1, "3 -> 2 takes %d calls, want 1", n32);
    CHECK(n21 == 2, "2 -> 1 takes %d calls, want 2 (black, the mask)", n21);

    /* every transition from every history, with day/night recolours in between */
    srand(5);
    prev = s.stage;
    resets_before = m.resets_seen;
    for (step = 0; step < 20000; step++) {
        if (rand() % 10 == 0) {
            u8 col = (u8)(rand() & 1 ? 8 : 4);
            n = sl_stage_recolour(&s, col, c);
            for (k = 0; k < n; k++)
                model_apply(&m, &c[k]);
            snprintf(when, sizeof when, "recolour at step %d", step);
            if (!check_shows(&m, &s, s.stage, when))
                break;
        }
        t = rand() % 4;
        resets_before = m.resets_seen; /* recolours redraw picture 6 on purpose: not counted against the change */
        vis_ops = apply_transition(&m, &s, t, &n);
        snprintf(when, sizeof when, "random step %d (%d -> %d)", step, prev, t);
        if (!check_shows(&m, &s, t, when))
            break;
        if (t != prev && m.resets_seen == resets_before)
            CHECK(vis_ops == 1, "%s: %d visible changes, want 1", when, vis_ops);
        if (m.resets_seen != resets_before)
            stage_resets++;
        if (m.n[5] > max_items)
            max_items = m.n[5];
        prev = t;
    }
    printf("random walk: picture 5 held at most %d shapes; %d of 20000 stage changes needed a visible redraw\n",
           max_items, stage_resets);
    CHECK(max_items <= SL_P5_MAX_ITEMS + 1, "picture 5 grew to %d shapes", max_items);
    CHECK(stage_resets == 0, "%d stage changes redrew a visible picture", stage_resets);

    /* back and forth between 1 and 2 without ever going off: picture 5 is redrawn once it is full, and still right */
    model_rest(&m);
    sl_stage_init(&s, 21, 95, 0, 6, 20, 35, 6, 8);
    n = sl_stage_setup(&s, c);
    for (i = 0; i < n; i++)
        model_apply(&m, &c[i]);
    for (step = 0; step < 200; step++) {
        t = step % 2 ? 2 : 1;
        apply_transition(&m, &s, t, &n);
        snprintf(when, sizeof when, "1 <-> 2 step %d", step);
        if (!check_shows(&m, &s, t, when))
            break;
    }
    CHECK(m.n[5] <= SL_P5_MAX_ITEMS + 1, "1 <-> 2: picture 5 holds %d shapes", m.n[5]);
}

/* ---- the stage logic ---------------------------------------------------------------------------------------------- */

/* a pull at rate rpm/s from `from`, delivered like the broadcast (fresh every 100 ms, ~30 ms late) or like OBD (DME refresh
   200 ms, a reading every 70 ms, 35 ms late). Records when each stage came up, ms after the start. time1/time2: the
   timed white stages (0 0: fixed at th[1] and th[2]). */
static void pull_timed(int broadcast, double from, double rate, const int th[4], u32 time1, u32 time2, double up_ms[4],
                       double phase)
{
    struct sl_logic l;
    double t;
    int s;
    sl_stages_init(&l, th[1], th[2], th[3], 250, 150, 300);
    sl_stages_tune(&l, time1, time2, 3000);
    for (s = 0; s < 4; s++)
        up_ms[s] = -1;
    for (t = phase; t < 6000; t += broadcast ? 100 : 70) {
        double sample = broadcast ? t - 30 : t - 35;
        double held = broadcast ? sample : 200.0 * (int)(sample / 200.0);
        int v = (int)(from + rate * (held > 0 ? held : 0) / 1000.0);
        if (sl_stages_feed(&l, (u32)t, v))
            for (s = 1; s <= l.stage; s++)
                if (up_ms[s] < 0)
                    up_ms[s] = t;
    }
}

static void pull(int broadcast, double from, double rate, const int th[4], double up_ms[4], double phase)
{
    pull_timed(broadcast, from, rate, th, 0, 0, up_ms, phase);
}

static void test_logic(void)
{
    static const int th[4] = { 0, 2000, 2500, 3000 };
    double up[4], cross, worst_late[4] = { 0 }, worst_early[4] = { 0 };
    int s, b;
    double ph;
    struct sl_logic l;
    u32 t;

    for (b = 0; b <= 1; b++) {
        for (s = 1; s <= 3; s++)
            worst_late[s] = worst_early[s] = 0;
        for (ph = 0; ph < 100; ph += 5) {
            pull(b, 1000, 1300, th, up, ph); /* from 1,000: the 300 ms climb rate has settled by stage 1 */
            for (s = 1; s <= 3; s++) {
                cross = (th[s] - 1000) / 1300.0 * 1000.0;
                CHECK(up[s] > 0, "%s: stage %d never came up", b ? "broadcast" : "OBD", s);
                if (up[s] - cross > worst_late[s])
                    worst_late[s] = up[s] - cross;
                if (up[s] - cross < worst_early[s])
                    worst_early[s] = up[s] - cross;
            }
            CHECK(up[1] <= up[2] && up[2] <= up[3], "stages out of order");
        }
        printf("%s, 1300 rpm/s: stage 1 %+.0f..%+.0f ms, stage 2 %+.0f..%+.0f, stage 3 %+.0f..%+.0f around the true "
               "crossings (before the HUD call)\n",
               b ? "broadcast" : "OBD      ", worst_early[1], worst_late[1], worst_early[2], worst_late[2],
               worst_early[3], worst_late[3]);
        for (s = 1; s <= 3; s++) {
            CHECK(worst_late[s] <= (b ? 60 : 100), "%s: stage %d up to %.0f ms late", b ? "broadcast" : "OBD", s,
                  worst_late[s]);
            CHECK(worst_early[s] >= -300, "%s: stage %d up to %.0f ms early", b ? "broadcast" : "OBD", s,
                  worst_early[s]);
        }
    }

    /* an upshift at 3,450 to 2,200 (fixed stages), then lifting to idle: 3 is kept through the fall, then straight to 1 */
    sl_stages_init(&l, 2000, 2500, 3000, 250, 150, 300);
    sl_stages_tune(&l, 0, 0, 3000);
    for (t = 0; t <= 1500; t += 100)
        sl_stages_feed(&l, t, 1500 + (int)t * 13 / 10);
    CHECK(l.stage == 3, "the pull to 3,450 ended at stage %d", l.stage);
    sl_stages_feed(&l, 1600, 2200);
    CHECK(l.stage == 3 && l.falling == 1, "right after the shift: stage %d (falling %d), want 3 kept", l.stage,
          l.falling);
    {
        int changes = 0;
        for (t = 1700; t <= 2200; t += 100)
            changes += sl_stages_feed(&l, t, 2200);
        CHECK(changes == 1 && l.stage == 1, "steady 2,200 after the shift: %d changes to stage %d, want one, to 1",
              changes, l.stage);
    }
    sl_stages_feed(&l, 2300, 2300);
    CHECK(l.stage == 1, "2,300 holds stage 1 (it does not go up: 2,300 < 2,500)");
    for (t = 2300; t <= 3000; t += 100)
        sl_stages_feed(&l, t, 800);
    CHECK(l.stage == 0, "idle: stage %d", l.stage);

    /* hysteresis: hovering just under stage 3 keeps it, and does not flicker */
    sl_stages_init(&l, 2000, 2500, 3000, 250, 150, 300);
    sl_stages_feed(&l, 0, 3050);
    {
        int changes = 0;
        for (t = 100; t < 3000; t += 100)
            changes += sl_stages_feed(&l, t, 2900 + (int)(t % 200 ? 60 : -60));
        CHECK(l.stage == 3 && changes == 0, "hovering at 2,840..2,960: stage %d, %d changes", l.stage, changes);
    }

    /* earliest points of any spacing: wide apart */
    sl_stages_init(&l, 4000, 5500, 6000, 250, 150, 300);
    for (t = 0; t <= 4000; t += 100)
        sl_stages_feed(&l, t, 3000 + (int)t * 3 / 4);
    CHECK(l.stage == 3, "a pull to 6,000 with stages at 4,000 / 5,500 / 6,000 ended at stage %d", l.stage);
}

/* ---- timed white stages (after the drive of 2026-11-19) ----------------------------------------------------------- */

static void test_timed(void)
{
    static const int th[4] = { 0, 4500, 5000, 5500 };
    static const double rates[] = { 300, 600, 900, 1300, 2000, 3300, 4300 };
    unsigned k;
    printf("timed stages, shift point 5,500, 700 / 350 ms (broadcast; ms before the true crossing of 5,500, before "
           "the HUD call):\n     rpm/s   stage 1   stage 2   stage 3    1->2   2->3\n");
    for (k = 0; k < sizeof rates / sizeof rates[0]; k++) {
        double r = rates[k], from = r < 1000 ? 4000 : 2500, cross3 = (5500 - from) / r * 1000.0;
        double up[4], sum[4] = { 0 }, lo[4], hi[4], ph;
        int n = 0, s;
        for (s = 1; s <= 3; s++) {
            lo[s] = 1e9;
            hi[s] = -1e9;
        }
        for (ph = 0; ph < 100; ph += 5) {
            pull_timed(1, from, r, th, 700, 350, up, ph);
            for (s = 1; s <= 3; s++) {
                double rel = up[s] - cross3;
                CHECK(up[s] > 0, "%.0f rpm/s: stage %d never came up", r, s);
                sum[s] += rel;
                if (rel < lo[s])
                    lo[s] = rel;
                if (rel > hi[s])
                    hi[s] = rel;
            }
            CHECK(up[1] < up[2] && up[2] < up[3], "%.0f rpm/s: stages out of order (%.0f %.0f %.0f)", r, up[1], up[2],
                  up[3]);
            if (r <= 1300) {
                /* the timed part: each white stage the set time before the shift point, whatever the gear */
                CHECK(up[3] - up[1] >= 580 && up[3] - up[1] <= 820, "%.0f rpm/s: stage 1 %.0f ms before 3, want ~700",
                      r, up[3] - up[1]);
                CHECK(up[3] - up[2] >= 230 && up[3] - up[2] <= 470, "%.0f rpm/s: stage 2 %.0f ms before 3, want ~350",
                      r, up[3] - up[2]);
            } else {
                /* fast gears: the earliest points decide, as on the drive */
                for (s = 1; s <= 2; s++) {
                    double c = (th[s] - from) / r * 1000.0;
                    CHECK(up[s] - c >= -300 && up[s] - c <= 60, "%.0f rpm/s: stage %d %.0f ms from crossing %d", r, s,
                          up[s] - c, th[s]);
                }
            }
            CHECK(up[3] - cross3 >= -300 && up[3] - cross3 <= 60, "%.0f rpm/s: stage 3 %.0f ms from the shift point",
                  r, up[3] - cross3);
            n++;
        }
        printf("    %6.0f  %+8.0f  %+8.0f  %+8.0f  %6.0f %6.0f\n", r, sum[1] / n, sum[2] / n, sum[3] / n,
               (sum[2] - sum[1]) / n, (sum[3] - sum[2]) / n);
    }
    /* OBD (a new value every 200 ms): still in order, roughly timed */
    {
        double up[4], ph;
        for (ph = 0; ph < 200; ph += 10) {
            pull_timed(0, 4000, 600, th, 700, 350, up, ph);
            CHECK(up[1] > 0 && up[1] < up[2] && up[2] < up[3], "OBD 600 rpm/s: stages %.0f %.0f %.0f", up[1], up[2],
                  up[3]);
            CHECK(up[3] - up[1] >= 450 && up[3] - up[1] <= 950, "OBD 600 rpm/s: stage 1 %.0f ms before 3",
                  up[3] - up[1]);
        }
    }
}

/* ---- upshifts: a pull to the shift, the fall, the climb in the next gear ------------------------------------------- */

struct shift_sim {
    double r1, top, land, fall_ms, r2, t_shift;
};

static double sim_rpm(const struct shift_sim *p, double t)
{
    double f;
    if (t <= p->t_shift)
        return p->top - p->r1 * (p->t_shift - t) / 1000.0;
    if (t <= p->t_shift + p->fall_ms) {
        f = (t - p->t_shift) / p->fall_ms;
        f = 1.0 - (1.0 - f) * (1.0 - f); /* fast at first, easing into the new gear */
        return p->top + (p->land - p->top) * f;
    }
    return p->land + p->r2 * (t - p->t_shift - p->fall_ms) / 1000.0;
}

/* one upshift through the logic, broadcast-like (100 ms +- 15, 30 ms late). Returns the down-steps after the shift;
   *first_down the stage of the first one, *late_ms how long after the fall ended it came, *ups_after the up-steps
   after it, *top_reached 1 when stage 3 was up at the shift. */
static int run_shift(const struct shift_sim *p, u32 time1, u32 time2, int fall, int *first_down, double *late_ms,
                     int *ups_after, int *top_reached, double *from_start_ms)
{
    struct sl_logic l;
    double t = (rand() % 100), end = p->t_shift + p->fall_ms + 2500;
    int downs = 0, prev;
    sl_stages_init(&l, 4500, 5000, 5500, 250, 150, 300);
    sl_stages_tune(&l, time1, time2, fall);
    *first_down = -1;
    *late_ms = 0;
    *from_start_ms = 0;
    *ups_after = 0;
    *top_reached = 0;
    for (; t < end; t += 100 + (rand() % 31) - 15) {
        double v = sim_rpm(p, t - 30);
        if (v > 5450) /* the next gear's climb: stop before the next shift point */
            if (t > p->t_shift + p->fall_ms)
                break;
        prev = l.stage;
        if (t - 30 <= p->t_shift)
            *top_reached = 0;
        sl_stages_feed(&l, (u32)t, (int)v);
        if (t - 30 <= p->t_shift)
            *top_reached = l.stage == 3;
        if (t - 30 > p->t_shift && l.stage < prev) {
            if (!downs++) {
                *first_down = l.stage;
                *late_ms = t - (p->t_shift + p->fall_ms + 30);
                *from_start_ms = t - (p->t_shift + 30);
            }
        } else if (downs && l.stage > prev) {
            (*ups_after)++;
        }
    }
    return downs;
}

static void test_upshifts(void)
{
    static const double ratio[] = { 0.640, 0.670, 0.803, 0.763 }; /* ZF 8HP 1-2, 2-3, 3-4, 4-5 */
    static const double r1s[] = { 600, 900, 1500, 2500, 3500 };
    int i, mode, n = 0;
    srand(11);
    for (mode = 0; mode < 3; mode++) {
        /* 0: timed + shift falls (new), 1: fixed + shift falls, 2: fixed, every reading counts (as on the drive) */
        u32 tm1 = mode == 0 ? 700 : 0, tm2 = mode == 0 ? 350 : 0;
        int fall = mode == 2 ? 0 : 3000, between = 0, direct = 0, worst_late = -1000, all = 0, no_top = 0;
        double worst_start = 0, sum_start = 0;
        for (i = 0; i < 600; i++) {
            struct shift_sim p;
            int first, ups, top, downs;
            double late, from_start;
            p.r1 = r1s[i % 5];
            p.top = 5550 + rand() % 450;
            p.land = p.top * ratio[(i / 5) % 4];
            p.fall_ms = 100 + rand() % 201;
            p.r2 = p.r1 * ratio[(i / 5) % 4];
            p.t_shift = (p.top - 3000) / p.r1 * 1000.0;
            downs = run_shift(&p, tm1, tm2, fall, &first, &late, &ups, &top, &from_start);
            all++;
            if (!top) {
                no_top++;
                continue;
            }
            sum_start += from_start;
            if (from_start > worst_start)
                worst_start = from_start;
            if (mode == 0) /* off at the first reading of the fall: within a reading (+ jitter) of the fall starting */
                CHECK(from_start <= 230, "timed: upshift %d: off %.0f ms after the fall started", i, from_start);
            if (downs > 1)
                between++;
            else if (downs == 1)
                direct++;
            if (late > worst_late)
                worst_late = (int)late;
            if (mode < 2) {
                CHECK(downs == 1, "mode %d: upshift %d (%.0f rpm/s, %.0f -> %.0f in %.0f ms): %d down-steps, want 1",
                      mode, i, p.r1, p.top, p.land, p.fall_ms, downs);
                CHECK(late <= 380, "mode %d: upshift %d: down %.0f ms after the fall ended", mode, i, late);
            }
            if (mode == 0)
                CHECK(first == 0, "timed: upshift %d landed at %.0f: stage %d, want 0 (more than 0.35 s from 5,500)",
                      i, p.land, first);
            if (mode == 1) /* the fall's slow tail counts as landed: up to ~350 rpm above the end of it */
                CHECK(first >= (p.land >= 5000 ? 2 : p.land >= 4500 ? 1 : 0) &&
                          first <= (p.land + 350 >= 5000 ? 2 : p.land + 350 >= 4500 ? 1 : 0),
                      "fixed: upshift %d landed at %.0f: stage %d", i, p.land, first);
        }
        printf("upshifts, %s: %d of %d went through a stage in between, %d straight; first down-step %.0f ms after "
               "the fall started on average (at most %.0f), at most %d ms after it ended%s\n",
               mode == 0 ? "timed + cut at shift" : mode == 1 ? "fixed + shift falls " : "as on the 1st drive ", between,
               all - no_top, direct, all - no_top ? sum_start / (all - no_top) : 0, worst_start, worst_late,
               no_top ? " (some never reached stage 3)" : "");
        CHECK(no_top == 0, "mode %d: %d pulls never reached stage 3", mode, no_top);
        if (mode == 2)
            CHECK(between > 0, "the drive's logic should step through stages (the model is off)");
        n += all;
    }
    (void)n;
}

/* lifting off (no shift): the stages still come down, through the normal hold; a long fast fall is followed */
static void test_falls(void)
{
    struct sl_logic l;
    u32 t;
    int downs = 0, ever_falling = 0, held_ms = 0;
    sl_stages_init(&l, 4500, 5000, 5500, 250, 150, 300);
    for (t = 0; t <= 3000; t += 100)
        sl_stages_feed(&l, t, 3500 + (int)t * 7 / 10); /* to 5,600 at 700 rpm/s */
    CHECK(l.stage == 3, "the pull to 5,600: stage %d", l.stage);
    for (t = 3100; t <= 6000; t += 100) {
        downs += sl_stages_feed(&l, t, 5600 - (int)(t - 3000) * 3 / 2); /* lifting off: -1,500 rpm/s */
        ever_falling |= l.falling;
    }
    CHECK(l.stage == 0 && !ever_falling && downs >= 1, "lifting off: stage %d, falling %d, %d down-steps", l.stage,
          ever_falling, downs);

    /* hard braking in a low gear: -4,000 rpm/s for 1.2 s; kept at most fall_max_ms, then followed down */
    sl_stages_init(&l, 4500, 5000, 5500, 250, 150, 300);
    for (t = 0; t <= 3000; t += 100)
        sl_stages_feed(&l, t, 3500 + (int)t * 7 / 10);
    for (t = 3100; t <= 5000; t += 100) {
        int v = 5600 - (int)(t - 3000) * 4;
        if (v < 900)
            v = 900;
        sl_stages_feed(&l, t, v);
        if (l.stage == 3)
            held_ms = (int)(t - 3000);
    }
    CHECK(l.stage == 0, "a long fast fall: stage %d at the end", l.stage);
    CHECK(held_ms <= 600 + 300 + 100, "a long fast fall: stage 3 kept %d ms", held_ms);
}

/* the drive's stage 1 flashes: a quick flare (kickdown or launch) that stops ~4,200, read at the logged rpm and slope.
   Assumed: steady before it, rising at the logged slope for flare_ms up to the logged reading, then +150 rpm/s. */
static int flashes(u32 rate_ms, u32 flare_ms)
{
    static const int rpm[] = { 4058, 4097, 4001, 3992, 3834 }, slope[] = { 3450, 2830, 4158, 4326, 4990 };
    int k, n = 0;
    for (k = 0; k < 5; k++) {
        struct sl_logic l;
        double t;
        int shown = 0;
        sl_stages_init(&l, 4500, 5000, 5500, 250, 150, 300);
        l.rate_ms = rate_ms;
        for (t = 0; t < 3000; t += 100) {
            double since = t - 1000; /* the flare's reading is at t = 1000 */
            double v = since <= -(double)flare_ms ? rpm[k] - slope[k] * flare_ms / 1000.0
                       : since <= 0              ? rpm[k] + slope[k] * since / 1000.0
                                                 : rpm[k] + 150.0 * since / 1000.0;
            if (v > 4250)
                v = 4250 + (v - 4250) / 8; /* it stops around 4,200 */
            v += ((int)t / 100 % 3) - 1;      /* the broadcast is never quite flat */
            sl_stages_feed(&l, (u32)t, (int)v);
            shown |= l.stage > 0;
        }
        n += shown;
    }
    return n;
}

static void test_flashes(void)
{
    int a = flashes(100, 200), b = flashes(300, 200), c = flashes(100, 400), d = flashes(300, 400);
    printf("stage 1 flashes of the drive (5), modelled: flare 200 ms: %d with the 100 ms slope, %d with the 300 ms "
           "rate; flare 400 ms: %d, %d\n", a, b, c, d);
    CHECK(b <= a && d <= c, "the 300 ms rate gives more flashes");
}

/* the car's broadcast, 2026-11-19: replay the 05.00 CD0 samples through the stage logic */
static void test_replay(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    static double tt[2000];
    static int rpm[2000];
    int n = 0, i, ups = 0, top = 0;
    struct sl_logic l;
    if (!f) {
        CHECK(0, "cannot open %s", path);
        return;
    }
    while (fgets(line, sizeof line, f) && n < 2000) {
        double t;
        unsigned v;
        if (sscanf(line, " %lf k2 f0 5000>2021 05.00 cd0 op c n2 %4x", &t, &v) == 2) {
            tt[n] = t;
            rpm[n] = (int)(v >> 2);
            n++;
        }
    }
    fclose(f);
    CHECK(n > 500, "%d broadcast samples in the log", n);
    sl_stages_init(&l, 2000, 2500, 3000, 250, 150, 300);
    for (i = 0; i < n; i++) {
        int was = l.stage;
        if (sl_stages_feed(&l, (u32)tt[i], rpm[i])) {
            int j, peak = 0;
            for (j = i; j < n && j < i + 5; j++)
                if (rpm[j] > peak)
                    peak = rpm[j];
            if (l.stage > was) {
                ups++;
                CHECK(peak >= l.th[l.stage] - 300, "stage %d at %.1f ms but the next 0.5 s peak at %d", l.stage,
                      tt[i], peak);
            }
            if (l.stage > top)
                top = l.stage;
        }
    }
    printf("broadcast replay, stages at 2000/2500/3000: %d samples, %d steps up, highest stage %d\n", n, ups, top);
    CHECK(top == 3, "the blips to 3,400 never reached stage 3");
    CHECK(l.stage == 0, "stage %d at the end (idle)", l.stage);
}

/* the second drive (2026-11-19, every reading logged): the readings again through the logic as it drove (fall_cut 0)
   and with the cut at the shift (1). The cut must come sooner at every upshift and change nothing else. */
static int replay_drive(const char *path, int cut, double *t30, int max, int *changes)
{
    FILE *f = fopen(path, "r");
    char line[512];
    struct sl_logic l;
    int n = 0, prev = 0, st, rpm;
    double t;
    if (!f)
        return -1;
    sl_stages_init(&l, 4500, 5000, 5500, 250, 150, 300);
    l.fall_cut = cut;
    *changes = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " %lf ms  rpm %d (b)", &t, &rpm) != 2 &&
            sscanf(line, " %lf ms  stage %d  rpm %d (b)", &t, &st, &rpm) != 3)
            continue;
        if (sl_stages_feed(&l, (u32)t, rpm)) {
            (*changes)++;
            if (prev == 3 && l.stage == 0 && n < max)
                t30[n++] = t;
            prev = l.stage;
        }
    }
    fclose(f);
    return n;
}

static void test_replay_drive(const char *path)
{
    static double a[64], b[64];
    int ca, cb, na = replay_drive(path, 0, a, 64, &ca), nb = replay_drive(path, 1, b, 64, &cb), i, sooner = 0;
    double sum = 0, lo = 1e9, hi = 0;
    CHECK(na == 16, "the drive as it ran: %d changes 3 -> 0, the log has 16", na);
    CHECK(nb == na && cb == ca, "with the cut: %d upshifts and %d changes, as it ran %d and %d", nb, cb, na, ca);
    for (i = 0; i < na && i < nb; i++) {
        double d = a[i] - b[i];
        CHECK(d > 0 && d < 600, "upshift %d: the cut came %.0f ms sooner", i, d);
        sooner += d > 0;
        sum += d;
        if (d < lo)
            lo = d;
        if (d > hi)
            hi = d;
    }
    printf("second drive replayed: %d upshifts, %d stage changes; with the cut at the shift orange goes off %.0f ms "
           "sooner on average (%.0f..%.0f), at %d of %d\n", na, ca, na ? sum / na : 0, lo, hi, sooner, na);
}

int main(int argc, char **argv)
{
    test_renderer();
    test_logic();
    test_timed();
    test_upshifts();
    test_falls();
    test_flashes();
    if (argc > 1)
        test_replay(argv[1]);
    if (argc > 2)
        test_replay_drive(argv[2]);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

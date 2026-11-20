/* SPDX-License-Identifier: MIT */
#include "logic.h"

void sl_logic_init(struct sl_logic *l, int on_rpm, int hyst, u32 lead_ms, u32 min_on_ms)
{
    struct sl_logic z = { 0 };
    *l = z;
    l->on_rpm = on_rpm;
    l->hyst = hyst;
    l->lead_ms = lead_ms;
    l->min_on_ms = min_on_ms;
    l->max_horizon_ms = 400; /* never extrapolate further than this */
    l->stale_ms = 500;       /* a value unchanged this long: steady revs, no slope */
    l->max_dt_ms = 600;      /* a slope only between changes closer than this */
    l->rate_ms = 300;        /* the climb rate over this much of the last changes */
    l->fall_quiet_ms = 250;  /* a shift's fall with no new value this long has ended (OBD refreshes every 200 ms) */
    l->fall_max_ms = 600;    /* a fall longer than this is no shift: it is followed */
}

void sl_stages_init(struct sl_logic *l, int t1, int t2, int t3, int hyst, u32 lead_ms, u32 min_on_ms)
{
    int s;
    sl_logic_init(l, t3, hyst, lead_ms, min_on_ms);
    l->th[1] = t1;
    l->th[2] = t2;
    l->th[3] = t3;
    for (s = 1; s <= 3; s++)
        l->on[s] = l->on_at[s] = l->th[s];
    sl_stages_tune(l, 700, 350, 3000);
    l->fall_cut = 1;
}

void sl_stages_tune(struct sl_logic *l, u32 time1_ms, u32 time2_ms, int fall_rpm_s)
{
    l->time_ms[1] = time1_ms;
    l->time_ms[2] = time2_ms;
    l->fall_rpm_s = fall_rpm_s;
}

/* ---- the last value changes and the climb rate over them ------------------------------------------------------------ */

static void hist_reset(struct sl_logic *l, u32 t, int rpm)
{
    l->hpos = 0;
    l->hn = 1;
    l->ht[0] = t;
    l->hv[0] = rpm;
    l->rate = 0;
}

static void hist_add(struct sl_logic *l, u32 t, int rpm)
{
    l->hpos = (l->hpos + 1) & (SL_HIST - 1);
    l->ht[l->hpos] = t;
    l->hv[l->hpos] = rpm;
    if (l->hn < SL_HIST)
        l->hn++;
}

static int clamp_rate(int r) { return r > 20000 ? 20000 : r < -20000 ? -20000 : r; }

/* rpm/s to the newest change from the newest one at least rate_ms before it (else the oldest kept): three or four
   broadcast steps, two or three OBD refreshes */
static int hist_rate(const struct sl_logic *l)
{
    int k, i, old = -1;
    u32 tn = l->ht[l->hpos], dt;
    for (k = 1; k < l->hn; k++) {
        i = (l->hpos - k) & (SL_HIST - 1);
        old = i;
        if (tn - l->ht[i] >= l->rate_ms)
            break;
    }
    if (old < 0)
        return 0;
    dt = tn - l->ht[old];
    return dt ? clamp_rate(sl_sdiv((l->hv[l->hpos] - l->hv[old]) * 1000, (int)dt)) : 0;
}

/* the last two distinct values, the slope between them, the climb rate, and the prediction for t. Returns 1 when rpm is
   a new value. */
static int predict(struct sl_logic *l, u32 t, int rpm)
{
    int rate = 0, changed = 0;
    u32 age, horizon;
    if (!l->have) {
        l->have = 1;
        l->v1 = l->v0 = rpm;
        l->t1 = l->t0 = t;
        l->slope = 0;
        l->hn = 0; /* the first reading is no change (the value may have been there a while): the rate starts after it */
        l->rate = 0;
        changed = 1;
    } else if (rpm != l->v1) {
        u32 dt = t - l->t1;
        l->v0 = l->v1;
        l->t0 = l->t1;
        l->v1 = rpm;
        l->t1 = t;
        if (dt > 0 && dt <= l->max_dt_ms) {
            l->slope = clamp_rate(sl_sdiv((rpm - l->v0) * 1000, (int)dt));
            hist_add(l, t, rpm);
            l->rate = hist_rate(l);
        } else if (dt == 0) {
            l->slope = 0;
            l->hv[l->hpos] = rpm; /* two values in the same ms: the newer one counts */
            l->rate = hist_rate(l);
        } else {
            l->slope = 0;
            hist_reset(l, t, rpm);
        }
        changed = 1;
    }
    age = t - l->t1;
    if (age <= l->stale_ms)
        rate = l->rate;
    horizon = age + l->lead_ms;
    if (horizon > l->max_horizon_ms)
        horizon = l->max_horizon_ms;
    l->pred = l->v1 + (rate > 0 ? sl_sdiv(rate * (int)horizon, 1000) : 0);
    return changed;
}

/* ---- one bar (step 4) ------------------------------------------------------------------------------------------------ */

int sl_logic_feed(struct sl_logic *l, u32 t, int rpm)
{
    int was = l->shown;
    predict(l, t, rpm);
    if (!l->shown) {
        if (l->pred >= l->on_rpm) {
            l->shown = 1;
            l->shown_at = t;
        }
    } else if (l->v1 < l->on_rpm - l->hyst && l->pred < l->on_rpm && t - l->shown_at >= l->min_on_ms) {
        l->shown = 0;
    }
    return l->shown != was;
}

/* ---- stages (step 5) ------------------------------------------------------------------------------------------------- */

/* where each stage comes up now, at climb rate r (0: not climbing): the white ones time_ms[s] before the shift point,
   never earlier than th[s] (time_ms[s] 0: always at th[s]) */
static void stage_on(struct sl_logic *l, int r)
{
    int s, x;
    l->on[3] = l->th[3];
    for (s = 1; s <= 2; s++) {
        if (!l->time_ms[s]) {
            l->on[s] = l->th[s];
            continue;
        }
        x = l->th[3] - (r > 0 ? sl_sdiv(r * (int)l->time_ms[s], 1000) : 0);
        l->on[s] = x > l->th[s] ? x : l->th[s];
    }
}

static int stage_up(const struct sl_logic *l)
{
    int s, up = 0;
    for (s = 1; s <= 3; s++)
        if (l->pred >= l->on[s])
            up = s;
    return up;
}

static void stage_set(struct sl_logic *l, int s, u32 t)
{
    int k;
    for (k = l->stage + 1; k <= s; k++)
        l->on_at[k] = l->on[k];
    l->stage = s;
    l->stage_at = t;
}

int sl_stages_feed(struct sl_logic *l, u32 t, int rpm)
{
    int was = l->stage, changed = predict(l, t, rpm), up, hold = 0, s;
    stage_on(l, t - l->t1 <= l->stale_ms && l->rate > 0 ? l->rate : 0);
    l->landed = 0;
    if (l->fall_rpm_s > 0) {
        if (changed && l->slope <= -l->fall_rpm_s) {
            /* falling like a shift: these readings make no stage, and the climb rate starts again after them */
            if (!l->falling) {
                l->falling = 1;
                l->fall_at = t;
                if (l->fall_cut && l->time_ms[1] && l->stage) {
                    /* timed stages land at 0 below the shift point: off now, in step with the shift */
                    l->stage = 0;
                    l->stage_at = t;
                    l->landed = 2;
                }
            } else if (l->falling == 1 && t - l->fall_at > l->fall_max_ms) {
                l->falling = 2;
            }
            hist_reset(l, t, rpm);
            if (l->falling == 1)
                return l->stage != was;
        } else if (l->falling == 1) {
            if (!changed && t - l->t1 < l->fall_quiet_ms && t - l->fall_at <= l->fall_max_ms)
                return 0; /* no new value yet */
            /* the fall has ended: straight to the stage for where the revs landed (no climb rate yet) */
            l->falling = 0;
            hist_reset(l, l->t1, l->v1);
            l->pred = l->v1;
            stage_on(l, 0);
            up = stage_up(l);
            if (up != l->stage) {
                stage_set(l, up, t);
                l->landed = 1;
            }
            return l->stage != was;
        } else if (changed && l->falling == 2) {
            l->falling = 0;
        }
    }
    up = stage_up(l);
    if (up > l->stage) {
        stage_set(l, up, t);
    } else if (up < l->stage && t - l->stage_at >= l->min_on_ms) {
        for (s = 1; s <= l->stage; s++)
            if (l->v1 >= l->on_at[s] - l->hyst || l->pred >= l->on[s])
                hold = s;
        if (hold < l->stage) {
            l->stage = hold;
            l->stage_at = t;
        }
    }
    return l->stage != was;
}

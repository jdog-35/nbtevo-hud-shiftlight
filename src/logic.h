/* SPDX-License-Identifier: MIT */
/* When to show the shift light, from the engine speed as it arrives (the 05.00 CD0 broadcast every ~100 ms, or the DME's
 * OBD 01 0C, which refreshes only every ~200 ms). Pure C (host-tested).
 *
 * The climb rate is taken over the last rate_ms of value changes (300 ms: three broadcast steps, so one jumpy step does
 * not count much), and the prediction extrapolates the last value over its age (time since it changed) plus a lead for
 * the HUD call, for rising revs only:
 *
 *   pred = v1 + rate * min(age + lead, max_horizon)      (rate > 0 and the value changed within stale_ms)
 *
 * One bar (step 4): shown when pred >= on_rpm; hidden when v1 < on_rpm - hyst and pred < on_rpm, after min_on_ms.
 *
 * Stages (step 5, timed since the drive of 2026-11-19): stage 3 (orange) at the shift point th[3]. The white stages come
 * a set time before it at the current climb rate, never earlier than th[1] and th[2]:
 *
 *   on[s] = max(th[s], th[3] - rate * time_ms[s])        (s = 1, 2; not climbing: th[3])
 *
 * so in every gear each white stage lasts about the same time (in 1st and 2nd th[1] and th[2] decide).
 * Up at once to the highest stage with pred >= on[s]. Down to the highest stage still held (v1 >= the on[s] it came up
 * at - hyst, or pred >= on[s]) once the current one has lasted min_on_ms.
 * A shift: the revs falling faster than fall_rpm_s (upshifts fell 4,800-9,400 rpm/s, lifting off at most 2,700). The
 * readings of the fall make no stage. With timed stages it lands below the shift point, so the light goes off at the
 * first reading of the fall (fall_cut, since the second drive: ~0.2-0.3 s sooner); otherwise the stage is kept until the
 * fall ends, then goes straight to the stage for where the revs landed. No stage in between either way. */
#ifndef SL_LOGIC_H
#define SL_LOGIC_H

#include "sltypes.h"

#define SL_HIST 8 /* value changes kept for the climb rate (a power of 2) */

struct sl_logic {
    /* settings */
    int on_rpm, hyst;
    int th[4];         /* stages: th[3] the shift point, th[1] < th[2] < th[3] the earliest the white stages come */
    u32 time_ms[4];    /* stages: how long before the shift point stages 1 and 2 come (0: always at th[s]) */
    int fall_rpm_s;    /* stages: a fall at least this fast is a shift (0: off) */
    int fall_cut;      /* stages, timed: off at the first reading of a shift's fall (it always lands below th[3]) */
    u32 lead_ms, min_on_ms, max_horizon_ms, stale_ms, max_dt_ms, rate_ms, fall_quiet_ms, fall_max_ms;
    /* state */
    int have, shown, stage;
    int v1, v0;        /* the last two distinct values */
    u32 t1, t0;        /* when they arrived */
    int slope;         /* rpm/s between them; 0 when unknown */
    int rate;          /* rpm/s over the last rate_ms of changes; 0 when unknown */
    int pred;          /* the last prediction */
    int on[4];         /* stages: the rpm each stage comes up at, now */
    int on_at[4];      /* stages: the rpm each shown stage came up at (its hold is that - hyst) */
    int falling;       /* stages: 1 a shift's fall (stage kept), 2 a fall that went on too long (followed) */
    int landed;        /* stages: the last change ended a shift's fall (1), or was the cut at its first reading (2) */
    u32 shown_at, stage_at, fall_at;
    u32 ht[SL_HIST];   /* the last value changes, newest at hpos */
    int hv[SL_HIST], hn, hpos;
};

void sl_logic_init(struct sl_logic *l, int on_rpm, int hyst, u32 lead_ms, u32 min_on_ms);
/* One reading at t_ms (any monotonic ms clock). Returns 1 when l->shown changed. */
int sl_logic_feed(struct sl_logic *l, u32 t_ms, int rpm);

/* Stages with the earliest points t1 < t2 and the shift point t3; timed white stages (700 / 350 ms before the shift
   point), shifts at 3,000 rpm/s: change with sl_stages_tune. */
void sl_stages_init(struct sl_logic *l, int t1, int t2, int t3, int hyst, u32 lead_ms, u32 min_on_ms);
/* time1_ms > time2_ms > 0: stages 1 and 2 that long before the shift point (0 / 0: always at t1 / t2, as before the
   drive of 2026-11-19); fall_rpm_s: a fall this fast is a shift (0: off, every reading counts) */
void sl_stages_tune(struct sl_logic *l, u32 time1_ms, u32 time2_ms, int fall_rpm_s);
/* One reading. Returns 1 when l->stage (0..3) changed. */
int sl_stages_feed(struct sl_logic *l, u32 t_ms, int rpm);

#endif

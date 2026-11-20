/* SPDX-License-Identifier: MIT */
/* The three-stage shift light on the HUD's two picture buffers (step 5). Pure C: it only produces the calls, so the host
 * tests can run every transition against a model of the HUD.
 *
 * Look (chosen 2026-11-18): bars along the top of the picture, x0..x1 and y0..y0+h-1,
 *   stage 1  a white bar at each end, pct1 % of the span each
 *   stage 2  a white bar at each end, pct2 % each
 *   stage 3  one orange bar across (the shift point)
 *
 * What the car showed (2026-11-19): shapes stack in send order; black (7) covers white; a non-convex 8-point shape fills
 * as two bars (hud_two_bars); ImageRemoveItem does not work. So:
 *   picture 6  the orange bar, drawn once (recoloured by day/night);
 *   picture 5  built by adding on top: the stage 1 shape (white), the stage 2 shape (white) over it on the way up, a black
 *              mask over the stage 2 extensions on the way down; reset to the stage 1 shape while hidden.
 * Exactly one of 5 and 6 is visible, and the panel (1) shows or hides it. Visible changes are always one call:
 *   off -> 1  panel on                    1 -> 2  the stage 2 shape (+ a fill colour call before it, when needed)
 *   2 -> 3    SwitchVisibility 5 -> 6     2 -> 1  the black mask (+ a fill colour call before it, when needed)
 *   3 -> 2    SwitchVisibility 6 -> 5 */
#ifndef SL_STAGES_H
#define SL_STAGES_H

#include "hud.h"

#define SL_STAGE_MAX_CALLS 12
#define SL_STAGE_MASK_COLOUR 7 /* black: covers white on this HUD, invisible by day and night */
#define SL_P5_MAX_ITEMS 24     /* picture 5 is redrawn before it holds more shapes than this */

struct sl_stage_pics {
    /* settings */
    int x0, x1, y0, h;
    u8 white, orange;
    /* derived: the stage 1 and stage 2 bar ends (left bar x0..lx1, right bar rx0..x1) */
    int s1_lx1, s1_rx0, s2_lx1, s2_rx0;
    /* the HUD as we left it */
    int stage, frame_on, vis6;
    int p5_look;  /* what picture 5 shows: 0 nothing drawn, 1 stage 1 bars, 2 stage 2 bars */
    int p5_fill;  /* picture 5's current fill colour, -1 unknown */
    int p5_items; /* shapes in picture 5 since its last RemoveAll */
};

/* Settings in; derives the bar ends. */
void sl_stage_init(struct sl_stage_pics *s, int x0, int x1, int y0, int h, int pct1, int pct2, u8 white, u8 orange);
/* After the HUD is at rest (panel off, 5 and 6 empty, 6 visible): draws 6 and 5, makes 5 the visible one. Returns the
   number of calls (at most SL_STAGE_MAX_CALLS). */
int sl_stage_setup(struct sl_stage_pics *s, struct hud_call *c);
/* The calls that show stage t (0..3) from the current state; 0 when already there. */
int sl_stage_to(struct sl_stage_pics *s, int t, struct hud_call *c);
/* The orange bar again in another colour (day/night). */
int sl_stage_recolour(struct sl_stage_pics *s, u8 orange, struct hud_call *c);

#endif

/* SPDX-License-Identifier: MIT */
#include "stages.h"

void sl_stage_init(struct sl_stage_pics *s, int x0, int x1, int y0, int h, int pct1, int pct2, u8 white, u8 orange)
{
    int w = x1 - x0 + 1, n1 = sl_sdiv(w * pct1, 100), n2 = sl_sdiv(w * pct2, 100);
    if (n1 < 1)
        n1 = 1;
    if (n2 <= n1)
        n2 = n1 + 1;
    s->x0 = x0;
    s->x1 = x1;
    s->y0 = y0;
    s->h = h;
    s->white = white;
    s->orange = orange;
    s->s1_lx1 = x0 + n1 - 1;
    s->s1_rx0 = x1 - n1 + 1;
    s->s2_lx1 = x0 + n2 - 1;
    s->s2_rx0 = x1 - n2 + 1;
    s->stage = 0;
    s->frame_on = 0;
    s->vis6 = 1; /* the resting state: picture 6 visible */
    s->p5_look = 0;
    s->p5_fill = -1;
    s->p5_items = 0;
}

static int orange_bar(struct sl_stage_pics *s, struct hud_call *c)
{
    int k = 0;
    hud_remove_all(&c[k++], HUD_BUF_B);
    hud_fill(&c[k++], HUD_BUF_B, s->orange);
    hud_line_width(&c[k++], HUD_BUF_B, 0);
    hud_rect(&c[k++], HUD_BUF_B, 0, s->x0, s->y0, s->x1, s->y0 + s->h - 1);
    return k;
}

static int p5_fill(struct sl_stage_pics *s, struct hud_call *c, u8 colour)
{
    if (s->p5_fill == colour)
        return 0;
    hud_fill(c, HUD_BUF_A, colour);
    s->p5_fill = colour;
    return 1;
}

/* picture 5 from scratch: emptied, white, no outline, the stage 1 shape */
static int p5_reset(struct sl_stage_pics *s, struct hud_call *c)
{
    int k = 0;
    hud_remove_all(&c[k++], HUD_BUF_A);
    s->p5_fill = -1; /* not assumed to survive RemoveAll */
    k += p5_fill(s, c + k, s->white);
    hud_line_width(&c[k++], HUD_BUF_A, 0);
    hud_two_bars(&c[k++], HUD_BUF_A, 0, s->x0, s->s1_lx1, s->s1_rx0, s->x1, s->y0, s->y0 + s->h - 1);
    s->p5_look = 1;
    s->p5_items = 1;
    return k;
}

/* stage 2 over stage 1: the stage 2 shape in white on top */
static int p5_up_to_2(struct sl_stage_pics *s, struct hud_call *c)
{
    int k = p5_fill(s, c, s->white);
    hud_two_bars(&c[k++], HUD_BUF_A, (u16)s->p5_items, s->x0, s->s2_lx1, s->s2_rx0, s->x1, s->y0, s->y0 + s->h - 1);
    s->p5_items++;
    s->p5_look = 2;
    return k;
}

/* stage 1 over stage 2: black over the stage 2 extensions (and the gap between them) */
static int p5_down_to_1(struct sl_stage_pics *s, struct hud_call *c)
{
    int k = p5_fill(s, c, SL_STAGE_MASK_COLOUR);
    hud_rect(&c[k++], HUD_BUF_A, (u16)s->p5_items, s->s1_lx1 + 1, s->y0, s->s1_rx0 - 1, s->y0 + s->h - 1);
    s->p5_items++;
    s->p5_look = 1;
    return k;
}

int sl_stage_setup(struct sl_stage_pics *s, struct hud_call *c)
{
    int k = 0;
    k += orange_bar(s, c + k);
    k += p5_reset(s, c + k);
    hud_switch(&c[k++], HUD_BUF_B, HUD_BUF_A); /* 5 up, 6 down; the panel stays off */
    s->vis6 = 0;
    s->frame_on = 0;
    s->stage = 0;
    return k;
}

int sl_stage_to(struct sl_stage_pics *s, int t, struct hud_call *c)
{
    int k = 0;
    if (t == s->stage || t < 0 || t > 3)
        return 0;
    switch (t) {
    case 0:
        if (s->frame_on) {
            hud_vis(&c[k++], HUD_FRAME, 0);
            s->frame_on = 0;
        }
        /* ready for the next rise, while hidden: a fresh stage 1 in picture 5, the visible one */
        if (s->vis6) {
            hud_switch(&c[k++], HUD_BUF_B, HUD_BUF_A);
            s->vis6 = 0;
        }
        if (s->p5_look != 1 || s->p5_items > 1)
            k += p5_reset(s, c + k);
        break;
    case 1:
    case 2:
        if (s->p5_look == 0 || s->p5_items >= SL_P5_MAX_ITEMS)
            k += p5_reset(s, c + k); /* a blink if picture 5 is up: only after many 1 <-> 2 changes */
        if (t == 2 && s->p5_look != 2)
            k += p5_up_to_2(s, c + k);
        if (t == 1 && s->p5_look != 1)
            k += p5_down_to_1(s, c + k);
        if (s->vis6) {
            hud_switch(&c[k++], HUD_BUF_B, HUD_BUF_A);
            s->vis6 = 0;
        }
        if (!s->frame_on) {
            hud_vis(&c[k++], HUD_FRAME, 1);
            s->frame_on = 1;
        }
        break;
    case 3:
        if (!s->vis6) {
            hud_switch(&c[k++], HUD_BUF_A, HUD_BUF_B);
            s->vis6 = 1;
        }
        if (!s->frame_on) {
            hud_vis(&c[k++], HUD_FRAME, 1);
            s->frame_on = 1;
        }
        break;
    }
    s->stage = t;
    return k;
}

int sl_stage_recolour(struct sl_stage_pics *s, u8 orange, struct hud_call *c)
{
    s->orange = orange;
    return orange_bar(s, c);
}

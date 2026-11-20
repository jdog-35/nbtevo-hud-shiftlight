/* SPDX-License-Identifier: MIT */
#include "hud.h"

static void be16(u8 *o, int v)
{
    o[0] = (u8)((u32)v >> 8);
    o[1] = (u8)v;
}

void hud_vis(struct hud_call *c, u16 h, int on)
{
    c->fkt = HUD_SET_VISIBILITY;
    be16(c->p, h);
    c->p[2] = on ? 1 : 0;
    c->n = 3;
}

void hud_switch(struct hud_call *c, u16 hide, u16 show)
{
    c->fkt = HUD_SWITCH_VISIBILITY;
    be16(c->p, hide);
    be16(c->p + 2, show);
    c->n = 4;
}

void hud_remove_all(struct hud_call *c, u16 h)
{
    c->fkt = HUD_REMOVE_ALL;
    be16(c->p, h);
    c->n = 2;
}

void hud_fill(struct hud_call *c, u16 h, u8 colour)
{
    c->fkt = HUD_SET_FILL_COLOR;
    be16(c->p, h);
    c->p[2] = colour;
    c->n = 3;
}

void hud_line_width(struct hud_call *c, u16 h, u8 w)
{
    c->fkt = HUD_SET_LINE_WIDTH;
    be16(c->p, h);
    c->p[2] = w;
    c->n = 3;
}

void hud_rect(struct hud_call *c, u16 h, u16 item, int x0, int y0, int x1, int y1)
{
    const int pts[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
    int i;
    c->fkt = HUD_ADD_POLYGON;
    be16(c->p, h);
    be16(c->p + 2, item);
    c->p[4] = 3; /* filled */
    c->p[5] = 4;
    for (i = 0; i < 8; i++)
        be16(c->p + 6 + 2 * i, pts[i] & 0xffff);
    c->n = 22;
}

void hud_text_empty(struct hud_call *c, u16 h)
{
    c->fkt = HUD_SET_TEXT;
    be16(c->p, h);
    c->p[2] = 0; /* encoding */
    c->p[3] = 0; /* terminator 0000 */
    c->p[4] = 0;
    c->n = 5;
}

void hud_two_bars(struct hud_call *c, u16 h, u16 item, int x0, int lx1, int rx0, int x1, int y0, int y1)
{
    const int pts[16] = { x0, y0, x1, y0, x1, y1, rx0, y1, rx0, y0, lx1, y0, lx1, y1, x0, y1 };
    int i;
    c->fkt = HUD_ADD_POLYGON;
    be16(c->p, h);
    be16(c->p + 2, item);
    c->p[4] = 3; /* filled */
    c->p[5] = 8;
    for (i = 0; i < 16; i++)
        be16(c->p + 6 + 2 * i, pts[i] & 0xffff);
    c->n = 38;
}

int hud_clear_seq(struct hud_call *c)
{
    hud_vis(&c[0], HUD_FRAME, 0);
    hud_remove_all(&c[1], HUD_BUF_A);
    hud_remove_all(&c[2], HUD_BUF_B);
    hud_text_empty(&c[3], HUD_STREET);
    hud_text_empty(&c[4], HUD_DIST);
    hud_text_empty(&c[5], HUD_UNIT);
    hud_vis(&c[6], HUD_STREET, 0);
    return 7;
}

int hud_call_ok(const struct hud_call *c)
{
    u16 h, h2;
    if (c->n < 2 || c->n > sizeof c->p)
        return 0;
    h = (u16)(c->p[0] << 8 | c->p[1]);
    if (h < HUD_FRAME || h > HUD_BUF_B)
        return 0;
    switch (c->fkt) {
    case HUD_SET_VISIBILITY:
        return c->n == 3 && c->p[2] <= 1;
    case HUD_SWITCH_VISIBILITY:
        h2 = (u16)(c->p[2] << 8 | c->p[3]);
        return c->n == 4 && h >= HUD_BUF_A && h2 >= HUD_BUF_A && h2 <= HUD_BUF_B && h != h2;
    case HUD_SET_TEXT:
        return c->n == 5 && !c->p[2] && !c->p[3] && !c->p[4];
    case HUD_SET_LINE_WIDTH:
    case HUD_SET_FILL_COLOR:
        return c->n == 3 && h >= HUD_BUF_A;
    case HUD_ADD_POLYGON: /* filled rectangles (4 points) and two-bar shapes (8 points), as the car drew them */
        return h >= HUD_BUF_A && c->p[4] == 3 &&
               ((c->p[5] == 4 && c->n == 22) || (c->p[5] == 8 && c->n == 38));
    case HUD_REMOVE_ALL:
        return c->n == 2 && h >= HUD_BUF_A;
    }
    return 0;
}

void hud_format(const struct hud_call *c, char *b)
{
    static const char hx[] = "0123456789abcdef";
    u32 i, k = 0;
    b[k++] = hx[(c->fkt >> 8) & 15];
    b[k++] = hx[(c->fkt >> 4) & 15];
    b[k++] = hx[c->fkt & 15];
    b[k++] = ' ';
    for (i = 0; i < c->n && i < sizeof c->p; i++) {
        b[k++] = hx[c->p[i] >> 4];
        b[k++] = hx[c->p[i] & 15];
    }
    b[k] = 0;
}

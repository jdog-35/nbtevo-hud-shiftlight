/* SPDX-License-Identifier: MIT */
/* HUDTerminal (MOST FBlock 82, instance 00, at the cluster) calls: function IDs, this car's 6WA instance numbers, and the
 * payloads, big-endian with the handle first. Every call is sent as StartResultAck (op 6); the cluster answers ResultAck
 * (op D). The encodings are checked against batches the cluster accepted in the car on 2026-11-18 (tests/test_pure.c).
 * Pure C. Protocol as documented by st1ng (nbtevo-carplay-advanced src/hud.h) and NBT-AAD (cluster/hudterm.hpp). */
#ifndef SL_HUD_H
#define SL_HUD_H

#include "sltypes.h"

enum {
    HUD_SET_VISIBILITY = 0x204,    /* [h][0|1] */
    HUD_SWITCH_VISIBILITY = 0x20E, /* [hide][show] */
    HUD_SET_TEXT = 0x210,          /* [h][00][UTF-16BE ...][0000] */
    HUD_SET_LINE_WIDTH = 0x219,    /* [h][w] */
    HUD_SET_FILL_COLOR = 0x21B,    /* [h][palette index] */
    HUD_ADD_POLYGON = 0x21D,       /* [h][item:2][type 3 = filled][count][x:2 y:2]...; stacked in send order */
    HUD_REMOVE_ALL = 0x220         /* [h] */
    /* not used: ImageRemoveItem 0x21F refused 3-, 4- and 5-byte payloads (Error 05, invalid length) in the car */
};
/* 6WA instances (this car): 1 the panel, 2 street, 3 distance, 4 unit (texts), 5 and 6 the picture (126 x 77) */
enum { HUD_FRAME = 1, HUD_STREET = 2, HUD_DIST = 3, HUD_UNIT = 4, HUD_BUF_A = 5, HUD_BUF_B = 6 };
enum { HUD_OP_CALL = 6, HUD_OP_ERRORACK = 9, HUD_OP_PROCESSINGACK = 0xA, HUD_OP_RESULTACK = 0xD, HUD_OP_ERROR = 0xF };

struct hud_call {
    u16 fkt;
    u8 n;
    u8 p[48];
};

void hud_vis(struct hud_call *c, u16 h, int on);
void hud_switch(struct hud_call *c, u16 hide, u16 show);
void hud_remove_all(struct hud_call *c, u16 h);
void hud_fill(struct hud_call *c, u16 h, u8 colour);
void hud_line_width(struct hud_call *c, u16 h, u8 w);
/* a filled rectangle as a 4-point polygon; coordinates are s16 */
void hud_rect(struct hud_call *c, u16 h, u16 item, int x0, int y0, int x1, int y1);
void hud_text_empty(struct hud_call *c, u16 h);
/* Two bars, x0..lx1 and rx0..x1 (rows y0..y1), as ONE filled 8-point shape joined by a zero-width slit along the top edge:
   (x0,y0) (x1,y0) (x1,y1) (rx0,y1) (rx0,y0) (lx1,y0) (lx1,y1) (x0,y1). The HUD fills it as two bars in one call, with a
   really faint hairline along the join (car, 2026-11-19). */
void hud_two_bars(struct hud_call *c, u16 h, u16 item, int x0, int lx1, int rx0, int x1, int y0, int y1);
/* The state the BMW navigation leaves at the end of a route (st1ng's drawClear): panel off, both pictures emptied, the
   three texts emptied, the street text hidden. Fills c[0..6]; returns 7. */
int hud_clear_seq(struct hud_call *c);
/* 1 when c is one of the calls above on a 6WA handle 1..6: nothing else is ever sent to the cluster */
int hud_call_ok(const struct hud_call *c);
/* "204 000101": the call as the test channel writes it, into b (at least 3 + 1 + 2 * 48 + 1 = 101 bytes) */
void hud_format(const struct hud_call *c, char *b);

#endif

/* SPDX-License-Identifier: MIT */
/* When the shift light may show (the menu's "Only in SPORT and SPORT+" and "Only in manual mode"), from the car data the
 * plugin writes (SLP_CAR) and the map of raw values (SLP_DIR/car-map). Pure C (host-tested).
 *
 * The raw values: driving.mode and driving.mode35up ('{"mode":n}', 15 and 63 not valid, NBT-AAD), driving.gear (its
 * first number). Which values mean SPORT / SPORT+ or manual is found in the car and written to car-map, one property
 * per line: "mode35up 3 4", "gear 5 6 7". A switch whose value or map is unknown lets the light show (it never hides
 * the light because of something not yet known). Manual mode: driving.shiftIndicator ("shift") when mapped, else
 * driving.gear (which on this car is 5 in D, S and M alike, 2026-11-19). */
#ifndef SL_GATE_H
#define SL_GATE_H

#define SL_MAP_MAX 12
#define SL_PROPS 6 /* mode, mode35up, gear, shift, nav, pwf */

struct sl_car {
    int v[SL_PROPS]; /* 0 driving.mode, 1 driving.mode35up, 2 driving.gear, 3 driving.shiftIndicator,
                        4 navigation.guidanceStatus, 5 vehicle.statePWF; -1 unknown */
};

struct sl_map {
    int n[SL_PROPS];               /* how many values each property has in the map (0: not mapped) */
    int v[SL_PROPS][SL_MAP_MAX];
};

void sl_car_parse(struct sl_car *c, const char *text);  /* "mode 3\nmode35up 63\ngear 4\nshift 2\nnav 0\npwf 4\n" */
void sl_map_parse(struct sl_map *m, const char *text);  /* "mode35up 3 4\ngear 5 6\n" ('#' comments) */
/* 1: the light may show. *why: a short reason (for the log) */
int sl_gate(const struct sl_car *c, const struct sl_map *m, int sport_only, int manual_only, const char **why);
/* the first decimal number in text (optionally negative); def when none */
int sl_text_int(const char *text, int def);

#endif

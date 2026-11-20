/* SPDX-License-Identifier: MIT */
/* Host tests (Mac) for src/gate.c: the car data and car-map files, and when the light may show.
 *   cc -Isrc tests/test_gate.c src/gate.c -o out/test_gate && out/test_gate */
#include <stdio.h>
#include <string.h>
#include "gate.h"

static int fails, checks;
#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static int gate(const char *car, const char *map, int sport, int manual, const char **why)
{
    struct sl_car c;
    struct sl_map m;
    sl_car_parse(&c, car);
    sl_map_parse(&m, map);
    return sl_gate(&c, &m, sport, manual, why);
}

int main(void)
{
    struct sl_car c;
    struct sl_map m;
    const char *why;

    /* the plugin's file */
    sl_car_parse(&c, "mode 3\nmode35up 63\ngear 4\n");
    CHECK(c.v[0] == 3 && c.v[1] == 63 && c.v[2] == 4, "car %d %d %d", c.v[0], c.v[1], c.v[2]);
    sl_car_parse(&c, "mode -1\nmode35up -1\ngear -1\n");
    CHECK(c.v[0] == -1 && c.v[1] == -1 && c.v[2] == -1, "unknown car %d %d %d", c.v[0], c.v[1], c.v[2]);
    sl_car_parse(&c, "");
    CHECK(c.v[0] == -1 && c.v[1] == -1 && c.v[2] == -1, "no file");
    sl_car_parse(&c, "gear 7");
    CHECK(c.v[2] == 7 && c.v[0] == -1, "no newline at the end");
    sl_car_parse(&c, "modes 4\nmode35upx 2\n");
    CHECK(c.v[0] == -1 && c.v[1] == -1, "other words are not properties");

    /* the map */
    sl_map_parse(&m, "# found 2026-11-xx\nmode35up 3 4\ngear 5 6 7\n");
    CHECK(m.n[0] == 0 && m.n[1] == 2 && m.n[2] == 3 && m.v[1][0] == 3 && m.v[1][1] == 4 && m.v[2][2] == 7,
          "map %d %d %d", m.n[0], m.n[1], m.n[2]);
    sl_map_parse(&m, "#mode 1 2\n");
    CHECK(m.n[0] == 0, "comment line");
    sl_map_parse(&m, "mode 1 2 3 4 5 6 7 8 9 10 11 12 13 14\n");
    CHECK(m.n[0] == SL_MAP_MAX, "at most %d values: %d", SL_MAP_MAX, m.n[0]);

    /* the gate: switches off -> always */
    CHECK(gate("mode35up 1\ngear 1\n", "mode35up 3 4\ngear 5\n", 0, 0, &why), "both switches off: %s", why);
    /* sport only */
    CHECK(gate("mode35up 3\n", "mode35up 3 4\n", 1, 0, &why), "SPORT: %s", why);
    CHECK(gate("mode35up 4\n", "mode35up 3 4\n", 1, 0, &why), "SPORT+: %s", why);
    CHECK(!gate("mode35up 2\n", "mode35up 3 4\n", 1, 0, &why) && !strcmp(why, "not a sport mode"), "COMFORT: %s", why);
    CHECK(gate("mode35up 63\nmode 3\n", "mode35up 3 4\nmode 3\n", 1, 0, &why), "mode35up not valid, mode is: %s", why);
    CHECK(!gate("mode35up 63\nmode 1\n", "mode35up 3 4\nmode 3\n", 1, 0, &why), "mode35up not valid, mode not sport");
    CHECK(gate("mode 15\n", "mode 3\n", 1, 0, &why) && strstr(why, "not known"), "mode 15 is not valid: %s", why);
    CHECK(gate("mode35up 2\n", "", 1, 0, &why) && strstr(why, "not known"), "no map: shows (%s)", why);
    CHECK(gate("", "mode35up 3\n", 1, 0, &why) && strstr(why, "not known"), "no value yet: shows (%s)", why);
    CHECK(!gate("mode 2\nmode35up 4\n", "mode 3\nmode35up 3\n", 1, 0, &why), "mode35up wins when both are mapped");
    /* manual only */
    CHECK(gate("gear 6\n", "gear 5 6 7\n", 0, 1, &why), "manual gear: %s", why);
    CHECK(!gate("gear 4\n", "gear 5 6 7\n", 0, 1, &why) && !strcmp(why, "not in manual mode"), "D: %s", why);
    CHECK(gate("gear 4\n", "mode35up 3\n", 0, 1, &why) && strstr(why, "manual mode not known"), "gear unmapped: %s",
          why);
    /* manual mode from driving.shiftIndicator (shift), which wins over gear when mapped */
    sl_car_parse(&c, "mode 3\nmode35up -1\ngear 5\nshift 9\n");
    CHECK(c.v[3] == 9 && c.v[2] == 5, "shift parsed: %d", c.v[3]);
    sl_car_parse(&c, "mode 4\nnav 1\npwf 8\n");
    CHECK(c.v[4] == 1 && c.v[5] == 8 && c.v[0] == 4 && c.v[3] == -1, "nav %d pwf %d", c.v[4], c.v[5]);
    CHECK(gate("gear 5\nshift 9\n", "shift 9 10\n", 0, 1, &why), "manual by shift: %s", why);
    CHECK(!gate("gear 5\nshift 0\n", "shift 9 10\n", 0, 1, &why) && !strcmp(why, "not in manual mode"), "D by shift");
    CHECK(!gate("gear 5\nshift 0\n", "gear 5\nshift 9\n", 0, 1, &why), "shift wins over gear");
    CHECK(gate("gear 5\nshift -1\n", "gear 5\nshift 9\n", 0, 1, &why), "shift unknown: gear decides");
    CHECK(gate("mode 4\ngear 5\nshift 9\n", "mode 4 5\nshift 9\n", 1, 1, &why), "SPORT + manual by shift");
    /* both */
    CHECK(gate("mode35up 3\ngear 6\n", "mode35up 3 4\ngear 6\n", 1, 1, &why), "SPORT and manual");
    CHECK(!gate("mode35up 3\ngear 4\n", "mode35up 3 4\ngear 6\n", 1, 1, &why), "SPORT but D");
    CHECK(!gate("mode35up 2\ngear 6\n", "mode35up 3 4\ngear 6\n", 1, 1, &why), "manual but COMFORT");

    /* numbers in text */
    CHECK(sl_text_int("5500\n", 0) == 5500 && sl_text_int("x", 7) == 7 && sl_text_int("a -3 b", 0) == -3 &&
              sl_text_int("", 9) == 9,
          "sl_text_int");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

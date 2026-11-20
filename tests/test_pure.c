/* SPDX-License-Identifier: MIT */
/* Host tests (Mac) for the pure modules: the HUD call encodings against batches the cluster accepted in the car on
 * 2026-11-18, the integer helpers, and the shift logic on a simulated ramp and on the car's rev-blip log.
 *   cc -Isrc tests/test_pure.c src/hud.c src/logic.c src/util.c -o out/test_pure && out/test_pure tests/data/obd-rev-blips.txt */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hud.h"
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

static void expect(const struct hud_call *c, const char *want)
{
    char b[128];
    hud_format(c, b);
    CHECK(!strcmp(b, want), "%s, want %s", b, want);
    CHECK(hud_call_ok(c), "%s should be allowed", b);
}

static void test_hud(void)
{
    struct hud_call c, seq[8];
    /* from the batches the cluster answered with op d (shiftlight/tools, 2026-11-18) */
    const char *clear[7] = { "204 000100", "220 0005", "220 0006", "210 0002000000", "210 0003000000",
                             "210 0004000000", "204 000200" };
    int i, n;
    hud_vis(&c, HUD_FRAME, 1);
    expect(&c, "204 000101");
    hud_vis(&c, HUD_BUF_B, 1);
    expect(&c, "204 000601");
    hud_remove_all(&c, HUD_BUF_B);
    expect(&c, "220 0006");
    hud_fill(&c, HUD_BUF_B, 8);
    expect(&c, "21b 000608");
    hud_line_width(&c, HUD_BUF_B, 0);
    expect(&c, "219 000600");
    hud_rect(&c, HUD_BUF_B, 0, 21, 0, 95, 5); /* the final preview's bar */
    expect(&c, "21d 00060000030400150000005f0000005f000500150005");
    hud_rect(&c, HUD_BUF_B, 0, 40, 20, 80, 63); /* st1ng's colour-probe square */
    expect(&c, "21d 00060000030400280014005000140050003f0028003f");
    hud_rect(&c, HUD_BUF_B, 0, 21, -8, 95, -3); /* the "above" test */
    expect(&c, "21d 0006000003040015fff8005ffff8005ffffd0015fffd");
    hud_text_empty(&c, HUD_DIST);
    expect(&c, "210 0003000000");
    hud_switch(&c, HUD_BUF_A, HUD_BUF_B);
    expect(&c, "20e 00050006");
    hud_two_bars(&c, HUD_BUF_B, 0, 21, 46, 70, 95, 0, 5); /* the "twobars" shape the car drew as two bars */
    expect(&c, "21d 00060000030800150000005f0000005f00050046000500460000002e0000002e000500150005");
    n = hud_clear_seq(seq);
    CHECK(n == 7, "clear sequence has %d calls", n);
    for (i = 0; i < n && i < 7; i++)
        expect(&seq[i], clear[i]);

    /* never anything else */
    hud_vis(&c, 7, 1);
    CHECK(!hud_call_ok(&c), "handle 7 allowed");
    hud_vis(&c, 0, 1);
    CHECK(!hud_call_ok(&c), "handle 0 allowed");
    hud_fill(&c, HUD_FRAME, 8);
    CHECK(!hud_call_ok(&c), "fill on the panel allowed");
    hud_text_empty(&c, HUD_DIST);
    c.p[3] = 0x31;
    c.p[4] = 0x00;
    CHECK(!hud_call_ok(&c), "a non-empty text allowed");
    hud_rect(&c, HUD_BUF_B, 0, 1, 1, 2, 2);
    c.p[4] = 2;
    CHECK(!hud_call_ok(&c), "a polyline allowed");
    hud_switch(&c, HUD_FRAME, HUD_BUF_B);
    CHECK(!hud_call_ok(&c), "switch from the panel allowed");
    hud_vis(&c, HUD_FRAME, 1);
    c.fkt = 0xC04;
    CHECK(!hud_call_ok(&c), "ImageGetSize allowed");
}

static void test_div(void)
{
    unsigned i;
    srand(7);
    for (i = 0; i < 200000; i++) {
        u32 n = (u32)rand() * 2654435761u, d = (u32)rand() % (i & 1 ? 100000u : 70u) + 1u;
        int a = (int)(n >> 1) * (i & 2 ? -1 : 1), b = (int)d * (i & 4 ? -1 : 1);
        if (sl_udiv(n, d) != n / d || sl_sdiv(a, b) != a / b) {
            CHECK(0, "div %u/%u or %d/%d", n, d, a, b);
            break;
        }
    }
    CHECK(sl_udiv(5, 0) == 0 && sl_sdiv(5, 0) == 0, "division by 0");
}

/* The car as measured: the DME holds rpm and refreshes it every 200 ms; a reading's reply comes 70 ms after the request
   (the value as it was 35 ms before the reply); readings back to back. Returns the time (ms) the bar was shown, -1 never. */
static double ramp(double from, double rate, int on, u32 lead, double phase_ms, double *cross_ms)
{
    struct sl_logic l;
    double t;
    sl_logic_init(&l, on, 250, lead, 300);
    *cross_ms = (on - from) / rate * 1000.0;
    for (t = 70; t < 5000; t += 70) {
        double sample = t - 35, refreshed = phase_ms + 200.0 * (int)((sample - phase_ms) / 200.0);
        int v = (int)(from + rate * (refreshed > 0 ? refreshed : 0) / 1000.0);
        if (sl_logic_feed(&l, (u32)t, v) && l.shown)
            return t;
    }
    return -1;
}

static void test_logic(void)
{
    struct sl_logic l;
    double cross, shown, early = 0, late = 0, sum = 0, ph;
    int k = 0;
    u32 t;

    sl_logic_init(&l, 3000, 250, 150, 300);
    for (t = 0; t < 5000; t += 70)
        CHECK(!sl_logic_feed(&l, t, 2000 + (int)(t % 3)), "steady 2000 rpm toggled at %u", t);
    CHECK(!l.shown, "steady 2000 rpm shown");

    /* hysteresis and the minimum on time */
    sl_logic_init(&l, 3000, 250, 150, 300);
    CHECK(sl_logic_feed(&l, 1000, 3100) && l.shown, "3100 rpm not shown");
    CHECK(!sl_logic_feed(&l, 1100, 2900) && l.shown, "2900 hid it (hysteresis 250)");
    CHECK(!sl_logic_feed(&l, 1200, 2700) && l.shown, "2700 hid it before 300 ms on");
    CHECK(sl_logic_feed(&l, 1320, 2700) && !l.shown, "2700 after 320 ms on did not hide it");

    /* a 1,300 rpm/s pull (2nd gear, full throttle, estimate) across every DME refresh phase */
    for (ph = 0; ph < 200; ph += 10) {
        shown = ramp(2000, 1300, 3000, 150, ph, &cross);
        CHECK(shown > 0, "ramp never shown (phase %.0f)", ph);
        if (shown < 0)
            continue;
        if (shown - cross < early)
            early = shown - cross;
        if (shown - cross > late)
            late = shown - cross;
        sum += shown - cross;
        k++;
    }
    printf("1300 rpm/s, on 3000: shown %.0f..%+.0f ms around the true crossing (mean %+.0f ms), before the HUD call\n",
           early, late, k ? sum / k : 0);
    /* with lead 150 ms it may be early by up to ~1 DME period, never later than ~1 reading after the crossing */
    CHECK(early >= -300, "shown %.0f ms early", early);
    CHECK(late <= 100, "shown %.0f ms late", late);

    /* without a lead it is later: the lead must buy time */
    {
        double s0 = 0, s1 = 0;
        int n0 = 0;
        for (ph = 0; ph < 200; ph += 10) {
            double c0, a = ramp(2000, 1300, 3000, 0, ph, &c0), b = ramp(2000, 1300, 3000, 150, ph, &c0);
            if (a > 0 && b > 0) {
                s0 += a - c0;
                s1 += b - c0;
                n0++;
            }
        }
        printf("lead 0: mean %+.0f ms; lead 150: mean %+.0f ms\n", n0 ? s0 / n0 : 0, n0 ? s1 / n0 : 0);
        CHECK(n0 && s1 < s0, "the lead does not make it earlier");
    }
}

/* the car's free-rev blips, read back (tests/data/obd-rev-blips.txt) */
static void test_replay(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    struct sl_logic l;
    int n = 0, shows = 0, hides = 0, rpm[400], i;
    double tt[400];
    if (!f) {
        CHECK(0, "cannot open %s", path);
        return;
    }
    while (fgets(line, sizeof line, f) && n < 400) {
        unsigned idx;
        double t, rt;
        int r;
        if (sscanf(line, " %u t %lf ms rt %lf ms rpm %d", &idx, &t, &rt, &r) == 4) {
            tt[n] = t;
            rpm[n] = r;
            n++;
        }
    }
    fclose(f);
    CHECK(n == 170, "%d readings in the log, want 170", n);
    sl_logic_init(&l, 2500, 250, 150, 300);
    for (i = 0; i < n; i++) {
        if (sl_logic_feed(&l, (u32)tt[i], rpm[i])) {
            int j, peak = 0;
            for (j = i; j < n && j < i + 6; j++)
                if (rpm[j] > peak)
                    peak = rpm[j];
            if (l.shown) {
                shows++;
                printf("  show at %7.1f ms: rpm %d, pred %d, slope %d rpm/s, peak of the next 6 readings %d\n", tt[i],
                       rpm[i], l.pred, l.slope, peak);
                CHECK(peak >= 2300, "false show at %.1f ms (peak %d)", tt[i], peak);
            } else {
                hides++;
                printf("  hide at %7.1f ms: rpm %d\n", tt[i], rpm[i]);
            }
        }
    }
    printf("replay, on 2500: %d shows, %d hides\n", shows, hides);
    CHECK(shows >= 1, "the blips above 2500 never showed it");
    CHECK(!l.shown, "still shown at the end (rpm %d)", rpm[n - 1]);
}

int main(int argc, char **argv)
{
    test_hud();
    test_div();
    test_logic();
    if (argc > 1)
        test_replay(argv[1]);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

/* SPDX-License-Identifier: MIT */
#include "gate.h"

static const char *const k_names[SL_PROPS] = { "mode", "mode35up", "gear", "shift", "nav", "pwf" };

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* a number at *pp (after spaces); 1 when one was read */
static int read_num(const char **pp, int *out)
{
    const char *p = *pp;
    int neg = 0, v = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (!is_digit(*p))
        return 0;
    while (is_digit(*p) && v < 1000000)
        v = v * 10 + (*p++ - '0');
    *out = neg ? -v : v;
    *pp = p;
    return 1;
}

/* the property index of the word at p (followed by a space or tab), -1 none; *len its length */
static int which(const char *p, int *len)
{
    int i, k;
    for (i = 0; i < SL_PROPS; i++) {
        const char *n = k_names[i];
        for (k = 0; n[k] && p[k] == n[k]; k++)
            ;
        if (!n[k] && (p[k] == ' ' || p[k] == '\t')) {
            *len = k;
            return i;
        }
    }
    return -1;
}

static const char *next_line(const char *p)
{
    while (*p && *p != '\n')
        p++;
    return *p ? p + 1 : p;
}

void sl_car_parse(struct sl_car *c, const char *text)
{
    const char *p = text;
    int i, len, v;
    for (i = 0; i < SL_PROPS; i++)
        c->v[i] = -1;
    while (p && *p) {
        i = which(p, &len);
        if (i >= 0) {
            const char *q = p + len;
            if (read_num(&q, &v))
                c->v[i] = v;
        }
        p = next_line(p);
    }
}

void sl_map_parse(struct sl_map *m, const char *text)
{
    const char *p = text;
    int i, len, v;
    for (i = 0; i < SL_PROPS; i++)
        m->n[i] = 0;
    while (p && *p) {
        i = *p == '#' ? -1 : which(p, &len);
        if (i >= 0) {
            const char *q = p + len;
            m->n[i] = 0;
            while (m->n[i] < SL_MAP_MAX && read_num(&q, &v))
                m->v[i][m->n[i]++] = v;
        }
        p = next_line(p);
    }
}

static int in_map(const struct sl_map *m, int i, int v)
{
    int k;
    for (k = 0; k < m->n[i]; k++)
        if (m->v[i][k] == v)
            return 1;
    return 0;
}

/* a value the car can send but that says "not valid" (NBT-AAD: 15 and 63) */
static int valid(int i, int v)
{
    return v >= 0 && !(i == 0 && v == 15) && !(i == 1 && v == 63);
}

int sl_gate(const struct sl_car *c, const struct sl_map *m, int sport_only, int manual_only, const char **why)
{
    const char *w = "on";
    if (sport_only) {
        int i, known = 0, ok = 0;
        for (i = 1; i >= 0; i--) /* mode35up first: whichever the car's variant sends and the map knows */
            if (valid(i, c->v[i]) && m->n[i]) {
                known = 1;
                ok = in_map(m, i, c->v[i]);
                break;
            }
        if (known && !ok) {
            *why = "not a sport mode";
            return 0;
        }
        if (!known)
            w = "on (drive mode not known yet)";
    }
    if (manual_only) {
        int k = valid(3, c->v[3]) && m->n[3] ? 3 : valid(2, c->v[2]) && m->n[2] ? 2 : -1;
        if (k >= 0) {
            if (!in_map(m, k, c->v[k])) {
                *why = "not in manual mode";
                return 0;
            }
        } else {
            w = "on (manual mode not known yet)";
        }
    }
    *why = w;
    return 1;
}

int sl_text_int(const char *text, int def)
{
    const char *p = text;
    int v;
    while (p && *p && !is_digit(*p) && !(*p == '-' && is_digit(p[1])))
        p++;
    return p && *p && read_num(&p, &v) ? v : def;
}

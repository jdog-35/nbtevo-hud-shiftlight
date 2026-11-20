#!/usr/bin/env python3
"""Stage changes in a drive log (tools/drive.sh log): every pull, how long each stage was up, and what the HUD showed
after each upshift (the fall from stage 3).

    python3 -I tools/drive_stats.py tests/data/drive-2026-11-19.txt
"""
import re
import sys

LINE = re.compile(r"^\s*([\d.]+) ms  stage (\d)  rpm (\d+) \((.)\)  pred (\d+)  slope (-?\d+) rpm/s")


def main(path):
    ev = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = LINE.match(line)
        if m:
            t, st, rpm, _, pred, slope = m.groups()
            ev.append((float(t), int(st), int(rpm), int(pred), int(slope)))

    # pulls: from leaving stage 0 to coming back to it
    pulls, cur = [], []
    for e in ev:
        cur.append(e)
        if e[1] == 0:
            pulls.append(cur)
            cur = []
    if cur:
        pulls.append(cur)

    print("Rises (first time each stage came up in a climb; ms between them; climb rate at stage 1):")
    rises = []
    for p in pulls:
        i = 0
        while i < len(p):
            # a climb: consecutive rising changes
            j = i
            while j + 1 < len(p) and p[j + 1][1] > p[j][1]:
                j += 1
            if j > i or p[i][1] > 0:
                seg = p[i:j + 1]
                if seg[-1][1] == 3 and len(seg) >= 2:
                    rises.append(seg)
            i = j + 1
    for seg in rises:
        t0 = seg[0][0]
        steps = "  ".join(f"s{e[1]}@{e[2]}" + (f" +{e[0] - prev[0]:.0f}" if k else "")
                          for k, (e, prev) in enumerate(zip(seg, [seg[0]] + seg[:-1])))
        print(f"  {t0 / 1000:7.1f} s  rate {seg[0][4]:5d} rpm/s  first->3 {seg[-1][0] - t0:5.0f} ms   {steps}")

    print("\nFalls from stage 3 (fast falls = upshifts): what showed after, for how long:")
    n_mid = 0
    n_fast = 0
    for k, e in enumerate(ev):
        if e[1] != 3 or k + 1 >= len(ev):
            continue
        nxt = ev[k + 1]
        if nxt[1] >= 3:
            continue
        fast = nxt[4] <= -3000
        n_fast += fast
        chain = [nxt]
        m = k + 2
        while m < len(ev) and ev[m][1] < chain[-1][1]:
            chain.append(ev[m])
            m += 1
        mid = [c for c in chain[:-1]]
        if fast and len(chain) > 1:
            n_mid += 1
        desc = " -> ".join(f"s{c[1]} (rpm {c[2]}, slope {c[4]})" for c in chain)
        held = f"   intermediate held {chain[1][0] - chain[0][0]:.0f} ms" if len(chain) > 1 else ""
        print(f"  {e[0] / 1000:7.1f} s  s3 -> {desc}{held}")
    print(f"\n  {n_fast} fast falls (slope <= -3000 rpm/s at the first lower stage); {n_mid} of them stepped down through "
          f"an intermediate stage")

    flashes = [(a, b) for a, b in zip(ev, ev[1:]) if a[1] == 1 and b[1] == 0 and b[0] - a[0] <= 600
               and (ev.index(a) == 0 or ev[ev.index(a) - 1][1] == 0)]
    print(f"\nStage 1 flashes (0 -> 1 -> 0 within 600 ms): {len(flashes)}")
    for a, b in flashes:
        print(f"  {a[0] / 1000:7.1f} s  rpm {a[2]} pred {a[3]} slope {a[4]}  ->  0 after {b[0] - a[0]:.0f} ms at rpm {b[2]}")


if __name__ == "__main__":
    main(sys.argv[1])


# ---- verbose logs (SL_VERBOSE=1, build 61a33a69 and later): every reading ------------------------------------------
READ = re.compile(r"^\s*([\d.]+) ms  rpm (\d+) \((.)\)  pred (\d+)  rate (-?\d+)  on (\d+)/(\d+)  rt [\d.]+ ms  "
                  r"\[stage (\d)\]( +falling)?")
EVT2 = re.compile(r"^\s*([\d.]+) ms  stage (\d)  rpm (\d+) \((.)\)  pred (\d+)  slope (-?\d+) rpm/s  rate (-?\d+)  "
                  r"on (\d+)/(\d+)  \(\d+ calls?\)( +(?:landed|shift))?")


def verbose(path):
    rd, ev = [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = READ.match(line)
        if m:
            t, rpm, _, pred, rate, on1, on2, st, fl = m.groups()
            rd.append((float(t), int(rpm), int(pred), int(rate), int(on1), int(on2), int(st), bool(fl)))
            continue
        m = EVT2.match(line)
        if m:
            t, st, rpm, _, pred, slope, rate, on1, on2, landed = m.groups()
            ev.append((float(t), int(st), int(rpm), int(pred), int(slope), int(rate), int(on1), int(on2), bool(landed)))
            rd.append((float(t), int(rpm), int(pred), int(rate), int(on1), int(on2), int(st), False))
    if not rd:
        return
    rd.sort()
    print(f"\nVerbose: {len(rd)} readings, {len(ev)} stage changes, {sum(e[8] for e in ev)} marked landed")

    # falls: runs of readings falling >= 1,500 rpm/s from >= 4,000 rpm
    print("\nFalls from >= 4,000 rpm (runs of readings falling >= 1,500 rpm/s): steepest step, drop, stages after")
    i, shifts, worst_other = 1, [], 0
    while i < len(rd):
        def sl(k):
            dt = rd[k][0] - rd[k - 1][0]
            return (rd[k][1] - rd[k - 1][1]) * 1000.0 / dt if dt > 0 else 0
        if sl(i) <= -1500 and rd[i - 1][1] >= 4000:
            j = i
            while j + 1 < len(rd) and sl(j + 1) <= -1500:
                j += 1
            t0, t1 = rd[i - 1][0], rd[j][0]
            steep = min(sl(k) for k in range(i, j + 1))
            drop = rd[i - 1][1] - rd[j][1]
            before = rd[i - 1][6]
            after = [e for e in ev if t0 < e[0] <= t1 + 1000]
            downs = []
            for e in after:
                if downs and e[1] > downs[-1][1]:
                    break
                if e[1] < (downs[-1][1] if downs else before):
                    downs.append(e)
            kind = "SHIFT" if steep <= -3000 else "lift "
            if steep > -3000:
                worst_other = min(worst_other, steep)
            seq = " -> ".join(f"{e[1]}{'L' if e[8] else ''}(+{e[0] - t0:.0f})" for e in downs)
            if before > 0 or downs:
                print(f"  {t0 / 1000:7.1f} s  {kind} {rd[i - 1][1]:5d} -> {rd[j][1]:5d} in {t1 - t0:4.0f} ms, steepest "
                      f"{steep:6.0f}  stage {before} -> {seq or 'none'}")
            if steep <= -3000:
                shifts.append((before, len(downs)))
            i = j + 1
        else:
            i += 1
    n_lit = [s for s in shifts if s[0] > 0]
    print(f"  shift-like falls: {len(shifts)}, {len(n_lit)} with a stage lit; of those with more than one down-step: "
          f"{sum(1 for s in n_lit if s[1] > 1)}. Steepest non-shift fall: {worst_other:.0f} rpm/s")

    # rises to stage 3: the rate when stage 1 came, the gaps
    print("\nRises (stage changes up to 3): rate at the first, rpm at each, ms between")
    k = 0
    while k < len(ev):
        if ev[k][1] > (ev[k - 1][1] if k else 0) and ev[k][1] < 3:
            j = k
            while j + 1 < len(ev) and ev[j + 1][1] > ev[j][1]:
                j += 1
            if ev[j][1] == 3:
                seg = ev[k:j + 1]
                parts = "  ".join(f"s{e[1]}@{e[2]}" + (f" +{e[0] - p[0]:.0f}" if n else "")
                                  for n, (e, p) in enumerate(zip(seg, [seg[0]] + seg[:-1])))
                print(f"  {seg[0][0] / 1000:7.1f} s  rate {seg[0][5]:5d}  on {seg[0][6]}/{seg[0][7]}   {parts}")
            k = j + 1
        else:
            k += 1


if __name__ == "__main__" and len(sys.argv) > 1:
    verbose(sys.argv[1])

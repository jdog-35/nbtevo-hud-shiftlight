"""Correlate MOST trace fields with polled rpm; list fields that change rarely (day/night candidates).
usage: python3 -I analyze_trace.py <trace.txt>"""
import re
import sys
from bisect import bisect_left

rec = re.compile(r"^\s*([\d.]+)\s+k(\d+) f([0-9a-f]+)\s+([0-9a-f]{4})>([0-9a-f]{4})\s+([0-9a-f]{2})\.([0-9a-f]{2})\s+([0-9a-f]{3}) op ([0-9a-f])\s+n(\d+)\s+([0-9a-f]*)\+?")
rpmre = re.compile(r"^\s*([\d.]+)\s+rpm (\d+)")
msgs, rpm = [], []
for line in open(sys.argv[1]):
    m = rpmre.match(line)
    if m:
        rpm.append((float(m.group(1)), int(m.group(2))))
        continue
    m = rec.match(line)
    if m:
        t = float(m.group(1))
        key = (m.group(4), m.group(6) + "." + m.group(7), m.group(8), m.group(9))
        data = bytes.fromhex(m.group(11)) if m.group(11) else b""
        msgs.append((t, key, data))

rt = [t for t, _ in rpm]


def rpm_at(t):
    i = bisect_left(rt, t)
    if i <= 0:
        return rpm[0][1]
    if i >= len(rpm):
        return rpm[-1][1]
    return rpm[i - 1][1]


def corr(xs, ys):
    n = len(xs)
    if n < 20:
        return 0.0
    mx, my = sum(xs) / n, sum(ys) / n
    sx = sum((x - mx) ** 2 for x in xs) ** 0.5
    sy = sum((y - my) ** 2 for y in ys) ** 0.5
    if sx == 0 or sy == 0:
        return 0.0
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / (sx * sy)


by = {}
for t, k, d in msgs:
    by.setdefault(k, []).append((t, d))

print("rpm readings:", len(rpm), "max", max(r for _, r in rpm))
print("\n== fields correlating with rpm (|r| > 0.8), 1- and 2-byte big-endian ==")
hits = []
for k, lst in by.items():
    if len(lst) < 20:
        continue
    n = min(len(d) for _, d in lst)
    for w in (1, 2):
        for off in range(0, n - w + 1):
            xs = [int.from_bytes(d[off:off + w], "big") for _, d in lst]
            if len(set(xs)) < 5:
                continue
            ys = [rpm_at(t) for t, _ in lst]
            r = corr(xs, ys)
            if abs(r) > 0.8:
                hits.append((abs(r), k, off, w, len(lst), min(xs), max(xs)))
for r, k, off, w, cnt, lo, hi in sorted(hits, reverse=True)[:25]:
    print(f"  r={r:.3f}  {k[0]} {k[1]} fkt {k[2]} op {k[3]}  bytes {off}..{off + w - 1}  n={cnt}  range {lo}..{hi}")

print("\n== fields that changed 1..6 times (day/night candidates), with change times ==")
for k, lst in sorted(by.items()):
    if len(lst) < 3:
        continue
    n = min(len(d) for _, d in lst)
    for off in range(n):
        vals = [d[off] for _, d in lst]
        ch = [(lst[i][0], vals[i - 1], vals[i]) for i in range(1, len(vals)) if vals[i] != vals[i - 1]]
        if 1 <= len(ch) <= 6:
            s = ", ".join(f"{t / 1000:.1f}s {a:02x}->{b:02x}" for t, a, b in ch)
            print(f"  {k[0]} {k[1]} fkt {k[2]} op {k[3]} byte {off} (n={len(lst)}): {s}")

print("\n== kinds seen only a few times (<= 6) ==")
for k, lst in sorted(by.items(), key=lambda kv: kv[1][0][0]):
    if len(lst) <= 6:
        times = ", ".join(f"{t / 1000:.1f}s" for t, _ in lst)
        print(f"  {k[0]} {k[1]} fkt {k[2]} op {k[3]} x{len(lst)}: {times}  first {lst[0][1].hex()}")

"""CD0 broadcast vs polled OBD rpm: update rate, value changes, and which one leads (cross-correlation over lags).
usage: python3 -I cd0_lag.py <trace.txt>"""
import re
import sys
from bisect import bisect_right

cd0, obd = [], []
for line in open(sys.argv[1]):
    m = re.match(r"^\s*([\d.]+)\s+k2 f0\s+5000>2021\s+05\.00\s+cd0 op c\s+n2\s+([0-9a-f]{4})", line)
    if m:
        cd0.append((float(m.group(1)), int(m.group(2), 16) / 4))
        continue
    m = re.match(r"^\s*([\d.]+)\s+rpm (\d+)", line)
    if m:
        obd.append((float(m.group(1)), int(m.group(2))))

print(f"CD0 to 2021: {len(cd0)} messages in {(cd0[-1][0] - cd0[0][0]) / 1000:.1f} s")
gaps = sorted(cd0[i][0] - cd0[i - 1][0] for i in range(1, len(cd0)))
print(f"  spacing ms: min {gaps[0]:.0f}, median {gaps[len(gaps) // 2]:.0f}, max {gaps[-1]:.0f}")
changes = sum(1 for i in range(1, len(cd0)) if cd0[i][1] != cd0[i - 1][1])
print(f"  value changed in {changes} of {len(cd0) - 1} steps")
oc = sum(1 for i in range(1, len(obd)) if obd[i][1] != obd[i - 1][1])
print(f"OBD: {len(obd)} readings, value changed in {oc} of {len(obd) - 1}")


def at(series, t):
    ts = [x for x, _ in series]
    i = bisect_right(ts, t) - 1
    return series[max(i, 0)][1]


# the blips: where OBD rpm is above 1000
lo = min(t for t, r in obd if r > 1000) - 1500
hi = max(t for t, r in obd if r > 1000) + 1500
grid = [lo + 10 * k for k in range(int((hi - lo) / 10))]
best = None
for lag in range(-400, 401, 10):  # positive lag: CD0 at t compared with OBD at t + lag (CD0 ahead)
    xs = [at(cd0, t) for t in grid]
    ys = [at(obd, t + lag) for t in grid]
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    c = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    c /= (sum((x - mx) ** 2 for x in xs) * sum((y - my) ** 2 for y in ys)) ** 0.5
    if best is None or c > best[0]:
        best = (c, lag)
print(f"best match: CD0 is {best[1]:+d} ms ahead of the OBD reading (r={best[0]:.3f})")
print("around the highest blip (t ms: CD0 / OBD):")
peak_t = max(obd, key=lambda x: x[1])[0]
for t, v in cd0:
    if peak_t - 900 <= t <= peak_t + 300:
        print(f"  {t:9.1f}  CD0 {v:6.0f}   OBD {at(obd, t):5d}")

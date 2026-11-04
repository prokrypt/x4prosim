#!/usr/bin/env python3
"""Line up the frontlight with the panel for every flashing refresh (flash duck tuning).

usage: duckmeasure.py trace.log [min_dip]
trace.log: QEMU -D output with -d trace:esp32s3_ledc_duty,trace:uc8179_frame (all
times are virtual, so the firmware's millis() line up with them).

A refresh flashes when its panel mean (0 black .. 1 white) dips at least min_dip
(default 0.15) below both its start and its end, or rises that far above both
(Night Mode: the dark background swings white). Its dark window runs from the
first to the last frame below half that dip (each side against its own level).
The light is the sum of all LEDC channels' duty fractions (PWM duty ~ light).
Per flash: light at the dark start and its mean over the window, as % of the
light before the duck; "lit ms" = window time weighted by how far the light is
above its lowest level (0 = dark the whole flash); "early ms" = light below half
before the window starts (the dim shows on a still screen); "late ms" = from the
window's end until the light is back above half (negative: up while still dark)."""
import bisect, re, sys

ledc, frames = {}, []
light = []      # (t_us, total duty fraction) after each change
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"esp32s3_ledc_duty t_us=(\d+) ch=(\d+) level=(\d+) max=(\d+)", line)
    if m:
        t, ch, lv, mx = map(int, m.groups())
        ledc[ch] = lv / mx
        light.append((t, sum(ledc.values())))
        continue
    m = re.search(r"uc8179_frame t_us=(\d+) frame=(-?\d+) white_e4=(\d+)", line)
    if m:
        frames.append((int(m[1]), int(m[2]), int(m[3]) / 1e4))
min_dip = float(sys.argv[2]) if len(sys.argv) > 2 else 0.15
light_t = [t for t, _ in light]


def light_at(t):
    i = bisect.bisect_right(light_t, t)
    return light[i - 1][1] if i else 0.0


def first_below_half(t0, t1, ref, low, step=1000):
    """First time in [t0, t1) (1 ms steps) the light is below half way from ref to low."""
    half = (ref + low) / 2
    for t in range(t0, t1, step):
        if light_at(t) < half:
            return t
    return None


refreshes, cur = [], None
for t, f, w in frames:
    if f == -1:
        cur = [(t, w)]
        refreshes.append(cur)
    elif cur is not None:
        cur.append((t, w))

print("start_ms   dip  dark_ms  light@dark  mean  lit_ms  early_ms  late_ms")
for r in refreshes:
    ws = [w for _, w in r]
    dip = min(ws[0], ws[-1]) - min(ws)
    peak = max(ws) - max(ws[0], ws[-1])
    if peak > dip:      # Night Mode: the dark background swings white; mirror it into a dip
        r = [(t, -w) for t, w in r]
    before, after = r[0][1], r[-1][1]
    low = min(w for _, w in r)
    if before - low < min_dip or after - low < min_dip:
        continue
    t_low = next(t for t, w in r if w == low)
    start = next(t for t, w in r if w < before - (before - low) / 2)
    end = max(t for t, w in r if t >= t_low and w < after - (after - low) / 2)
    ref = light_at(r[0][0] - 1_000_000)     # before any duck of this refresh
    lo = min([lv for t, lv in light if start - 2_000_000 <= t <= end + 2_000_000] + [light_at(start)])
    span = ref - lo
    if ref == 0:
        print(f"{r[0][0] / 1e3:9.0f}  {before - low:.2f}  {(end - start) / 1e3:6.0f}  light off")
        continue
    # Light over the window, sampled per ms.
    samples = [light_at(t) for t in range(start, end + 1, 1000)]
    mean = sum(samples) / len(samples)
    lit = sum((s - lo) / span for s in samples) if span > 0 else len(samples)
    dim = first_below_half(start - 2_000_000, end, ref, lo) if span > 0 else None
    early = (start - dim) / 1e3 if dim is not None else float("nan")
    up = next((t for t in range(end, end + 3_000_000, 1000) if light_at(t) >= (ref + lo) / 2), None)
    late = (up - end) / 1e3 if up is not None else float("nan")
    print(f"{r[0][0] / 1e3:9.0f}  {before - low:.2f}  {(end - start) / 1e3:6.0f}  "
          f"{100 * light_at(start) / ref:8.0f}%  {100 * mean / ref:3.0f}%  {lit:6.0f}  {early:8.0f}  {late:7.0f}")

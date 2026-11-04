#!/usr/bin/env python3
"""Light-sleep residency and wake causes from an RTC_CNTL trace (battery work without device logs).

usage: sleepmeasure.py trace.log [from_s [to_s]]
trace.log: QEMU -D output with -d trace:esp32s3_rtc_sleep,trace:esp32s3_rtc_wake (virtual
times, so they line up with the firmware's millis()). from_s/to_s limit it to a window.

Prints the light-sleep count, the share of guest time asleep (residency), sleep lengths,
wakes per cause, and the most common gaps between wakes: a periodic waker (a 1 s loop
tick, a 250 ms select) shows up as a peak at its period. Deep sleeps are listed one by one
(slept time is host time, the guest clock stops)."""
import collections, re, sys

CAUSE = {0: "none", 1: "ext0", 2: "ext1", 4: "gpio", 8: "timer"}
lo = float(sys.argv[2]) * 1e6 if len(sys.argv) > 2 else 0
hi = float(sys.argv[3]) * 1e6 if len(sys.argv) > 3 else float("inf")
first = last = start = None
sleeps, wakes, deep = [], [], []    # light sleep lengths (us), light wake (t, cause), deep (t, cause, slept)
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"esp32s3_rtc_(sleep|wake) t_us=(\d+) deep=(\d) \w+=0x([0-9a-f]+) \w+=(-?\d+)", line)
    if not m:
        continue
    kind, t, is_deep, x, v = m[1], int(m[2]), m[3] == "1", int(m[4], 16), int(m[5])
    if not lo <= t <= hi:
        continue
    first = t if first is None else first
    last = t
    if kind == "sleep":
        start = t
    elif is_deep:
        deep.append((t, CAUSE.get(x, hex(x)), v))
    elif start is not None:     # a wake whose sleep began before the window doesn't count
        sleeps.append(v)
        wakes.append((t, CAUSE.get(x, hex(x))))
if first is None:
    sys.exit("no esp32s3_rtc_sleep/wake lines (run with -d trace:esp32s3_rtc_*)")

span = max(last - first, 1)
asleep = sum(sleeps)
print(f"window {first / 1e6:.1f}-{last / 1e6:.1f} s guest ({span / 1e6:.1f} s)")
print(f"light sleeps {len(sleeps)}  asleep {asleep / 1e3:.0f} ms  residency {100 * asleep / span:.1f}%  "
      f"wakes/s {len(sleeps) / (span / 1e6):.2f}")
if sleeps:
    s = sorted(sleeps)
    print(f"sleep ms  median {s[len(s) // 2] / 1e3:.1f}  p90 {s[len(s) * 9 // 10] / 1e3:.1f}  max {s[-1] / 1e3:.1f}")
    print("wakes by cause  " + "  ".join(f"{c} {n}" for c, n in collections.Counter(c for _, c in wakes).most_common()))
    gaps = collections.Counter(round((b - a) / 1e4) * 10 for (a, _), (b, _) in zip(wakes, wakes[1:]))
    print("top wake gaps (ms x count)  " + "  ".join(f"{g}x{n}" for g, n in gaps.most_common(8)))
for t, c, slept in deep:
    print(f"deep sleep woke at {t / 1e6:.1f} s by {c} after {slept / 1e6:.1f} s host")

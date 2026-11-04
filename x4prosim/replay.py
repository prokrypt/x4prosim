#!/usr/bin/env python3
"""Replay an input journal (gt911_input / x4pro_key trace lines) into a running instance.

usage: replay.py rec.trace [NAME]
Record with: x4prosim/run.sh flash.bin sd.img -d trace:gt911_input,trace:x4pro_key -D rec.trace
Events fire at their recorded guest time (board state uptime_us) when the replay starts
early enough, e.g. from a fresh boot; otherwise the whole journal shifts so the first
event fires now. Spacing stays in guest time either way."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import x4qmp

LINE = re.compile(r"\b(gt911_input|x4pro_key) t_us=(\d+) (.*)")


def parse(path):
    for line in open(path):
        m = LINE.search(line)
        if m:
            kv = dict(p.split("=", 1) for p in m.group(3).split())
            yield int(m.group(2)), m.group(1), kv


def main():
    events = list(parse(sys.argv[1]))
    if not events:
        sys.exit(f"no gt911_input/x4pro_key lines in {sys.argv[1]}")
    d = x4qmp.find(sys.argv[2] if len(sys.argv) > 2 else None)
    host0 = time.monotonic()
    try:
        x4qmp.state(d)
        now = lambda: x4qmp.state(d)["uptime_us"]
    except RuntimeError:
        print("replay: no board state property; spacing follows host time", file=sys.stderr)
        now = lambda: int((time.monotonic() - host0) * 1e6)
    shift = max(0, now() - events[0][0])
    home = False
    for t, kind, kv in events:
        while now() < t + shift:
            time.sleep(0.002)
        if kind == "x4pro_key":
            x4qmp.qom_set(d, "/machine/x4pro-keys", kv["key"], kv["down"] == "1")
        else:
            if (kv["home"] == "1") != home:
                home = kv["home"] == "1"
                x4qmp.qom_set(d, "/machine/gt911", "home", home)
            x4qmp.qom_set(d, "/machine/gt911", "touch", f"{kv['x']},{kv['y']}" if kv["touch"] == "1" else "up")
        print(f"{(t + shift) / 1e6:9.3f} {kind} {' '.join(f'{k}={v}' for k, v in kv.items())}")


if __name__ == "__main__":
    main()

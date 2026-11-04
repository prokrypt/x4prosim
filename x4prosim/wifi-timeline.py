#!/usr/bin/env python3
"""Wi-Fi connect timeline from CrossDink logs ("[ms] [LVL] [TAG] ..."), side by side.

usage: wifi-timeline.py LOG [LOG...]     e.g. a sim log and a device log of the same flow
Each milestone is ms after the first join attempt in that log (the first one only), so a sim
artefact (NTP blocked 5 s, no RF delays) stands apart from firmware latency."""
import re, sys

MILESTONES = [
    ("join", r"\[WIFI\] Connecting to ssid=|wifi remote: rejoining|WiFi\.begin returned"),
    ("assoc", r"STA event: connected to AP"),
    ("got IP", r"STA event: got IP"),
    ("NTP start", r"Starting NTP sync"),
    ("NTP fail", r"NTP sync failed"),  # a successful sync logs nothing
    ("server up", r"server running on port 80"),
    ("first HTTP", r"\[WEB\] (Connection fd|GET |POST |Request)"),
]
STAMP = re.compile(r"^\[(\d+)\]")


def timeline(path):
    found, t0 = {}, None
    for line in open(path, errors="replace"):
        m = STAMP.match(line)
        if not m:
            continue
        ms = int(m.group(1))
        for name, rx in MILESTONES:
            if name not in found and re.search(rx, line):
                if name == "join":
                    t0 = ms
                if t0 is not None:
                    found[name] = ms - t0
    return found


def main(paths):
    cols = [timeline(p) for p in paths]
    print(f"{'ms after join':14}" + "".join(f"{p[-28:]:>30}" for p in paths))
    for name, _ in MILESTONES:
        print(f"{name:14}" + "".join(f"{str(c.get(name, '-')):>30}" for c in cols))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])

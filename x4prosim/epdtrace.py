#!/usr/bin/env python3
"""Diff panel command streams: device log vs x4prosim trace.

usage: epdtrace.py device.log sim.trace [--from-cmd 0x00] [--count N]

device.log: console capture of a firmware built with -DFREEINK_EPD_TRACE=1
  ("[EPD] c=XX len=N b0 .. b7" lines). sim.trace: x4prosim run with
  QEMU_EXTRA="-d trace:uc8179_cmd -D sim.trace" (uc8179_cmd lines).
Prints both opcode/length sequences side by side and the first divergence;
exits 1 if the streams differ within --count commands.

Adapted from kycube/x4pro-emu tools/epdtrace.py:
  Copyright (c) 2026 KFloyd.com, MIT License.
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
"""
import argparse, re, sys

DEV = re.compile(r'\[EPD\] c=([0-9A-Fa-f]{2}) len=(\d+)((?: [0-9A-Fa-f]{2})*)')
SIM = re.compile(r'uc8179_cmd t_us=\d+ cmd=0x([0-9a-f]{2}) len=(\d+) data=([0-9a-f]*)')


def load(path):
    out = []
    for line in open(path, errors='replace'):
        m = DEV.search(line)
        if m:
            out.append((int(m.group(1), 16), int(m.group(2)), m.group(3).strip().lower()))
            continue
        m = SIM.search(line)
        if m:
            h = m.group(3)
            out.append((int(m.group(1), 16), int(m.group(2)), ' '.join(h[i:i + 2] for i in range(0, len(h), 2))))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('device')
    ap.add_argument('sim')
    ap.add_argument('--from-cmd', help='start both streams at the first occurrence of this opcode (e.g. 0x00)')
    ap.add_argument('--count', type=int, default=60)
    a = ap.parse_args()
    d, e = load(a.device), load(a.sim)
    if a.from_cmd:
        fc = int(a.from_cmd, 16)
        d = d[next((i for i, x in enumerate(d) if x[0] == fc), len(d)):]
        e = e[next((i for i, x in enumerate(e) if x[0] == fc), len(e)):]
    first = None
    for i in range(min(a.count, max(len(d), len(e)))):
        x = d[i] if i < len(d) else None
        y = e[i] if i < len(e) else None
        same = x is not None and y is not None and x[:2] == y[:2]
        if not same and first is None:
            first = i
        fx = f"{x[0]:02X} len={x[1]:<6} {x[2]}" if x else '-'
        fy = f"{y[0]:02X} len={y[1]:<6} {y[2]}" if y else '-'
        print(f"{'  ' if same else '!!'} {i:3d}  dev: {fx:40s} sim: {fy}")
    print(f"device: {len(d)} commands, sim: {len(e)} commands; opcode sequences equal: "
          f"{[x[0] for x in d] == [y[0] for y in e]}")
    if first is not None:
        print(f"first divergence at index {first}")
        sys.exit(1)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Compare a screenshot with a golden, pixel by pixel, outside masked boxes.

usage: shotdiff.py [--bw] [--max N] [--diff OUT.png] [--mask X,Y,W,H]... shot golden
shot and golden: PNG/PPM from `screendump ... panel`, or a device capture: the 480x800
BMP the reader saves, or the PBM/PGM from /api/screenshot or CMD:SCREENSHOT. A golden in
the other orientation is turned to whichever quarter turn matches best.
Masks are boxes in shot coordinates (clock, battery, ...); GOLDEN.mask next to the golden
adds more, one X,Y,W,H per line (# comments). --bw compares black/white (threshold 128),
for device captures against the emulator's gray levels. Prints the differing pixel count
and their bounding box; --diff writes a picture of them (red) over a faded golden, masks
blue. Exit 0 when at most N (default 0) pixels differ, 1 otherwise, 2 if the sizes can't
match. Needs Pillow."""
import argparse, os, sys
from PIL import Image, ImageChops, ImageDraw


def boxes(args):
    out = list(args.mask)
    mf = os.path.splitext(args.golden)[0] + ".mask"
    if os.path.exists(mf):
        out += [l.split("#")[0].strip() for l in open(mf)]
    return [tuple(int(v) for v in b.split(",")) for b in out if b]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("shot")
    ap.add_argument("golden")
    ap.add_argument("--mask", action="append", default=[], metavar="X,Y,W,H")
    ap.add_argument("--max", type=int, default=0)
    ap.add_argument("--diff")
    ap.add_argument("--bw", action="store_true")
    args = ap.parse_args()
    a, g = Image.open(args.shot).convert("L"), Image.open(args.golden).convert("L")
    if args.bw:
        a, g = (im.point(lambda v: 255 if v >= 128 else 0) for im in (a, g))
    mask = Image.new("L", a.size, 255)    # 255 = compared
    d = ImageDraw.Draw(mask)
    for x, y, w, h in boxes(args):
        d.rectangle([x, y, x + w - 1, y + h - 1], fill=0)

    def diff(gi):
        m = ImageChops.difference(a, gi).point(lambda v: 255 if v else 0)
        m = ImageChops.multiply(m, mask)
        return m.histogram()[255], m

    turns = [g] if g.size == a.size else [g.transpose(t) for t in (Image.ROTATE_90, Image.ROTATE_270)
                                           if g.size[::-1] == a.size]
    if not turns:
        print(f"{args.shot}: size {a.size} vs golden {g.size}", file=sys.stderr)
        return 2
    n, m, g = min((diff(gi) + (gi,) for gi in turns), key=lambda r: r[0])
    bbox = m.getbbox()
    print(f"{args.shot}: {n} px differ" + (f", box {bbox[0]},{bbox[1]} to {bbox[2] - 1},{bbox[3] - 1}" if bbox else ""))
    if args.diff:
        out = Image.merge("RGB", [g.point(lambda v: 160 + v * 95 // 255)] * 3)
        out.paste((90, 140, 255), mask=ImageChops.invert(mask).point(lambda v: 96 if v else 0))
        out.paste((230, 0, 0), mask=m)
        out.save(args.diff)
    return 0 if n <= args.max else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Make an SD image like a real card: MBR, one FAT32 (LBA) partition at 1 MiB.
usage: mksd.py [--force] sd.img [size_mb] [dir_to_copy ...]
size_mb must be a power of 2 (QEMU's sd-card refuses other sizes). Each dir's
contents land in the card root; an existing image is kept unless --force."""
import argparse, os, struct, subprocess, sys
ap = argparse.ArgumentParser(usage=__doc__.splitlines()[1].removeprefix("usage: "))
ap.add_argument("--force", action="store_true")
ap.add_argument("img")
ap.add_argument("mb", nargs="?", type=int, default=1024)
ap.add_argument("dirs", nargs="*")
a = ap.parse_args()
if a.mb < 64 or a.mb & (a.mb - 1):    # under 64 MB FAT32 has <65525 clusters and SdFat reads it as FAT16
    sys.exit(f"mksd.py: size {a.mb} MB must be a power of 2, at least 64")
if os.path.exists(a.img) and not a.force:
    sys.exit(f"mksd.py: {a.img} exists (--force to overwrite)")
total = a.mb * 2048
start = 2048
with open(a.img, "wb") as f:
    f.truncate(total * 512)
    entry = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff", start, total - start)
    f.seek(446); f.write(entry); f.seek(510); f.write(b"\x55\xaa")
subprocess.run(["mkfs.vfat", "-F", "32", "--offset", str(start), a.img, str((total - start) // 2)],
               check=True, stdout=subprocess.DEVNULL)
for d in a.dirs:
    # mtools copies a host folder onto the image (e.g. a books/ tree); -m keeps mtimes, -Q stops at the first error.
    names = [os.path.join(d, n) for n in os.listdir(d)]
    if names:
        subprocess.run(["mcopy", "-s", "-m", "-Q", "-i", f"{a.img}@@{start * 512}", *names, "::"], check=True)

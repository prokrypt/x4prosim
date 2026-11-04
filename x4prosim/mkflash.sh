#!/bin/sh
# Merge a PlatformIO or ESP-IDF build (bootloader, partition table, app) into a 16 MB flash image.
# usage: x4prosim/mkflash.sh [--factory] [--base IMG] <build dir> out.bin
#   pio: .pio/build/<env>   idf: build (bootloader/ and partition_table/ subfolders are found too)
#   --factory (S3): the X4 Pro factory partition table + an otadata booting app0, what the device
#     runs (mkfactory.py), instead of the build's partitions.bin
#   --base IMG: write the pieces over a copy of IMG (e.g. a run's flash.bin.run) instead of an
#     erased 16 MB, so NVS, otadata and the other app slot survive an app update. IMG may be out.bin.
# The chip comes from the bootloader header (ESP32-S3 or ESP32-C3). For the C3 (X3/X4 image) the
# NVS partition (0x9000, the Arduino layout) is seeded with CrossPoint's cached device type
# cphw/dev_det=2 (X3), what the firmware writes after its first boot: CrossPoint up to 1.6.5
# deadlocks on a first boot that runs the X3 fingerprint probe (it uses Wire before Wire.begin).
set -e
here=$(cd "$(dirname "$0")" && pwd)
factory=; base=
while :; do case $1 in --factory) factory=1; shift;; --base) base=$2; shift 2;; *) break;; esac; done
b=$1; out=$2
find1() { find "$b" -maxdepth 2 -name "$1" | head -1; }
boot=$(find1 bootloader.bin); part=$(find1 "partition*.bin")
app=$b/firmware.bin
[ -f "$app" ] || app=$(find "$b" -maxdepth 1 -name "*.bin" ! -name "bootloader.bin" ! -name "partition*.bin" | head -1)
[ -f "$boot" ] && [ -f "$part" ] && [ -f "$app" ] || { echo "need bootloader.bin, partition*.bin and an app .bin in $b" >&2; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
chip=$(od -An -tu1 -j12 -N1 "$boot" | tr -d ' ')
set -- 0x0 "$boot" 0x8000 "$part"
if [ "$chip" = 5 ]; then
  esp=esp32c3
  # a --base image already holds the firmware's own NVS
  [ -n "$base" ] || { python3 "$here/mknvs.py" "$tmp/nvs.bin" 0x5000 cphw dev_det=2; set -- "$@" 0x9000 "$tmp/nvs.bin"; }
else
  esp=esp32s3
  [ -z "$factory" ] || { python3 "$here/mkfactory.py" "$tmp/part.bin" "$tmp/ota.bin"; set -- 0x0 "$boot" 0x8000 "$tmp/part.bin" 0xe000 "$tmp/ota.bin"; }
fi
set -- "$@" 0x10000 "$app"
if [ -n "$base" ]; then
  [ "$base" -ef "$out" ] || cp "$base" "$out"
  while [ $# -gt 0 ]; do dd if="$2" of="$out" bs=4096 seek=$(($1 / 4096)) conv=notrunc status=none; shift 2; done
else
  python3 -m esptool --chip $esp merge_bin --fill-flash-size 16MB -o "$out" "$@"
fi

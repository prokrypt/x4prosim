#!/bin/sh
# Merge a PlatformIO or ESP-IDF build (bootloader, partition table, app) into a 16 MB flash image.
# usage: x4prosim/mkflash.sh <build dir> out.bin
#   pio: .pio/build/<env>   idf: build (bootloader/ and partition_table/ subfolders are found too)
set -e
b=$1
find1() { find "$b" -maxdepth 2 -name "$1" | head -1; }
boot=$(find1 bootloader.bin); part=$(find1 "partition*.bin")
app=$b/firmware.bin
[ -f "$app" ] || app=$(find "$b" -maxdepth 1 -name "*.bin" ! -name "bootloader.bin" ! -name "partition*.bin" | head -1)
[ -f "$boot" ] && [ -f "$part" ] && [ -f "$app" ] || { echo "need bootloader.bin, partition*.bin and an app .bin in $b" >&2; exit 1; }
python3 -m esptool --chip esp32s3 merge_bin --fill-flash-size 16MB -o "$2" 0x0 "$boot" 0x8000 "$part" 0x10000 "$app"

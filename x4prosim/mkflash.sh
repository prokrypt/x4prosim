#!/bin/sh
# Merge a CrossDink pio build into a 16 MB flash image.
# usage: x4prosim/mkflash.sh <CrossDink>/.pio/build/x4-pro-debug out.bin
set -e
b=$1
python3 -m esptool --chip esp32s3 merge_bin --fill-flash-size 16MB -o "$2" \
  0x0 "$b/bootloader.bin" 0x8000 "$b/partitions.bin" 0x10000 "$b/firmware.bin"

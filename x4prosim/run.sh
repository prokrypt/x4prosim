#!/bin/sh
# Boot an X4 Pro flash image in x4prosim. USB-CDC console (the firmware log) -> stdout.
# usage: x4prosim/run.sh flash.bin [sd.img] [extra qemu args...]
# Monitor: unix socket /tmp/x4prosim-mon.sock (HMP). GDB: add "-s -S".
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
img=$1; sd=${2:-sd.img}; shift; [ $# -gt 0 ] && shift
[ -f "$sd" ] || { truncate -s 1G "$sd"; mkfs.vfat -F 32 "$sd" >/dev/null; }
# QEMU writes to the flash image (NVS, OTA data); run a copy to keep the source clean.
cp "$img" "$img.run"
exec "$here/build/qemu-system-xtensa" -nographic -machine esp32s3 -m 8M \
  -global ssi_psram.is_octal=true \
  -drive file="$img.run",if=mtd,format=raw \
  -drive file="$sd",if=sd,format=raw \
  -chardev stdio,id=cdc,mux=off -serial null \
  -global driver=misc.esp32s3.usb_serial_jtag,property=chardev,value=cdc \
  -monitor unix:/tmp/x4prosim-mon.sock,server,nowait "$@"

#!/bin/sh
# Boot an X4 Pro flash image in x4prosim. USB-CDC console (the firmware log) -> stdout.
# usage: x4prosim/run.sh flash.bin [sd.img] [extra qemu args...]
# Wi-Fi: joins fake AP "PICSimLabWifi" -> QEMU user net (DHCP); host :8080 -> device :80. X4NET overrides.
# Monitor: unix socket /tmp/x4prosim-mon.sock (HMP). GDB: add "-s -S".
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
img=$1; sd=${2:-sd.img}; shift; [ $# -gt 0 ] && shift
[ -f "$sd" ] || python3 "$here/x4prosim/mksd.py" "$sd"
# QEMU writes to the flash image (NVS, OTA data); run a copy to keep the source clean.
cp "$img" "$img.run"
# -icount: guest time follows instructions (~240 MHz), not host speed, so light-sleep timing can't overshoot.
exec "$here/build/qemu-system-xtensa" -machine x4pro -icount shift=2,sleep=on \
  -drive file="$img.run",if=mtd,format=raw \
  -drive file="$sd",if=sd,format=raw \
  -chardev stdio,id=cdc,mux=off -serial null \
  -global driver=misc.esp32s3.usb_serial_jtag,property=chardev,value=cdc \
  -monitor unix:/tmp/x4prosim-mon.sock,server,nowait -display sdl,show-cursor=on \
  ${X4NET--nic user,model=esp32_wifi,hostfwd=tcp::8080-:80} "$@"

#!/bin/sh
# Boot an X4 Pro or X3 flash image in x4prosim. USB-CDC console (the firmware log) -> stdout.
# usage: x4prosim/run.sh flash.bin [sd.img] [extra qemu args...]
# The machine follows the image's chip (byte 12 of the bootloader header): ESP32-S3 -> x4pro,
# ESP32-C3 -> x3. X4MACHINE=x3|x4pro overrides.
# Wi-Fi: the fake AP "PICSimLabWifi" is bridged to a QEMU NIC.
#   X4BR=br0: on your LAN (router DHCP); needs "allow br0" in /usr/local/etc/qemu/bridge.conf, run as root.
#   default: QEMU NAT (10.0.2.15), host :8080 -> device :80. X4NET overrides both.
# SD: X4SD=snapshot discards the run's SD writes (cold caches every run); X4SD=none boots with no card.
# Instance dir /tmp/x4prosim/$X4NAME (default: this shell's pid): mon.sock (HMP), qmp.sock (QMP,
# for watch.py/replay.py), log.txt (copy of the firmware log). Several sims run side by side.
# GDB: add "-s -S".
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
img=$1; sd=${2:-sd.img}; shift; [ $# -gt 0 ] && shift
case ${X4SD-} in
  "") sdopt=if=sd ;; snapshot) sdopt=if=sd,snapshot=on ;; none) sdopt=if=none ;;
  *) echo "x4prosim: X4SD=snapshot|none" >&2; exit 2 ;;
esac
[ -f "$sd" ] || python3 "$here/x4prosim/mksd.py" "$sd"
# QEMU writes to the flash image (NVS, OTA data); run a copy to keep the source clean.
cp "$img" "$img.run"
dir=/tmp/x4prosim/${X4NAME:-$$}
mkdir -p "$dir" && rm -f "$dir/mon.sock" "$dir/qmp.sock"
echo "x4prosim: instance $dir" >&2
chip=$(od -An -tu1 -j12 -N1 "$img" | tr -d ' ')
: "${X4MACHINE:=$([ "$chip" = 5 ] && echo x3 || echo x4pro)}"
if [ "$X4MACHINE" = x3 ]; then
  qemu=$here/build/qemu-system-riscv32
  icount_shift=0
  echo "x4prosim: X3 (ESP32-C3); keys: arrows, Enter, Backspace, P" >&2
else
  qemu=$here/build/qemu-system-xtensa
  icount_shift=2
fi
# release archives have the binaries in bin/ instead of build/
[ -x "$qemu" ] || qemu=$here/bin/${qemu##*/}
[ -n "$X4BR" ] && : "${X4NET=-nic bridge,br=$X4BR,helper=$(dirname "$qemu")/qemu-bridge-helper,model=esp32_wifi}"
[ -n "$X4BR" ] && echo "x4prosim: Wi-Fi bridged to $X4BR (LAN DHCP)" >&2 || echo "x4prosim: Wi-Fi on QEMU NAT (10.0.2.15); X4BR=br0 for your LAN" >&2
net=${X4NET--nic user,model=esp32_wifi,hostfwd=tcp::8080-:80}
# X3 costs are nanoseconds: one icount tick is 1 ns; Xtensa keeps 4 ns ticks.
exec "$qemu" -machine "$X4MACHINE" -icount shift=$icount_shift,sleep=on \
  -drive file="$img.run",if=mtd,format=raw \
  -drive file="$sd",$sdopt,format=raw \
  -chardev stdio,id=cdc,mux=off,logfile="$dir/log.txt" -serial null \
  -global driver=misc.esp32s3.usb_serial_jtag,property=chardev,value=cdc \
  -monitor unix:"$dir/mon.sock",server,nowait -qmp unix:"$dir/qmp.sock",server,nowait \
  -display "${X4DISPLAY:-sdl,show-cursor=on}" \
  $net "$@"

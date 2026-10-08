#!/usr/bin/env python3
"""Boot an X4 Pro image headless and run a script of steps against it.

usage: drive.py flash.bin sd.img log.txt STEP...
steps:  wait:SECONDS     press:up|down|power[:MS]     shot:FILE.png
        hmp:COMMAND      (any monitor command, output printed)
The firmware log (USB-CDC) goes to log.txt."""
import os, socket, subprocess, sys, time

here = os.path.dirname(os.path.abspath(__file__))
qemu = os.path.join(here, "..", "build", "qemu-system-xtensa")
img, sd, log, steps = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
sock = f"/tmp/x4prosim-{os.getpid()}.sock"
if not os.path.exists(sd):
    subprocess.run([sys.executable, os.path.join(here, "mksd.py"), sd], check=True)
subprocess.run(["cp", img, img + ".run"], check=True)
# icount: guest time follows instructions (~240 MHz), not host speed, so light-sleep timing can't overshoot.
p = subprocess.Popen([qemu, "-machine", "x4pro", "-icount", "shift=2,sleep=on", "-display", "none", "-serial", "null",
                      "-drive", f"file={img}.run,if=mtd,format=raw", "-drive", f"file={sd},if=sd,format=raw",
                      "-chardev", f"file,id=cdc,path={log}",
                      "-global", "driver=misc.esp32s3.usb_serial_jtag,property=chardev,value=cdc",
                      "-monitor", f"unix:{sock},server,nowait"] + os.environ.get("QEMU_EXTRA", "").split(),
                     stdout=subprocess.DEVNULL, stderr=sys.stderr)
for _ in range(50):
    if os.path.exists(sock):
        break
    time.sleep(0.1)
s = socket.socket(socket.AF_UNIX)
s.connect(sock)
s.settimeout(0.3)

def hmp(cmd):
    s.sendall(cmd.encode() + b"\n")
    out = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
    except socket.timeout:
        pass
    # HMP echoes the line with terminal escapes; keep only the reply lines.
    lines = out.decode(errors="replace").replace("\r", "").split("\n")
    return "\n".join(l for l in lines[1:] if not l.startswith("(qemu)")).strip()

hmp("")
try:
    for step in steps:
        kind, _, arg = step.partition(":")
        if kind == "wait":
            time.sleep(float(arg))
        elif kind == "press":
            key, _, ms = arg.partition(":")
            hmp(f"qom-set /machine/x4pro-keys {key} true")
            time.sleep(int(ms or 150) / 1000)
            hmp(f"qom-set /machine/x4pro-keys {key} false")
        elif kind == "shot":
            print(hmp(f"screendump {os.path.abspath(arg)} -f png panel") or f"shot {arg}")
        elif kind == "hmp":
            print(hmp(arg))
        else:
            sys.exit(f"unknown step {step}")
finally:
    p.kill()
    os.unlink(sock)

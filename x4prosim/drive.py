#!/usr/bin/env python3
"""Boot an X4 Pro or X3 image headless and run a script of steps against it.

usage: drive.py flash.bin sd.img log.txt STEP...
steps:  wait:SECONDS     press:KEY[:MS]     shot:FILE.png
        hmp:COMMAND      (any monitor command, output printed)
        send:TEXT        write TEXT + newline to the USB-CDC console (serial remote: send:CMD:PING)
        waitlog:REGEX[:S]  wait until the log matches after the previous match (default 60 s;
                         exit 1 on timeout); the match is printed
KEY: up|down|power on the X4 Pro; back|confirm|left|right|up|down|power on the X3.
The machine follows the image's chip (X4MACHINE=x3|x4pro overrides).
The firmware log (USB-CDC) goes to log.txt; the console is a socket chardev, so send: works.
Wi-Fi: the fake AP is on QEMU NAT with host :8080 -> device :80, as run.sh; X4NET overrides
(X4NET="" for no NIC; without one WiFi.mode(OFF) stalls ~5 s)."""
import os, re, socket, subprocess, sys, threading, time

here = os.path.dirname(os.path.abspath(__file__))
img, sd, log, steps = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
with open(img, "rb") as f:
    chip = f.read(13)[12]    # bootloader image header: 5 = ESP32-C3, 9 = ESP32-S3
machine = os.environ.get("X4MACHINE") or ("x3" if chip == 5 else "x4pro")
qemu = os.path.join(here, "..", "build", "qemu-system-riscv32" if machine == "x3" else "qemu-system-xtensa")
if not os.path.exists(qemu):    # release archives have the binaries in bin/
    qemu = os.path.join(here, "..", "bin", os.path.basename(qemu))
keys = "x3-keys" if machine == "x3" else "x4pro-keys"
sock = f"/tmp/x4prosim-{os.getpid()}.sock"
cdc = f"/tmp/x4prosim-{os.getpid()}-cdc.sock"
net = os.environ.get("X4NET", "-nic user,model=esp32_wifi,hostfwd=tcp::8080-:80").split()
if not os.path.exists(sd):
    subprocess.run([sys.executable, os.path.join(here, "mksd.py"), sd], check=True)
subprocess.run(["cp", img, img + ".run"], check=True)
# X3 regional instruction costs are nanoseconds; Xtensa keeps 4 ns ticks.
icount_shift = 0 if machine == "x3" else 2
p = subprocess.Popen([qemu, "-machine", machine,
                      "-icount", f"shift={icount_shift},sleep=on",
                      "-display", "none", "-serial", "null",
                      "-drive", f"file={img}.run,if=mtd,format=raw", "-drive", f"file={sd},if=sd,format=raw",
                      "-chardev", f"socket,id=cdc,path={cdc},server=on,wait=off,logfile={log}",
                      "-global", "driver=misc.esp32s3.usb_serial_jtag,property=chardev,value=cdc",
                      "-monitor", f"unix:{sock},server,nowait"] + net + os.environ.get("QEMU_EXTRA", "").split(),
                     stdout=subprocess.DEVNULL, stderr=sys.stderr)
for _ in range(50):
    if os.path.exists(sock) and os.path.exists(cdc):
        break
    time.sleep(0.1)
s = socket.socket(socket.AF_UNIX)
s.connect(sock)
s.settimeout(0.3)
c = socket.socket(socket.AF_UNIX)
c.connect(cdc)
# The log reaches log.txt through the chardev's logfile; drain the socket so the guest never blocks.
threading.Thread(target=lambda: all(iter(lambda: c.recv(65536), b"")), daemon=True).start()
logpos = 0

def waitlog(rx, timeout):
    global logpos
    end = time.time() + timeout
    r = re.compile(rx.encode())
    while time.time() < end:
        with open(log, "rb") as f:
            m = r.search(f.read(), logpos)
        if m:
            logpos = m.end()
            return m.group(0).decode(errors="replace")
        time.sleep(0.2)
    return None

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
            hmp(f"qom-set /machine/{keys} {key} true")
            time.sleep(int(ms or 150) / 1000)
            hmp(f"qom-set /machine/{keys} {key} false")
        elif kind == "shot":
            print(hmp(f"screendump {os.path.abspath(arg)} -f png panel") or f"shot {arg}")
        elif kind == "hmp":
            print(hmp(arg))
        elif kind == "send":
            c.sendall(arg.encode() + b"\n")
        elif kind == "waitlog":
            rx, _, t = arg.rpartition(":")
            if not t.replace(".", "").isdigit():
                rx, t = arg, "60"
            m = waitlog(rx, float(t))
            print(f"waitlog {rx} -> {m}")
            if m is None:
                sys.exit(1)
        else:
            sys.exit(f"unknown step {step}")
finally:
    p.kill()
    os.unlink(sock)
    os.unlink(cdc)

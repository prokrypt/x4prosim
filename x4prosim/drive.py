#!/usr/bin/env python3
"""Boot an X4 Pro or X3 image headless and run a script of steps against it.

usage: drive.py flash.bin sd.img log.txt STEP...
steps:  wait:SECONDS       press:KEY[+KEY...][:MS]    shot:FILE.png
        boot:KEY[+KEY...][:MS]  (first step only: keys held from power-on)
        tap:X,Y[:MS]       tap:home[:MS]              swipe:X0,Y0:X1,Y1[:MS]
        touch:X,Y[;X,Y...] touch:up                   (up to 5 contacts, held)
        hmp:COMMAND        (any monitor command, output printed)
KEY: up|down|power on the X4 Pro; back|confirm|left|right|up|down|power on the X3.
X,Y: touch controller coordinates, 480x800 portrait, as the panel screendump.
On the X4 Pro, MS is guest time (press 150, tap 100, swipe 300 by default), and
input is held until the guest has seen it: keys until GPIO has been read twice
8 ms apart, touch until the frame has been read; release waits the same way.
Input the guest never saw prints a warning. The X3 holds MS of host time.
The machine follows the image's chip (X4MACHINE=x3|x4pro overrides).
The firmware log (USB-CDC) goes to log.txt."""
import os, socket, subprocess, sys, time

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
if not os.path.exists(sd):
    subprocess.run([sys.executable, os.path.join(here, "mksd.py"), sd], check=True)
subprocess.run(["cp", img, img + ".run"], check=True)
# X3 regional instruction costs are nanoseconds; Xtensa keeps 4 ns ticks.
icount_shift = 0 if machine == "x3" else 2
p = subprocess.Popen([qemu, "-machine", machine,
                      "-icount", f"shift={icount_shift},sleep=on",
                      "-display", "none", "-serial", "null",
                      "-drive", f"file={img}.run,if=mtd,format=raw", "-drive", f"file={sd},if=sd,format=raw",
                      "-chardev", f"file,id=cdc,path={log}",
                      "-global", "driver=misc.esp32s3.usb_serial_jtag,property=chardev,value=cdc",
                      "-monitor", f"unix:{sock},server,nowait"]
                     + (["-S"] if steps and steps[0].startswith("boot:") else []) + os.environ.get("QEMU_EXTRA", "").split(),
                     stdout=subprocess.DEVNULL, stderr=sys.stderr)
for _ in range(50):
    if os.path.exists(sock):
        break
    time.sleep(0.1)
s = socket.socket(socket.AF_UNIX)
s.connect(sock)
s.settimeout(5)

def reply():
    """Monitor output up to the next prompt."""
    out = b""
    try:
        while not out.endswith(b"(qemu) "):
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
    except socket.timeout:
        pass
    return out

def hmp(cmd):
    s.sendall(cmd.encode() + b"\n")
    out = reply()
    # HMP echoes the line with terminal escapes; keep only the reply lines.
    lines = out.decode(errors="replace").replace("\r", "").split("\n")
    return "\n".join(l for l in lines[1:] if not l.startswith("(qemu)")).strip()

def qint(path, prop):
    try:
        return int(hmp(f"qom-get {path} {prop}"))
    except ValueError:      # X3, or a build without the counters
        return None

def vclock():
    return qint(f"/machine/{keys}", "vclock-ms")

def until(cond, timeout):
    end = time.monotonic() + timeout
    while not cond():
        if time.monotonic() > end:
            return False
        time.sleep(0.005)
    return True

def press(names, ms, step, then=None):
    """Hold keys ms guest time and until GPIO is sampled twice; release likewise."""
    for k in names:
        hmp(f"qom-set /machine/{keys} {k} true")
    if then:
        then()
    t0, n0 = vclock(), qint("/machine/soc/gpio", "in-samples")
    if t0 is None or n0 is None:
        time.sleep(ms / 1000)
        seen = True
    else:
        # Guest time can run slower than host time; the bound only stops a hang.
        seen = until(lambda: vclock() - t0 >= ms and qint("/machine/soc/gpio", "in-samples") - n0 >= 2,
                     2 + ms / 100)
    for k in names:
        hmp(f"qom-set /machine/{keys} {k} false")
    if n0 is not None:
        n1 = qint("/machine/soc/gpio", "in-samples")
        seen &= until(lambda: qint("/machine/soc/gpio", "in-samples") - n1 >= 2, 2)
    if not seen:
        print(f"warning: {step}: not seen by the guest")

def touch_read(timeout):
    return until(lambda: qint("/machine/gt911", "read-seq") >= qint("/machine/gt911", "input-seq"), timeout)

def touch(cmd, step, ms=0):
    """Set touch input, hold ms guest time and until the guest has read it."""
    hmp(f"qom-set /machine/gt911 {cmd}")
    t0 = vclock()
    if not until(lambda: vclock() - t0 >= ms, 2 + ms / 100) or not touch_read(5):
        print(f"warning: {step}: not read by the guest")

def point(xy):
    x, y = (int(v) for v in xy.split(","))
    return x, y

reply()     # banner
try:
    for step in steps:
        kind, _, arg = step.partition(":")
        if kind == "wait":
            time.sleep(float(arg))
        elif kind in ("press", "boot"):
            names, _, ms = arg.partition(":")
            press(names.split("+"), int(ms or 150), step, (lambda: hmp("cont")) if kind == "boot" else None)
        elif kind == "tap":
            where, _, ms = arg.partition(":")
            down, up = ("home on", "home off") if where == "home" else (f"touch {where}", "touch up")
            touch(down, step, int(ms or 100))
            touch(up, step)
        elif kind == "touch":
            touch(f"touch {arg}", step)
        elif kind == "swipe":
            a, b, ms = (arg.split(":") + [""])[:3]
            (x0, y0), (x1, y1) = point(a), point(b)
            n = max(1, round(int(ms or 300) / 20))
            unread = 0
            for i in range(n + 1):  # one point per 20 ms guest, each held until read (0.5 s max)
                hmp(f"qom-set /machine/gt911 touch {x0 + (x1 - x0) * i // n},{y0 + (y1 - y0) * i // n}")
                t0 = vclock()
                unread += not touch_read(0.5)
                until(lambda: vclock() - t0 >= 20, 2)
            touch("touch up", step)
            if unread:
                print(f"warning: {step}: {unread} of {n + 1} points not read by the guest")
        elif kind == "shot":
            print(hmp(f"screendump {os.path.abspath(arg)} -f png panel") or f"shot {arg}")
        elif kind == "hmp":
            print(hmp(arg))
        else:
            sys.exit(f"unknown step {step}")
finally:
    p.kill()
    os.unlink(sock)

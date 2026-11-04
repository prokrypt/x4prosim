#!/usr/bin/env python3
"""uc8179 addressing self-test: PSR UD/SHL and the PTL (0x90) partial window.

usage: x4prosim/paneltest.py [path/to/qemu-system-xtensa]

Starts the x4pro machine under qtest (no firmware), bit-bangs panel commands
on the panel's sclk/sda/dc/cs inputs, refreshes with animate=false and checks
the screendump (portrait: shown x = 479 - gate, y = source). Exit 1 on a failure.
"""
import json, os, socket, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
QEMU = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "build", "qemu-system-xtensa")
PANEL = "/machine/peripheral/panel"


class Sim:
    def __init__(self, tmp):
        self.qmp_path, self.log = os.path.join(tmp, "qmp.sock"), os.path.join(tmp, "qemu.log")
        self.p = subprocess.Popen([QEMU, "-machine", "x4pro", "-accel", "qtest", "-qtest", "stdio",
                                   "-display", "none", "-serial", "null", "-monitor", "none",
                                   "-qmp", f"unix:{self.qmp_path},server=on,wait=off",
                                   "-global", "uc8179.animate=false", "-d", "guest_errors", "-D", self.log],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                  text=True)
        self.irq("ssi-gpio-cs", 0)
        self.irq("sclk", 0)

    def irq(self, name, level):
        self.p.stdin.write(f"set_irq_in {PANEL} {name} 0 {level}\n")
        self.p.stdin.flush()
        while not self.p.stdout.readline().startswith(("OK", "FAIL", "ERR")):
            pass

    def byte(self, v):
        for bit in range(7, -1, -1):
            self.irq("sda", (v >> bit) & 1)
            self.irq("sclk", 1)
            self.irq("sclk", 0)

    def cmd(self, c, *data):
        self.irq("dc", 0)
        self.byte(c)
        self.irq("dc", 1)
        for d in data:
            self.byte(d)

    def shot(self):
        q = socket.socket(socket.AF_UNIX)
        q.connect(self.qmp_path)
        f = q.makefile("rw")
        f.readline()
        for c in ({"execute": "qmp_capabilities"},
                  {"execute": "screendump", "arguments": {"filename": self.qmp_path + ".ppm", "format": "ppm"}}):
            f.write(json.dumps(c) + "\n")
            f.flush()
            while "return" not in (r := json.loads(f.readline())) and "error" not in r:
                pass
        q.close()
        data = open(self.qmp_path + ".ppm", "rb").read()
        w, h = map(int, data.split(b"\n")[1].split())
        px = data[data.index(b"255\n") + 4:]
        return lambda x, y: px[(y * w + x) * 3] < 128     # True = black

    def close(self):
        self.p.kill()
        self.p.wait()
        return open(self.log).read() if os.path.exists(self.log) else ""


def black_set(is_black, xs, ys):
    return {(x, y) for x in xs for y in ys if is_black(x, y)}


fails = 0


def check(name, ok):
    global fails
    print(("PASS " if ok else "FAIL ") + name)
    fails += not ok


with tempfile.TemporaryDirectory() as tmp:
    # RAM row 0, columns 0..15 black; full OTP refresh.
    for psr, want in ((0x0F, {(0, y) for y in range(16)}),          # UD=1 SHL=1 (X4 Pro): gate 0, x 0..15
                      (0x0B, {(0, y) for y in range(784, 800)}),    # SHL=0: x mirrored
                      (0x07, set())):                               # UD=0: RAM row 0 -> gate 599, not visible
        s = Sim(tmp)
        s.cmd(0x00, psr, 0x0A)
        s.cmd(0x13, 0x00, 0x00)
        s.cmd(0x12)
        got = black_set(s.shot(), range(0, 4), list(range(0, 24)) + list(range(776, 800)))
        s.close()
        check(f"PSR 0x{psr:02X}: RAM row 0 x 0..15 shows at {sorted(want)[:1]}..", got == want)

    # PTL window x 80..95, y 10..17 inside PTIN: DTM fills the window, the refresh drives only it.
    s = Sim(tmp)
    s.cmd(0x91)
    s.cmd(0x90, 0, 80, 0, 95, 0, 10, 0, 17, 0)
    s.cmd(0x13, *([0x00] * 16))
    s.cmd(0x12)
    win = {(x, y) for x in range(10, 18) for y in range(80, 96)}
    got = black_set(s.shot(), range(0, 30), range(0, 120))
    check("PTL window: 16 bytes land in RAM rows 10..17, x 80..95", got == win)
    s.cmd(0x92)
    s.cmd(0x12)                     # PTOUT: full refresh, RAM row 0 never written
    got = black_set(s.shot(), range(0, 30), range(0, 120))
    check("PTOUT full refresh keeps the window picture, row 0 white", got == win)
    log = s.close()
    check("windowed refresh logs a guest error", "partial-window refresh x 80..95 y 10..17" in log)

    # PTIN without PTL is the whole panel (what the X4 Pro driver sends): no error.
    s = Sim(tmp)
    s.cmd(0x91)
    s.cmd(0x13, 0x00, 0x00)
    s.cmd(0x12)
    got = black_set(s.shot(), range(0, 4), range(0, 24))
    log = s.close()
    check("PTIN without PTL: whole panel, no guest error",
          got == {(0, y) for y in range(16)} and "partial-window" not in log)

sys.exit(1 if fails else 0)

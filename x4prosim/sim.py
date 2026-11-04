#!/usr/bin/env python3
"""Boot an X4 Pro (or X3) image headless and run test steps against it.

usage: sim.py [--json] flash.bin sd.img DIR STEP...
The instance lives in DIR (log.txt, qemu.err, the two sockets, screenshots), so runs in
different DIRs go in parallel. A STEP is "kind:arg" or "kind arg"; "@FILE" runs a file of
steps (one per line, # comments), "-" reads them from stdin.
  wait S              host sleep
  waitlog REGEX [S]   wait until the firmware log matches after the mark (default 60 s)
  mark                later waitlog/cmd only look at the log from here
  cmd VERB ...        serial remote "CMD:VERB" (CrossDink debug builds), waits up to 90 s
                      for its OK:/ERR: line; ERR or no reply fails
  shot NAME[.png]     panel screendump to DIR/NAME.png (a NAME with / is a path)
  key KEY[:MS]        hold KEY for MS ms (default 150); press is the same.
                      X4 Pro: up|down|power; X3: back|confirm|left|right|up|down|power
  tap X,Y             GT911 tap (X4 Pro), panel coordinates
  hmp COMMAND         any monitor command, its output printed
Prints one line per step (JSON {"t","step","ok","out"} with --json). Stops at the first
failed step and exits 1. QEMU_EXTRA adds QEMU arguments; the machine follows the image's
chip (X4MACHINE=x3|x4pro overrides). The guest gets a user-mode NIC (open AP
"PICSimLabWifi"), so Wi-Fi calls don't wait for a missing radio; X4NET="-nic ..." replaces it."""
import json, os, re, socket, subprocess, sys, threading, time

here = os.path.dirname(os.path.abspath(__file__))


class Sim:
    def __init__(self, img, sd, out):
        self.out = os.path.abspath(out)
        os.makedirs(self.out, exist_ok=True)
        with open(img, "rb") as f:
            chip = f.read(13)[12]    # bootloader image header: 5 = ESP32-C3, 9 = ESP32-S3
        self.machine = os.environ.get("X4MACHINE") or ("x3" if chip == 5 else "x4pro")
        name = "qemu-system-riscv32" if self.machine == "x3" else "qemu-system-xtensa"
        qemu = os.path.join(here, "..", "build", name)
        if not os.path.exists(qemu):    # release archives have the binaries in bin/
            qemu = os.path.join(here, "..", "bin", name)
        self.keys = "x3-keys" if self.machine == "x3" else "x4pro-keys"
        if not os.path.exists(sd):
            subprocess.run([sys.executable, os.path.join(here, "mksd.py"), sd], check=True)
        run = os.path.join(self.out, "flash.run")    # the guest writes flash (NVS, OTA data)
        subprocess.run(["cp", img, run], check=True)
        cdc, mon = os.path.join(self.out, "cdc.sock"), os.path.join(self.out, "mon.sock")
        for p in (cdc, mon):
            if os.path.exists(p):
                os.unlink(p)
        self.err = os.path.join(self.out, "qemu.err")
        self.q = subprocess.Popen(
            [qemu, "-machine", self.machine,
             # X3 regional instruction costs are nanoseconds; Xtensa keeps 4 ns ticks.
             "-icount", f"shift={0 if self.machine == 'x3' else 2},sleep=on",
             "-display", "none", "-serial", "null",
             "-drive", f"file={run},if=mtd,format=raw", "-drive", f"file={sd},if=sd,format=raw",
             "-chardev", f"socket,id=cdc,path={cdc},server=on,wait=off,logfile={self.out}/log.txt",
             "-global", "driver=misc.esp32s3.usb_serial_jtag,property=chardev,value=cdc",
            ] + os.environ.get("X4NET", "-nic user,model=esp32_wifi").split() + [
             "-monitor", f"unix:{mon},server,nowait"] + os.environ.get("QEMU_EXTRA", "").split(),
            stdout=subprocess.DEVNULL, stderr=open(self.err, "w"))
        self.cdc = self._connect(cdc, None)
        self.mon = self._connect(mon, 0.3)
        self.buf, self.mark, self.cdc_closed = b"", 0, False
        self.cv = threading.Condition()
        # Read the console all the time: an unread socket would stall the guest's USB-CDC writes.
        threading.Thread(target=self._pump, daemon=True).start()
        self.hmp("")

    def _connect(self, path, timeout):
        for _ in range(100):
            if self.q.poll() is not None:
                break
            try:
                c = socket.socket(socket.AF_UNIX)
                c.connect(path)
                c.settimeout(timeout)
                return c
            except OSError:
                time.sleep(0.1)
        self.close()
        sys.exit(f"cannot connect {path}: " + open(self.err).read())

    def _pump(self):
        while True:
            try:
                c = self.cdc.recv(65536)
            except OSError:
                c = b""
            with self.cv:
                if not c:
                    self.cdc_closed = True
                    self.cv.notify_all()
                    return
                self.buf += c
                self.cv.notify_all()

    def hmp(self, cmd):
        self.mon.sendall(cmd.encode() + b"\n")
        out = b""
        try:
            while True:
                c = self.mon.recv(65536)
                if not c:
                    break
                out += c
        except socket.timeout:
            pass
        # HMP echoes the line with terminal escapes; keep only the reply lines.
        lines = out.decode(errors="replace").replace("\r", "").split("\n")
        return "\n".join(l for l in lines[1:] if not l.startswith("(qemu)")).strip()

    def set_mark(self):
        with self.cv:
            self.mark = len(self.buf)

    def waitlog(self, rx, timeout):
        r = re.compile(rx.encode())
        end = time.time() + timeout
        with self.cv:
            while True:
                m = r.search(self.buf, self.mark)
                if m:
                    self.mark = m.end()
                    return m.group(0).decode(errors="replace")
                left = end - time.time()
                if left <= 0 or self.cdc_closed or self.q.poll() is not None:
                    return None
                self.cv.wait(min(left, 0.5))

    def step(self, line):
        """Runs one step; returns (ok, output)."""
        kind, arg = re.match(r"\s*([A-Za-z]+)[:\s]?(.*)", line).groups()
        kind, arg = kind.lower(), arg.strip()
        if kind == "wait":
            time.sleep(float(arg))
            return True, ""
        if kind == "mark":
            self.set_mark()
            return True, ""
        if kind == "waitlog":
            rx, t = arg, 60.0
            m = re.match(r"(.*\S)\s+(\d+(?:\.\d+)?)$", arg)
            if m:
                rx, t = m.group(1), float(m.group(2))
            got = self.waitlog(rx, t)
            return got is not None, got or f"timeout {t:g} s"
        if kind == "cmd":
            self.set_mark()
            self.cdc.sendall(f"CMD:{arg}\n".encode())
            # Whole line only: the reply can arrive split across socket reads.
            got = self.waitlog(r"(?m)^(OK|ERR):[^\r\n]*(?=\r?\n)", 90)    # WAITIDLE may take 60 s
            return bool(got and got.startswith("OK:")), got or "no reply in 90 s"
        if kind == "shot":
            path = arg if "/" in arg else os.path.join(self.out, arg)
            if not path.endswith(".png"):
                path += ".png"
            r = self.hmp(f"screendump {os.path.abspath(path)} -f png panel")
            return not r, r or path
        if kind in ("key", "press"):
            key, _, ms = arg.partition(":")
            if " " in key:
                key, ms = key.split(None, 1)
            self.hmp(f"qom-set /machine/{self.keys} {key} true")
            time.sleep(int(ms or 150) / 1000)
            r = self.hmp(f"qom-set /machine/{self.keys} {key} false")
            return not r, r
        if kind == "tap":
            r = self.hmp(f"qom-set /machine/gt911 tap {arg.replace(' ', '')}")
            return not r, r
        if kind == "hmp":
            return True, self.hmp(arg)
        return False, f"unknown step {line!r}"

    def close(self):
        self.q.kill()
        self.q.wait()


def expand(args):
    for a in args:
        if a == "-" or a.startswith("@"):
            for line in (sys.stdin if a == "-" else open(a[1:])):
                line = line.strip()
                if line and not line.startswith("#"):
                    yield line
        else:
            yield a


def main(argv):
    as_json = argv[:1] == ["--json"]
    if as_json:
        argv = argv[1:]
    if len(argv) < 3:
        sys.exit(__doc__)
    sim = Sim(*argv[:3])
    t0, ok = time.time(), True
    try:
        for line in expand(argv[3:]):
            ok, out = sim.step(line)
            t = round(time.time() - t0, 1)
            if as_json:
                print(json.dumps({"t": t, "step": line, "ok": ok, "out": out}), flush=True)
            else:
                print(f"[{t:6.1f}] {'ok ' if ok else 'FAIL'} {line}" + (f" -> {out}" if out else ""), flush=True)
            if not ok:
                break
    finally:
        sim.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

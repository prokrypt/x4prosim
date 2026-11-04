#!/usr/bin/env python3
"""OTA slots of an X4 Pro flash image, and an end-to-end Wi-Fi OTA test in x4prosim.

usage: ota.py otadata FLASH
         The OTA slots from the partition table, both otadata entries (seq, state, CRC) and the
         slot the bootloader picks.
       ota.py test FLASH APP [OUTDIR]
         Boots FLASH (a CrossDink debug build: /api/ota), joins the fake AP from a seeded wifi.json,
         POSTs APP to /api/ota through a QEMU hostfwd, then checks the reply, that otadata selects
         the other slot and that the next boot runs it. Then two brick-risk cases on copies of the
         updated image: a corrupted new slot must boot the old one, and a new slot left in
         PENDING_VERIFY shows whether the bootloader rolls back (reported, not asserted).
         Exit 1 on a failed check. Logs and images stay in OUTDIR (default a temp dir).
The otadata layout and selection rule follow ESP-IDF's bootloader_support (esp_ota_select_entry_t:
seq, label[20], state, crc32_le(seq)); idea and parser shape after kycube/x4pro-emu (MIT,
(c) 2026 KFloyd.com) tests/test_device_otadata.py and tools/device.py."""
import os, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time, urllib.request, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
STATES = {0: "NEW", 1: "PENDING_VERIFY", 2: "VALID", 3: "INVALID", 4: "ABORTED", 0xFFFFFFFF: "UNDEFINED"}
TOKEN = "123456"


def partitions(img):
    """(label, type, subtype, offset, size) from the table at 0x8000."""
    out = []
    for i in range(0x8000, 0x8C00, 32):
        magic, t, st, off, size, label = struct.unpack_from("<HBBII16s", img, i)
        if magic != 0x50AA:
            break
        out.append((label.rstrip(b"\0").decode(), t, st, off, size))
    return out


def ota_layout(img):
    parts = partitions(img)
    otadata = next(p for p in parts if p[1] == 1 and p[2] == 0)
    apps = sorted((p for p in parts if p[1] == 0 and 0x10 <= p[2] < 0x20), key=lambda p: p[2])
    return otadata, apps


def entries(img, otadata):
    """Both otadata sectors: (seq, state, crc_ok)."""
    out = []
    for sector in (0, 0x1000):
        seq, _label, state, crc = struct.unpack_from("<I20sII", img, otadata[3] + sector)
        out.append((seq, state, seq != 0xFFFFFFFF and crc == zlib.crc32(struct.pack("<I", seq), 0xFFFFFFFF)))
    return out


def selected(img, rollback=True):
    """The app slot the bootloader picks: the highest valid seq, slot (seq - 1) % count; ota_0 if none."""
    otadata, apps = ota_layout(img)
    valid = [e for e in entries(img, otadata) if e[2] and not (rollback and e[1] in (3, 4))]
    if not valid:
        return apps[0][0]
    return apps[(max(valid)[0] - 1) % len(apps)][0]


def cmd_otadata(path):
    img = open(path, "rb").read()
    otadata, apps = ota_layout(img)
    for a in apps:
        print(f"{a[0]:8} 0x{a[3]:06x} size 0x{a[4]:06x}")
    for i, (seq, state, ok) in enumerate(entries(img, otadata)):
        print(f"otadata[{i}] seq={seq:#x} state={STATES.get(state, hex(state))} crc={'ok' if ok else 'erased' if seq == 0xFFFFFFFF else 'bad'}")
    print("boots", selected(img))


def write_entry(path, sector, seq, state):
    with open(path, "r+b") as f:
        img = f.read()
        otadata, _ = ota_layout(img)
        f.seek(otadata[3] + sector * 0x1000)
        f.write(struct.pack("<I20sII", seq, b"\xff" * 20, state, zlib.crc32(struct.pack("<I", seq), 0xFFFFFFFF)))


class Sim:
    """One x4pro QEMU run: console on a socket (log in a file), NIC with host port -> device :80."""

    def __init__(self, flash, sd, log):
        qemu = os.path.join(HERE, "..", "build", "qemu-system-xtensa")
        if not os.path.exists(qemu):
            qemu = os.path.join(HERE, "..", "bin", "qemu-system-xtensa")
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]
        self.log, self.pos = log, 0
        self.cdc = log + ".sock"
        self.p = subprocess.Popen(
            [qemu, "-machine", "x4pro", "-icount", "shift=2,sleep=on", "-display", "none", "-serial", "null",
             "-monitor", "none", "-drive", f"file={flash},if=mtd,format=raw", "-drive", f"file={sd},if=sd,format=raw",
             "-chardev", f"socket,id=cdc,path={self.cdc},server=on,wait=off,logfile={log}",
             "-global", "driver=misc.esp32s3.usb_serial_jtag,property=chardev,value=cdc",
             "-nic", f"user,model=esp32_wifi,hostfwd=tcp:127.0.0.1:{self.port}-:80"],
            stdout=subprocess.DEVNULL, stderr=open(log + ".err", "w"))
        for _ in range(100):
            try:
                self.c = socket.socket(socket.AF_UNIX)
                self.c.connect(self.cdc)
                break
            except OSError:
                time.sleep(0.1)
        # The log reaches the file via logfile=; drain the socket so the guest never blocks on it.
        threading.Thread(target=self._drain, daemon=True).start()

    def _drain(self):
        try:
            while self.c.recv(65536):
                pass
        except OSError:
            pass

    def wait(self, rx, timeout=120):
        """Wait for rx in the log after the previous match; the match, or exit 1."""
        end, r = time.time() + timeout, re.compile(rx.encode())
        while time.time() < end:
            m = r.search(open(self.log, "rb").read(), self.pos)
            if m:
                self.pos = m.end()
                return m.group(0).decode(errors="replace")
            time.sleep(0.2)
        fail(f"timeout waiting for {rx!r} ({self.log})")

    def cmd(self, verb):
        self.c.sendall(f"CMD:{verb}\n".encode())
        return self.wait(rf"(OK|ERR):{verb.split()[0]}[^\r\n]*", 60)

    def running(self):
        return re.search(r"running=(\S+)", self.cmd("BOOTSLOT")).group(1)

    def stop(self):
        self.p.kill()
        self.p.wait()
        self.c.close()


def fail(msg):
    print("FAIL", msg)
    sys.exit(1)


def boot_slot(flash, sd, log, reboot=False):
    """The slot the image boots after a power-on, or after a power-on and a CMD:REBOOT (software
    reset): CrossDink sets BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON, so only the second validates."""
    sim = Sim(flash, sd, log)
    sim.wait(r"Home", 180)
    slot = sim.running()
    if reboot:
        sim.cmd("REBOOT")
        sim.wait(r"rst:", 60)
        sim.wait(r"Home", 180)
        slot = sim.running()
    sim.stop()
    return slot


def cmd_test(flash, app, out=None):
    out = out or tempfile.mkdtemp(prefix="x4prosim-ota-")
    os.makedirs(out, exist_ok=True)
    root = os.path.join(out, "sdroot")
    os.makedirs(os.path.join(root, ".crossdink"), exist_ok=True)
    os.makedirs(os.path.join(root, "debug"), exist_ok=True)
    with open(os.path.join(root, ".crossdink", "wifi.json"), "w") as f:   # the fake AP is open
        f.write('{"lastConnectedSsid":"PICSimLabWifi","credentials":[{"ssid":"PICSimLabWifi","password_obf":""}]}')
    with open(os.path.join(root, "debug", "remote-token"), "w") as f:
        f.write(TOKEN + "\n")
    sd = os.path.join(out, "sd.img")
    subprocess.run([sys.executable, os.path.join(HERE, "mksd.py"), sd, "256", root], check=True)
    run = os.path.join(out, "flash.run")
    shutil.copy(flash, run)
    print("out", out)

    sim = Sim(run, sd, os.path.join(out, "1-ota.txt"))
    sim.wait(r"Home", 180)
    before = sim.running()
    print("boot 1 runs", before)
    print(sim.cmd("GOTO transfer-wifi"))
    sim.wait(r"server running on port 80", 120)
    t = time.time()
    req = urllib.request.Request(f"http://127.0.0.1:{sim.port}/api/ota", data=open(app, "rb").read(),
                                 headers={"Content-Type": "application/octet-stream", "X-Token": TOKEN})
    try:
        reply = urllib.request.urlopen(req, timeout=900).read().decode()
    except urllib.error.HTTPError as e:
        reply = f"{e.code} {e.read().decode(errors='replace')}"
    print(f"POST /api/ota ({os.path.getsize(app)} bytes, {time.time() - t:.0f} s host) -> {reply}")
    if not reply.startswith("OK:OTA"):
        fail("OTA refused")
    sim.wait(r"rst:", 60)                          # the restart the reply promised
    sim.wait(r"Home", 180)
    after = sim.running()
    sim.stop()
    print("boot 2 runs", after)
    img = open(run, "rb").read()
    want = next(a[0] for a in ota_layout(img)[1] if a[0] != before)
    if after != want or selected(img) != want:
        fail(f"expected {want}: booted {after}, otadata selects {selected(img)}")
    otadata, apps = ota_layout(img)
    seq, state, _ = max(e for e in entries(img, otadata) if e[2])
    print(f"otadata: seq={seq:#x} state={STATES.get(state, hex(state))} -> {want} ok")

    # Case A: the new slot's image is damaged after the switch; a validating (software) reset
    # must refuse it and boot the old slot.
    bad = os.path.join(out, "flash-corrupt.run")
    shutil.copy(run, bad)
    slot = next(a for a in apps if a[0] == want)
    with open(bad, "r+b") as f:
        f.seek(slot[3] + 0x1000)
        b = f.read(1)
        f.seek(slot[3] + 0x1000)
        f.write(bytes([b[0] ^ 0xFF]))
    got = boot_slot(bad, sd, os.path.join(out, "2-corrupt.txt"), reboot=True)
    print("corrupted", want, "-> after a software reset boots", got)
    if got != before:
        fail(f"a corrupted {want} must fall back to {before}")

    # Case B: the new slot never confirmed itself (PENDING_VERIFY at the next reset).
    pend = os.path.join(out, "flash-pending.run")
    shutil.copy(run, pend)
    sector = [e[0] for e in entries(img, otadata)].index(seq)
    write_entry(pend, sector, seq, 1)
    got = boot_slot(pend, sd, os.path.join(out, "3-pending.txt"))
    st = entries(open(pend, "rb").read(), otadata)[sector][1]
    print(f"{want} PENDING_VERIFY at reset -> boots {got}, entry now {STATES.get(st, hex(st))}",
          "(rollback on)" if got == before else "(rollback off in this bootloader)")
    print("PASS")


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["otadata"] and len(a) == 2:
        cmd_otadata(a[1])
    elif a[:1] == ["test"] and len(a) in (3, 4):
        cmd_test(*a[1:])
    else:
        sys.exit(__doc__)

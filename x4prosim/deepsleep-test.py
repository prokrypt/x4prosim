#!/usr/bin/env python3
"""Deep-sleep self-check for the X4 Pro RTC_CNTL model, over QEMU's qtest protocol (no firmware).

usage: x4prosim/deepsleep-test.py
Arms each wake source the way ESP-IDF does, enters deep sleep, and checks the VM suspends,
then wakes into a reset with reason DEEPSLEEP and the right SLP_WAKEUP_CAUSE."""
import os, socket, subprocess, sys, time

here = os.path.dirname(os.path.abspath(__file__))
qemu = os.path.join(here, "..", "build", "qemu-system-xtensa")
if not os.path.exists(qemu):
    qemu = os.path.join(here, "..", "bin", "qemu-system-xtensa")

RTC, RTCIO = 0x60008000, 0x60008400
SLP_TIMER0, SLP_TIMER1, STATE0, RESET_STATE, WAKEUP_STATE = 0x04, 0x08, 0x18, 0x38, 0x3C
TIME_UPDATE, TIME0 = 0x0C, 0x10
EXT_WAKEUP_CONF, DIG_PWC, EXT_WAKEUP1, EXT_WAKEUP1_STATUS, WAKEUP_CAUSE = 0x64, 0x90, 0xE0, 0xE4, 0x130
EXT0, EXT1, TIMER = 1, 2, 8
GPIO = "/machine/soc/gpio"


class QTest:
    def __init__(self):
        sock = f"/tmp/x4prosim-qtest-{os.getpid()}.sock"
        srv = socket.socket(socket.AF_UNIX)
        srv.bind(sock)
        srv.listen(1)
        self.p = subprocess.Popen([qemu, "-machine", "x4pro", "-accel", "qtest", "-display", "none",
                                   "-qtest", f"unix:{sock}", "-qtest-log", "/dev/null", "-monitor", "none", "-serial", "null"],
                                  stdout=subprocess.DEVNULL)
        self.s, _ = srv.accept()
        srv.close()
        os.unlink(sock)
        self.f = self.s.makefile("r")

    def cmd(self, line):
        self.s.sendall((line + "\n").encode())
        while True:
            r = self.f.readline().split()
            if r and r[0] == "OK":
                return r[1:]
            assert r and r[0] == "IRQ", f"{line}: {r}"

    def w(self, a, v):
        self.cmd(f"writel 0x{a:x} 0x{v:x}")

    def r(self, a):
        return int(self.cmd(f"readl 0x{a:x}")[0], 16)

    def pin(self, n, level):
        self.cmd(f"set_irq_in {GPIO} pin-in {n} {level}")

    def close(self):
        self.p.kill()


def sleep(q, ena):
    q.w(RTC + WAKEUP_STATE, ena << 15)
    q.w(RTC + DIG_PWC, 1 << 31)
    q.w(RTC + STATE0, 1 << 31)


def check(q, name, cause):
    time.sleep(0.3)  # the reset runs on the next main-loop pass
    assert q.r(RTC + RESET_STATE) & 0x3f == 5, f"{name}: reset reason not DEEPSLEEP"
    assert q.r(RTC + WAKEUP_CAUSE) == cause, f"{name}: cause {q.r(RTC + WAKEUP_CAUSE)} != {cause}"
    assert not q.r(RTC + STATE0) & (1 << 31), f"{name}: still asleep"
    print(f"ok  {name}")


q = QTest()
try:
    # ext1, any low on GPIO3 (Power): nothing until the pad drops, then GPIO3 in the status.
    q.w(RTC + EXT_WAKEUP1, 1 << 3)
    q.w(RTC + EXT_WAKEUP_CONF, 0)
    sleep(q, EXT1)
    assert q.r(RTC + STATE0) & (1 << 31), "ext1: woke with the pad high"
    q.pin(3, 0)
    check(q, "ext1 Power low", EXT1)
    assert q.r(RTC + EXT_WAKEUP1_STATUS) == 1 << 3, "ext1: status"
    q.pin(3, 1)

    # ext0 on GPIO21 (charger STAT) going low.
    q.pin(21, 1)
    q.w(RTCIO + 0xDC, 21 << 27)
    sleep(q, EXT0)
    q.pin(21, 0)
    check(q, "ext0 GPIO21 low", EXT0)

    # RTC timer 0.5 s ahead (RC slow clock 150 kHz). Under qtest the virtual clock stands
    # still, so this only passes if the wake runs on host time while suspended.
    q.w(RTC + TIME_UPDATE, 1 << 31)
    now = q.r(RTC + TIME0)
    q.w(RTC + SLP_TIMER0, now + 75000)
    q.w(RTC + SLP_TIMER1, 1 << 16)
    sleep(q, TIMER)
    assert q.r(RTC + STATE0) & (1 << 31), "timer: woke early"
    time.sleep(1)
    check(q, "RTC timer", TIMER)
    q.w(RTC + TIME_UPDATE, 1 << 31)
    assert q.r(RTC + TIME0) - now >= 75000, "timer: RTC counter didn't count the sleep"
    print("ok  RTC counter advanced by the sleep")
finally:
    q.close()

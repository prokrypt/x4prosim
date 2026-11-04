#!/usr/bin/env python3
"""Light- and deep-sleep self-check for the X4 Pro RTC_CNTL model, over QEMU's qtest protocol (no firmware).

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
INT_RAW, INT_CLR = 0x44, 0x4C
DIG_ISO, PAD_HOLD, DIG_PAD_HOLD = 0x94, 0xD8, 0xDC
GPIO_BASE, GPIO_OUT, GPIO_OUT1, GPIO_ENABLE, GPIO_ENABLE1 = 0x60004000, 0x04, 0x10, 0x20, 0x2C
EXT_WAKEUP_CONF, DIG_PWC, EXT_WAKEUP1, EXT_WAKEUP1_STATUS, WAKEUP_CAUSE = 0x64, 0x90, 0xE0, 0xE4, 0x130
EXT0, EXT1, TIMER = 1, 2, 8
GPIO = "/machine/soc/gpio"
TRACE = f"/tmp/x4prosim-deepsleep-{os.getpid()}.trace"


class QTest:
    def __init__(self, *extra):
        sock = f"/tmp/x4prosim-qtest-{os.getpid()}.sock"
        srv = socket.socket(socket.AF_UNIX)
        srv.bind(sock)
        srv.listen(1)
        self.p = subprocess.Popen([qemu, "-machine", "x4pro", "-accel", "qtest", "-display", "none",
                                   "-qtest", f"unix:{sock}", "-qtest-log", "/dev/null", "-monitor", "none", "-serial", "null",
                                   "-d", "trace:esp32s3_rtc_wake,trace:esp32s3_rtc_deep_pads", "-D", TRACE, *extra],
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
    # Light sleep (DIG_PWC clear) on the RTC timer 10 ms ahead: the wake interrupt after 20 ms.
    q.w(RTC + TIME_UPDATE, 1 << 31)
    q.w(RTC + SLP_TIMER0, q.r(RTC + TIME0) + 1500)
    q.w(RTC + SLP_TIMER1, 1 << 16)
    q.w(RTC + WAKEUP_STATE, TIMER << 15)
    q.w(RTC + STATE0, 1 << 31)
    q.cmd("clock_step 20000000")
    assert q.r(RTC + INT_RAW) & 1, "light: no wake interrupt"
    assert q.r(RTC + WAKEUP_CAUSE) == TIMER, "light: cause"
    q.w(RTC + INT_CLR, 1)
    print("ok  light sleep RTC timer")

    # Held pads: GPIO2 (RTC pad hold) high, GPIO40 (digital hold + deep-sleep autohold) low;
    # GPIO5 and GPIO13 are outputs left unheld. Traced on the first deep sleep below.
    q.w(GPIO_BASE + GPIO_ENABLE, (1 << 2) | (1 << 5) | (1 << 13))
    q.w(GPIO_BASE + GPIO_ENABLE1, 1 << (40 - 32))
    q.w(GPIO_BASE + GPIO_OUT, (1 << 2) | (1 << 5))
    q.w(RTC + PAD_HOLD, 1 << 2)
    q.w(RTC + DIG_PAD_HOLD, 1 << (40 - 21))
    q.w(RTC + DIG_ISO, 1 << 11)
    assert q.r(RTC + PAD_HOLD) == 1 << 2, "PAD_HOLD read back"

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
lines = open(TRACE).read().splitlines()
pads = [l.split("esp32s3_rtc_deep_pads ")[1] for l in lines if "esp32s3_rtc_deep_pads" in l]
assert pads[0] == f"held=0x{(1 << 2) | (1 << 40):x} held_high=0x4 unheld_out=0x{(1 << 5) | (1 << 13):x}", f"pads: {pads[0]}"
print("ok  held pads traced")
wakes = [l.split(" deep=")[1].split()[:2] for l in lines if "esp32s3_rtc_wake" in l]
os.unlink(TRACE)
assert wakes == [["0", "cause=0x8"], ["1", "cause=0x2"], ["1", "cause=0x1"], ["1", "cause=0x8"]], f"trace: wakes {wakes}"
print("ok  wakes traced")

# deep-timer-skip: a 10 min timer sleep wakes at once with the 10 min on the RTC counter.
q = QTest("-global", "driver=misc.esp32s3.rtc_cntl,property=deep-timer-skip,value=true")
try:
    q.w(RTC + TIME_UPDATE, 1 << 31)
    now = q.r(RTC + TIME0)
    q.w(RTC + SLP_TIMER0, now + 600 * 150000)
    q.w(RTC + SLP_TIMER1, 1 << 16)
    sleep(q, TIMER)
    check(q, "RTC timer, deep-timer-skip", TIMER)
    q.w(RTC + TIME_UPDATE, 1 << 31)
    assert q.r(RTC + TIME0) - now >= 600 * 150000, "skip: RTC counter didn't count the alarm"
    print("ok  RTC counter advanced by the skipped sleep")
finally:
    q.close()
    os.unlink(TRACE)

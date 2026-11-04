"""Minimal QMP client for a running x4prosim instance (run.sh: /tmp/x4prosim/NAME/qmp.sock).

One connection per call: QEMU serves one QMP client at a time, so watch.py, replay.py and
a shell can share an instance."""
import glob, json, os, socket

ROOT = "/tmp/x4prosim"


def call(d, name, **args):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(10)
    try:
        s.connect(os.path.join(d, "qmp.sock"))
        f = s.makefile("rwb")

        def reply():
            while True:
                line = f.readline()
                if not line:
                    raise ConnectionError("QMP closed")
                m = json.loads(line)
                if "event" not in m:
                    return m

        reply()     # greeting
        for cmd, a in (("qmp_capabilities", {}), (name, args)):
            f.write(json.dumps({"execute": cmd, "arguments": a}).encode() + b"\n")
            f.flush()
            r = reply()
            if "error" in r:
                raise RuntimeError(f"{cmd}: {r['error']['desc']}")
        return r["return"]
    finally:
        s.close()


def find(name=None):
    """Instance dir: a path, a NAME under /tmp/x4prosim, or the newest one that answers."""
    if name:
        return name if os.sep in name else os.path.join(ROOT, name)
    for d in sorted(glob.glob(ROOT + "/*/qmp.sock"), key=os.path.getmtime, reverse=True):
        try:
            call(os.path.dirname(d), "query-status")
            return os.path.dirname(d)
        except (OSError, ConnectionError):
            pass
    raise SystemExit(f"no running x4prosim instance under {ROOT} (start one with x4prosim/run.sh)")


def state(d):
    """Board state JSON (X4PROSIM.md "Board state")."""
    return json.loads(call(d, "qom-get", path="/machine", property="state"))


def qom_set(d, path, prop, value):
    call(d, "qom-set", path=path, property=prop, value=value)


def shot(d, png):
    call(d, "screendump", filename=os.path.abspath(png), device="panel", format="png")

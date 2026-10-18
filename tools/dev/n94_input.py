#!/usr/bin/env python3
"""Drive the iPhone 4S machine's touch panel and buttons over QMP, and take PNG screenshots.

  n94_input.py [--port 4444] COMMAND ...

Commands (coordinates are panel pixels, 0..639 x 0..959, portrait, origin top left):
  tap X Y [--hold MS]            press and lift finger 0
  swipe X1 Y1 X2 Y2 [--ms 400]   press, move in steps, rest, lift (iOS 6 reads a lift while moving as a flick)
  key NAME [--hold MS]           press a button: home, power, volup, voldown (the machine's keys:
                                 Home, End, Page Up, Page Down)
  shot OUT.png                   screendump of the panel, converted with Pillow
  wake                           press Home if the panel is dark (Auto-Lock), then wait 3 s
  status | quit                  QMP query-status / quit (the logs flush on quit)
  wait SECS                      sleep on the host (to chain steps in one call)
  keepalive [SECS [X Y]]         tap X Y (320 85, a navigation bar's title) every SECS (20) seconds, forever;
                                 run it on the second QMP server (--port 4445); it pauses while another run is busy

Several commands chain with ';' as one argument list: n94_input.py tap 320 800 \\; wait 2 \\; shot a.png
The machine takes the pointer as an absolute device (0..32767 over the panel) on the left button;
QEMU must run with -qmp tcp:127.0.0.1:PORT,server=on,wait=off.
"""
import argparse
import json
import os
import socket
import sys
import tempfile
import time

W, H = 640, 960
KEYS = {"home": "home", "power": "end", "volup": "pgup", "voldown": "pgdn"}


class Qmp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.f = self.s.makefile("rwb")
        self._read()                         # greeting
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise ConnectionError("QMP closed")
            msg = json.loads(line)
            if "event" not in msg:
                return msg

    def cmd(self, execute, **args):
        self.f.write(json.dumps({"execute": execute, "arguments": args}).encode() + b"\r\n")
        self.f.flush()
        r = self._read()
        if "error" in r:
            raise RuntimeError(f"{execute}: {r['error']}")
        return r.get("return")

    def pointer(self, x, y, down=None):
        ev = [{"type": "abs", "data": {"axis": "x", "value": round(x * 32767 / (W - 1))}},
              {"type": "abs", "data": {"axis": "y", "value": round(y * 32767 / (H - 1))}}]
        if down is not None:
            ev.append({"type": "btn", "data": {"down": down, "button": "left"}})
        self.cmd("input-send-event", events=ev)


def run(q, argv):
    op = argv[0]
    if op == "tap":
        ap = argparse.ArgumentParser(prog="tap")
        ap.add_argument("x", type=float); ap.add_argument("y", type=float)
        ap.add_argument("--hold", type=int, default=120)
        a = ap.parse_args(argv[1:])
        q.pointer(a.x, a.y)
        q.pointer(a.x, a.y, True)
        time.sleep(a.hold / 1000)
        q.pointer(a.x, a.y, False)
    elif op == "swipe":
        ap = argparse.ArgumentParser(prog="swipe")
        for n in ("x1", "y1", "x2", "y2"):
            ap.add_argument(n, type=float)
        ap.add_argument("--ms", type=int, default=400)
        ap.add_argument("--steps", type=int, default=16)
        a = ap.parse_args(argv[1:])
        q.pointer(a.x1, a.y1)
        q.pointer(a.x1, a.y1, True)
        for i in range(1, a.steps + 1):
            time.sleep(a.ms / 1000 / a.steps)
            q.pointer(a.x1 + (a.x2 - a.x1) * i / a.steps, a.y1 + (a.y2 - a.y1) * i / a.steps)
        time.sleep(0.25)
        q.pointer(a.x2, a.y2, False)
    elif op == "key":
        ap = argparse.ArgumentParser(prog="key")
        ap.add_argument("name", choices=sorted(KEYS))
        ap.add_argument("--hold", type=int, default=150)
        a = ap.parse_args(argv[1:])
        q.cmd("send-key", keys=[{"type": "qcode", "data": KEYS[a.name]}], **{"hold-time": a.hold})
        time.sleep(a.hold / 1000 + 0.05)
    elif op == "shot":
        from PIL import Image
        out = os.path.abspath(argv[1])
        fd, ppm = tempfile.mkstemp(suffix=".ppm", dir=os.path.dirname(out))
        os.close(fd)
        try:
            q.cmd("screendump", filename=ppm)
            Image.open(ppm).save(out)
        finally:
            os.unlink(ppm)
        print(out)
    elif op == "wake":
        # iOS 6 locks after a minute without a touch (Auto-Lock's default) and a wake with no touch
        # lasts ~10 s: press Home only when the panel is dark, so a lit app is not sent home.
        from PIL import Image
        fd, ppm = tempfile.mkstemp(suffix=".ppm")
        os.close(fd)
        try:
            q.cmd("screendump", filename=ppm)
            dark = Image.open(ppm).convert("L").getextrema()[1] == 0
        finally:
            os.unlink(ppm)
        if dark:
            run(q, ["key", "home"])
            time.sleep(3)
    elif op == "status":
        print(q.cmd("query-status"))
    elif op == "quit":
        q.cmd("quit")
    elif op == "wait":
        time.sleep(float(argv[1]))
    else:
        sys.exit(f"unknown command {op!r}")


def main():
    argv = sys.argv[1:]
    port = 4444
    if argv[:1] == ["--port"]:
        port, argv = int(argv[1]), argv[2:]
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return
    steps, cur = [], []
    for a in argv:
        if a == ";":
            steps.append(cur); cur = []
        else:
            cur.append(a)
    steps.append(cur)
    if steps[0][0] == "keepalive":
        a = steps[0][1:]
        keepalive(port, float(a[0]) if a else 20, a[1:3] if len(a) >= 3 else ["320", "85"])
        return
    q = Qmp(port)
    try:
        for st in steps:
            if st:
                if st[0] == "wait":          # the keepalive may tap while we only wait
                    if os.path.exists(BUSY):
                        os.unlink(BUSY)
                else:
                    open(BUSY, "w").close()
                run(q, st)
    finally:
        if os.path.exists(BUSY):
            os.unlink(BUSY)


BUSY = os.path.join(tempfile.gettempdir(), "n94_input.busy")


def keepalive(port, every, at):
    """Tap AT (a spot with no control: by default a navigation bar's title) every EVERY seconds on a
    second QMP server, so Auto-Lock never darkens the panel between steps; skipped while another
    n94_input.py run holds BUSY. Not the status bar: a tap there scrolls a list back to its top."""
    q = Qmp(port)
    while True:
        time.sleep(every)
        if os.path.exists(BUSY) and time.time() - os.path.getmtime(BUSY) < 600:
            continue
        run(q, ["tap", at[0], at[1], "--hold", "80"])


if __name__ == "__main__":
    main()

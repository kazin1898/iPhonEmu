#!/usr/bin/env python3
"""Where the host's time goes while the iPhone 4S machine runs: per QEMU thread, per MMIO region, in the GL bridge.

  n94_profile.py [--port 4544] [--pid PID] [--secs 8] [--what idle|scroll|apps] [--mmio] [--json OUT]

Windows only for the threads (Toolhelp, GetThreadTimes, GetThreadDescription: QEMU names its threads "CPU 0/TCG",
"CPU 1/TCG", the main loop is the process's first thread). For --secs of the chosen activity (a still home screen,
page swipes back and forth, or Ajustes/Calendario/Safari opened and closed in turn) it reports each thread's share of
one host core; with --mmio it also turns `memory_region_ops_read`/`_write` on for the window and counts the
accesses per region from the unit's qemu.log (the -D file `device boot` gives QEMU). A vCPU near 100 % is TCG-bound;
well under, the guest waits (WFI: its idle loop, a VBL, a device).
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import n94_bench  # noqa: E402
import n94_input  # noqa: E402

TH32CS_SNAPTHREAD = 0x4


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                ("dwFlags", wt.DWORD)]


def threads(pid):
    """{tid: (name, cpu seconds)} for the process's threads."""
    k32 = ctypes.windll.kernel32
    k32.OpenThread.restype = wt.HANDLE
    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    out = {}
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            h = k32.OpenThread(0x0800 | 0x1000, False, te.th32ThreadID)   # QUERY_LIMITED_INFORMATION
            if h:
                c, e, kt, ut = (ctypes.c_ulonglong() for _ in range(4))
                k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
                name = ctypes.c_wchar_p()
                try:
                    if k32.GetThreadDescription(h, ctypes.byref(name)) >= 0 and name.value:
                        nm = name.value
                    else:
                        nm = ""
                except AttributeError:
                    nm = ""
                out[te.th32ThreadID] = (nm, (kt.value + ut.value) / 1e7, c.value)
                k32.CloseHandle(h)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return out


def activity(q, what, secs, stop):
    run = lambda *a: n94_input.run(q, list(a))
    t0 = time.time()
    i = 0
    while time.time() - t0 < secs and not stop.is_set():
        if what == "scroll":
            x1, x2 = (560, 80) if i % 2 == 0 else (80, 560)
            run("swipe", str(x1), "500", str(x2), "500", "--ms", "200", "--steps", "8")
            time.sleep(0.6)
        elif what == "apps":
            x, y = n94_bench.ICONS[("ajustes", "calendario", "safari")[i % 3]]
            run("tap", str(x), str(y), "--hold", "80")
            time.sleep(2.5)
            run("key", "home")
            time.sleep(1.5)
        else:
            time.sleep(0.5)
        i += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=4544)
    ap.add_argument("--pid", type=int)
    ap.add_argument("--secs", type=float, default=8)
    ap.add_argument("--what", choices=("idle", "scroll", "apps"), default="scroll")
    ap.add_argument("--mmio", help="the unit's qemu.log (-D): count MMIO accesses per region over the window")
    ap.add_argument("--json")
    a = ap.parse_args()
    pid = a.pid or n94_bench.find_pid(a.port)
    q = n94_input.Qmp(a.port)
    q2 = n94_input.Qmp(a.port + 1)
    log_pos = os.path.getsize(a.mmio) if a.mmio else 0
    if a.mmio:
        for ev in ("memory_region_ops_read", "memory_region_ops_write"):
            q2.cmd("trace-event-set-state", name=ev, enable=True)
    tl0 = n94_bench.timeline(q2)
    before, w0 = threads(pid), time.time()
    stop = threading.Event()
    activity(q, a.what, a.secs, stop)
    after, w1 = threads(pid), time.time()
    if a.mmio:
        for ev in ("memory_region_ops_read", "memory_region_ops_write"):
            q2.cmd("trace-event-set-state", name=ev, enable=False)
    t_last = tl0[-1][1] if tl0 else 0
    new = sum(e[2] for e in n94_bench.timeline(q2) if e[1] > t_last)
    wall = w1 - w0
    rows = []
    first = min(after.values(), key=lambda v: v[2])[2] if after else 0
    for tid, (nm, cpu, created) in after.items():
        d = cpu - before.get(tid, (nm, 0.0, 0))[1]
        if d > 0.005 * wall:
            rows.append({"thread": nm or ("main" if created == first else f"tid {tid}"), "core_frac": round(d / wall, 3)})
    rows.sort(key=lambda r: -r["core_frac"])
    res = {"what": a.what, "wall_s": round(wall, 2), "new_frames": new, "fps": round(new / wall, 1),
           "process_core_frac": round(sum(r["core_frac"] for r in rows), 3), "threads": rows}
    if a.mmio:
        c = collections.Counter()
        with open(a.mmio, "rb") as f:
            f.seek(log_pos)
            for line in f:
                m = re.search(rb"memory_region_ops_(read|write) .* name '([^']+)'", line)
                if m:
                    c[m.group(2).decode()] += 1
        res["mmio_per_s"] = {k: round(v / wall) for k, v in c.most_common(15)}
    out = json.dumps(res, indent=1)
    if a.json:
        open(a.json, "w").write(out)
    print(out)


if __name__ == "__main__":
    main()

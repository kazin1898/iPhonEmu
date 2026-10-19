#!/usr/bin/env python3
"""Time the iPhone 4S machine the way a user sees it: boot to the lock screen, frames of an animation, launches.

  n94_bench.py boot NAME [--root DIR] [--port 4544] [--limit 400] [--cpus 2]
  n94_bench.py anim [--port 4544] [--pid PID] [--scrolls 6] [--apps ajustes,fotos,...] [--json OUT]

boot   starts a unit (ios_prep.py device boot NAME --display none --no-usb --detach) and polls a screendump once
       a second until the lock screen is up (the slider's knob lit on a dark track): seconds from QEMU's start.
       QEMU is left running for `anim`.
anim   on a unit at its lock screen or home screen: unlocks, then
       - idle: the QEMU process's CPU over 5 s of a still home screen (the floor the animations sit on);
       - scroll: SpringBoard's page swipes, left and right (--scrolls of them);
       - each app in --apps: tap its icon, wait for the panel to settle, Home back.
       For each gesture: the frames the panel latched (the display's frame-timeline, guest virtual time, one
       entry per VBL, new = a swap), frames/s over the animation's span (first new frame to last), the VBLs it
       held, and the QEMU process's CPU time per new frame (host cost: TCG, the GL bridge, everything). An
       app's launch time is the tap to the last new frame of its opening (the panel settled for 1.5 s).

The frame counts do not depend on screendumps (which steal QMP time and can halve the VBL): the display model
records every VBL itself (/machine/display frame-timeline, hw/arm/frame-timeline.h).
"""
import argparse
import ctypes
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import n94_input  # noqa: E402

# Home screen page 1 of a fresh 10B329 unit (4 x 4 grid, then the dock), panel pixels.
ICONS = {
    "mensagens": (90, 120), "calendario": (243, 120), "fotos": (397, 120), "camera": (548, 120),
    "videos": (90, 296), "mapas": (243, 296), "tempo": (397, 296), "passbook": (548, 296),
    "notas": (90, 472), "lembretes": (243, 472), "relogio": (397, 472), "gamecenter": (548, 472),
    "banca": (90, 648), "itunes": (243, 648), "appstore": (397, 648), "ajustes": (548, 648),
    "telefone": (90, 870), "mail": (243, 870), "safari": (397, 870), "musica": (548, 870),
}


def cpu_seconds(pid):
    """The process's user + kernel CPU time (Windows GetProcessTimes; os.times elsewhere is not per-PID)."""
    if os.name != "nt":
        with open(f"/proc/{pid}/stat") as f:
            v = f.read().rsplit(")", 1)[1].split()
        return (int(v[11]) + int(v[12])) / os.sysconf("SC_CLK_TCK")
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(0x1000, False, pid)              # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        raise OSError(f"no process {pid}")
    try:
        c, e, kt, ut = (ctypes.c_ulonglong() for _ in range(4))
        if not k32.GetProcessTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut)):
            raise OSError("GetProcessTimes")
        return (kt.value + ut.value) / 1e7
    finally:
        k32.CloseHandle(h)


def timeline(q):
    """[(seq, virt_ns, new)] of the display's frame-timeline, oldest first."""
    txt = q.cmd("qom-get", path="/machine/display", property="frame-timeline")
    out = []
    for line in txt.splitlines():
        p = line.split()
        if len(p) >= 3 and p[0].isdigit():
            out.append((int(p[0]), int(p[1]), int(p[2])))
    return out


def shot_luma(q, boxes):
    from PIL import Image, ImageStat
    fd, ppm = tempfile.mkstemp(suffix=".ppm")
    os.close(fd)
    try:
        q.cmd("screendump", filename=ppm)
        im = Image.open(ppm).convert("L")
        return [ImageStat.Stat(im.crop(b)).mean[0] for b in boxes]
    finally:
        os.unlink(ppm)


KNOB, TRACK = (60, 830, 150, 895), (170, 830, 480, 895)


def at_lock_screen(q):
    knob, track = shot_luma(q, [KNOB, TRACK])
    return knob > 90 and knob > 4 * track      # relative: the backlight scales both


def cmd_boot(a):
    py = sys.executable
    args = [py, "-I", os.path.join(REPO, "tools", "ios-prep", "ios_prep.py"), "device", "boot", a.name,
            "--display", "none", "--no-usb", "--detach", "--qmp", str(a.port), "--cpus", str(a.cpus)]
    if a.root:
        args += ["--root", a.root]
    t0 = time.time()
    subprocess.run(args, check=True)
    q = None
    while time.time() - t0 < a.limit:
        time.sleep(1)
        try:
            q = q or n94_input.Qmp(a.port)
            if at_lock_screen(q):
                t = time.time() - t0
                print(json.dumps({"boot_to_lock_s": round(t, 1), "unit": a.name, "cpus": a.cpus}))
                return
        except (OSError, ConnectionError, RuntimeError):
            q = None
    sys.exit(f"no lock screen within {a.limit} s")


def gesture(q, pid, act, settle=1.5, limit=8.0):
    """Run act(); wait until no new frame for `settle` s (at most `limit`); measure the frames since."""
    before = timeline(q)
    t_last = before[-1][1] if before else 0
    c0, w0 = cpu_seconds(pid), time.time()
    act()
    t_act = time.time()
    last_new, seen = time.time(), 0
    while time.time() - t_act < limit:
        time.sleep(0.25)
        tl = [e for e in timeline(q) if e[1] > t_last]
        n = sum(e[2] for e in tl)
        if n != seen:
            seen, last_new = n, time.time()
        elif time.time() - last_new >= settle:
            break
    c1, w1 = cpu_seconds(pid), time.time()
    tl = [e for e in timeline(q) if e[1] > t_last]
    new = [e for e in tl if e[2]]
    r = {"new_frames": len(new), "vbls": len(tl), "cpu_s": round(c1 - c0, 3), "wall_s": round(w1 - w0, 2)}
    if len(new) >= 2:
        span = (new[-1][1] - new[0][1]) / 1e9
        held = sum(1 for e in tl if new[0][1] <= e[1] <= new[-1][1] and not e[2])
        r.update(span_s=round(span, 3), fps=round((len(new) - 1) / span, 1) if span else None, held_in_span=held,
                 settle_s=round((new[-1][1] - t_last) / 1e9, 2) if t_last else None)
    if new:
        r["cpu_ms_per_frame"] = round((c1 - c0) * 1000 / len(new), 1)
    return r


def cmd_anim(a):
    q = n94_input.Qmp(a.port)
    pid = a.pid or find_pid(a.port)
    run = lambda *argv: n94_input.run(q, list(argv))
    res = {"pid": pid}
    # Light the panel and unlock (a no-op swipe on the home screen if already unlocked).
    run("key", "home"); time.sleep(1.5)
    if at_lock_screen(q):
        run("swipe", "105", "862", "610", "862")
        time.sleep(3)
    # No second Home: on the home screen's page 1 it opens Spotlight, whose blinking caret is a frame every
    # other VBL. After an unlock SpringBoard shows the page it last showed (page 1 after a boot).
    c0 = cpu_seconds(pid); time.sleep(5); res["idle_cpu_frac"] = round((cpu_seconds(pid) - c0) / 5, 3)
    scrolls = []
    for i in range(a.scrolls):                              # page 1 -> 2, 2 -> 1, ...
        x1, x2 = (560, 80) if i % 2 == 0 else (80, 560)
        scrolls.append(gesture(q, pid, lambda: run("swipe", str(x1), "500", str(x2), "500", "--ms", "200",
                                                   "--steps", "8")))
        if i == 0 and a.shots:
            run("shot", os.path.join(a.shots, "page2.png"))
    res["scroll"] = scrolls
    apps = {}
    for name in [s for s in a.apps.split(",") if s]:
        x, y = ICONS[name]
        r = gesture(q, pid, lambda: run("tap", str(x), str(y), "--hold", "80"), settle=1.5, limit=20)
        r["launch_s"] = r.get("settle_s")
        if a.shots:
            run("shot", os.path.join(a.shots, f"{name}.png"))
        r_home = gesture(q, pid, lambda: run("key", "home"), settle=1.0, limit=8)
        apps[name] = {"open": r, "close": r_home}
        time.sleep(1)
    res["apps"] = apps
    res["summary"] = summary(res)
    out = json.dumps(res, indent=1)
    if a.json:
        with open(a.json, "w") as f:
            f.write(out)
    print(json.dumps(res["summary"], indent=1))


def summary(res):
    s = {"idle_cpu_frac": res["idle_cpu_frac"]}
    sc = [r for r in res["scroll"] if r.get("fps")]
    if sc:
        s["scroll_fps_median"] = statistics.median(r["fps"] for r in sc)
        s["scroll_frames_median"] = statistics.median(r["new_frames"] for r in sc)
        s["scroll_cpu_ms_per_frame_median"] = statistics.median(r["cpu_ms_per_frame"] for r in sc)
    for name, r in res.get("apps", {}).items():
        o = r["open"]
        s[f"{name}_launch_s"] = o.get("launch_s")
        s[f"{name}_open_fps"] = o.get("fps")
        s[f"{name}_cpu_ms_per_frame"] = o.get("cpu_ms_per_frame")
    return s


def find_pid(port):
    """The QEMU whose command line serves QMP on this port."""
    if os.name == "nt":
        out = subprocess.run(["powershell", "-NoProfile", "-Command",
                              "Get-CimInstance Win32_Process -Filter \"Name='qemu-system-arm.exe'\" | "
                              "ForEach-Object { \"$($_.ProcessId) $($_.CommandLine)\" }"],
                             capture_output=True, text=True).stdout
    else:
        out = subprocess.run(["ps", "-eo", "pid,args"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if f"127.0.0.1:{port}," in line and "qemu-system-arm" in line:
            return int(line.split()[0])
    sys.exit(f"no QEMU with QMP on {port}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("boot")
    p.add_argument("name")
    p.add_argument("--root")
    p.add_argument("--port", type=int, default=4544)
    p.add_argument("--limit", type=float, default=400)
    p.add_argument("--cpus", type=int, default=2)
    p = sub.add_parser("anim")
    p.add_argument("--port", type=int, default=4544)
    p.add_argument("--pid", type=int)
    p.add_argument("--scrolls", type=int, default=6)
    p.add_argument("--apps", default="ajustes,fotos,mapas,safari,calendario")
    p.add_argument("--shots", help="a directory for a screenshot of each app once open")
    p.add_argument("--json")
    a = ap.parse_args()
    {"boot": cmd_boot, "anim": cmd_anim}[a.cmd](a)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Every picture the panel showed during a gesture, as a contact sheet: the proof a UI moved without a glitch.

  n94_frames.py [--port 4544] [--max 240] OUT_DIR GESTURE...

GESTURE is an n94_input.py command chain (`swipe 560 500 80 500 --ms 200`, `tap 548 648`, `key home`, ...). The
display model keeps the next --max pictures it latches in memory (QOM capture-frames: writing 2.4 MB frames at 60 a
second would slow what they record) until the gesture and --settle seconds are over, then writes them (capture-dump)
as NNNN_<guest ms>.ppm into OUT_DIR, and this makes OUT_DIR/sheet.png: every frame small, with the milliseconds since
the one before. A frame repeated, a gap of more than one VBL (17 ms) and a frame much darker than its neighbours
(a black flash) are listed.
"""
import argparse
import glob
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import n94_input  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=4544)
    ap.add_argument("--max", type=int, default=240)
    ap.add_argument("--settle", type=float, default=1.5)
    ap.add_argument("--cols", type=int, default=12)
    ap.add_argument("out")
    ap.add_argument("gesture", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    from PIL import Image, ImageDraw, ImageStat
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    for f in glob.glob(os.path.join(out, "*.ppm")):
        os.unlink(f)
    q = n94_input.Qmp(a.port)
    q.cmd("qom-set", path="/machine/display", property="capture-frames", value=a.max)
    steps, cur = [], []
    for w in a.gesture:
        if w == ";":
            steps.append(cur); cur = []
        else:
            cur.append(w)
    steps.append(cur)
    for st in steps:
        if st:
            n94_input.run(q, st)
    time.sleep(a.settle)
    q.cmd("qom-set", path="/machine/display", property="capture-dump", value=out.replace("\\", "/"))
    files = sorted(glob.glob(os.path.join(out, "*.ppm")))
    if not files:
        print("no frame latched")
        return
    ts = [int(os.path.basename(f).split("_")[1].split(".")[0]) for f in files]
    thumbs, lum, prev, issues = [], [], None, []
    for i, f in enumerate(files):
        im = Image.open(f).convert("RGB")
        small = im.resize((im.width // 5, im.height // 5))
        lum.append(ImageStat.Stat(small.convert("L")).mean[0])
        if prev is not None and small.tobytes() == prev:
            issues.append(f"frame {i}: same picture as the one before")
        prev = small.tobytes()
        thumbs.append(small)
    for i in range(1, len(ts)):
        if ts[i] - ts[i - 1] > 25 and i < len(ts) - 1:
            issues.append(f"frame {i}: {ts[i] - ts[i - 1]} ms after the one before")
    for i in range(1, len(lum) - 1):
        if lum[i] < 0.6 * min(lum[i - 1], lum[i + 1]):
            issues.append(f"frame {i}: much darker than its neighbours ({lum[i]:.0f} against {lum[i - 1]:.0f}/{lum[i + 1]:.0f})")
    tw, th = thumbs[0].size
    cols = min(a.cols, len(thumbs))
    rows = -(-len(thumbs) // cols)
    sheet = Image.new("RGB", (cols * (tw + 4) + 4, rows * (th + 22) + 4), (13, 17, 23))
    d = ImageDraw.Draw(sheet)
    for i, t in enumerate(thumbs):
        x, y = 4 + (i % cols) * (tw + 4), 4 + (i // cols) * (th + 22)
        sheet.paste(t, (x, y))
        dt = "" if i == 0 else f"+{ts[i] - ts[i - 1]}"
        d.text((x + 2, y + th + 4), f"{i} {dt}", fill=(201, 209, 217))
    sheet.save(os.path.join(out, "sheet.png"))
    span = (ts[-1] - ts[0]) / 1000 if len(ts) > 1 else 0
    print(f"{len(files)} frames over {span:.2f} s ({(len(files) - 1) / span if span else 0:.1f}/s); sheet {out}\\sheet.png")
    for s in issues:
        print("  " + s)


if __name__ == "__main__":
    main()

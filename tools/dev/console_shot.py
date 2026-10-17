#!/usr/bin/env python3
"""Render the tail of a serial log as a PNG, a "screenshot" of the console for the progress folder.

  console_shot.py LOG OUT.png [--lines 60] [--title "M2: launchd on the restore ramdisk"]
                  [--grep PATTERN ...]

Until the machine has a panel, the kernel's console is the only picture of what the guest does;
once it has one, QEMU's `screendump` takes over and this stays for the serial side. --grep keeps
only matching lines (useful to show the milestones of a long log) before taking the tail.
Needs Pillow; uses Consolas when it is installed (Windows), else Pillow's default font.
"""
import argparse
import datetime
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

FONTS = [r"C:\Windows\Fonts\consola.ttf", "/System/Library/Fonts/Menlo.ttc",
         "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"]


def load_font(size):
    for p in FONTS:
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("out")
    ap.add_argument("--lines", type=int, default=60)
    ap.add_argument("--cols", type=int, default=132)
    ap.add_argument("--title", default="")
    ap.add_argument("--grep", action="append", default=[], help="keep only lines matching (repeatable)")
    args = ap.parse_args()

    with open(args.log, "r", encoding="utf-8", errors="replace") as f:
        lines = [l.rstrip("\r\n") for l in f]
    total = len(lines)
    if args.grep:
        pats = [re.compile(p) for p in args.grep]
        lines = [l for l in lines if any(p.search(l) for p in pats)]
    # Consecutive repeats (a driver polling, a resource wait) collapse to one line with a count.
    collapsed = []
    for l in lines:
        if collapsed and collapsed[-1][0] == l:
            collapsed[-1][1] += 1
        else:
            collapsed.append([l, 1])
    lines = [l + (f"   (x{n})" if n > 1 else "") for l, n in collapsed]
    lines = lines[-args.lines:]
    lines = [l[:args.cols] + ("…" if len(l) > args.cols else "") for l in lines]

    font = load_font(15)
    title_font = load_font(18)
    cw, ch = font.getbbox("M")[2], 20
    pad = 16
    header = 64   # title line, then the log name / line count / time stamp on its own line
    width = pad * 2 + cw * (args.cols + 2)
    height = header + pad * 2 + ch * max(len(lines), 1)
    img = Image.new("RGB", (width, height), (24, 24, 28))
    d = ImageDraw.Draw(img)
    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    title = args.title or os.path.basename(args.log)
    d.text((pad, 10), title, font=title_font, fill=(230, 230, 235))
    meta = f"{os.path.basename(args.log)}  ·  {total} linhas  ·  {stamp}"
    d.text((pad, 36), meta, font=font, fill=(140, 140, 150))
    d.line((pad, header - 4, width - pad, header - 4), fill=(60, 60, 70))
    y = header + pad
    for l in lines:
        color = (200, 200, 205)
        if re.search(r"panic|Failing|failed|error|Unformatted|waiting for root", l, re.I):
            color = (255, 120, 110)
        elif re.search(r"launchd\[1\] has started|BSD root|iBoot version|restored|has started up", l):
            color = (120, 220, 140)
        d.text((pad, y), l, font=font, fill=color)
        y += ch
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    img.save(args.out)
    print(f"{args.out}: {width}x{height}, {len(lines)} lines of {total}")


if __name__ == "__main__":
    main()

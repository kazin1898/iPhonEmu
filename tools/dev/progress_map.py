#!/usr/bin/env python3
"""Render docs/progress/progress.json as a progress map (SVG) and refresh the README's Progress block.

  progress_map.py [--json docs/progress/progress.json] [--svg docs/progress/progress-map.svg]
                  [--readme README.md] [--history docs/progress/history.md]

The map is one row per subsystem: ten blocks, filled green for the percent done (a half-filled block
for the remainder), the percent and a note. The README block between <!-- progress:start --> and
<!-- progress:end --> gets the overall badge, the image and the milestones; history.md gets one line
per run with the overall figure, so the curve can be read back later. Overall = weighted mean.
"""
import argparse
import datetime
import html
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GREEN, HALF, EMPTY, TEXT, DIM, BG, HEAD = "#3fb950", "#7bd389", "#30363d", "#e6edf3", "#8b949e", "#0d1117", "#c9d1d9"


def overall(data):
    num = sum(i["percent"] * i.get("weight", 1) for g in data["groups"] for i in g["items"])
    den = sum(i.get("weight", 1) for g in data["groups"] for i in g["items"])
    return round(num / den, 1)


def blocks(percent, n=10):
    full = int(percent // (100 / n))
    half = 1 if percent % (100 / n) >= (100 / n) / 2 and full < n else 0
    return full, half, n - full - half


def render_svg(data, path):
    rows = sum(len(g["items"]) + 1 for g in data["groups"])
    w, row_h, top = 980, 26, 56
    h = top + rows * row_h + 24
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" '
         f'font-family="Segoe UI, Helvetica, Arial, sans-serif" font-size="13">',
         f'<rect width="{w}" height="{h}" rx="8" fill="{BG}"/>',
         f'<text x="20" y="28" fill="{TEXT}" font-size="17" font-weight="600">iPhonEmu progress map</text>',
         f'<text x="20" y="46" fill="{DIM}">overall {overall(data)}% · {html.escape(data["goal"])} · updated {data["updated"]}</text>']
    y = top
    for g in data["groups"]:
        y += row_h
        o.append(f'<text x="20" y="{y}" fill="{HEAD}" font-weight="600">{html.escape(g["name"])}</text>')
        for it in g["items"]:
            y += row_h
            o.append(f'<text x="36" y="{y}" fill="{TEXT}">{html.escape(it["name"])}</text>')
            full, half, empty = blocks(it["percent"])
            x = 380
            for kind, count in (("full", full), ("half", half), ("empty", empty)):
                for _ in range(count):
                    fill = {"full": GREEN, "half": HALF, "empty": EMPTY}[kind]
                    o.append(f'<rect x="{x}" y="{y - 13}" width="18" height="16" rx="3" fill="{fill}"/>')
                    x += 21
            o.append(f'<text x="{x + 8}" y="{y}" fill="{TEXT}" font-weight="600">{it["percent"]}%</text>')
            o.append(f'<text x="{x + 52}" y="{y}" fill="{DIM}" font-size="12">{html.escape(it.get("note", ""))}</text>')
    o.append("</svg>")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(o))


def render_readme_block(data):
    pct = overall(data)
    color = "red" if pct < 25 else "orange" if pct < 50 else "yellow" if pct < 75 else "brightgreen"
    ms = " · ".join(("✅ " if m["done"] else "⬜ ") + m["name"] for m in data["milestones"])
    bar = lambda p: "🟩" * blocks(p)[0] + "🟨" * blocks(p)[1] + "⬜" * blocks(p)[2]
    lines = ["<!-- progress:start -->",
             f"![overall](https://img.shields.io/badge/overall-{pct}%25-{color}) "
             f"![milestones](https://img.shields.io/badge/milestones-{sum(m['done'] for m in data['milestones'])}%2F{len(data['milestones'])}-blue) "
             f"![updated](https://img.shields.io/badge/updated-{data['updated']}-lightgrey)",
             "",
             "[![progress map](docs/progress/progress-map.svg)](docs/progress/progress.json)",
             "",
             f"Milestones: {ms}. History: `docs/progress/history.md`. Figures are estimates of work done toward the",
             "goal, weighted by effort (`docs/progress/progress.json`); `tools/dev/progress_map.py` regenerates this block.",
             "", "| Area | Progress | |", "|---|---|---|"]
    for g in data["groups"]:
        for it in g["items"]:
            lines.append(f"| {g['name']}: {it['name']} | {bar(it['percent'])} | {it['percent']}% |")
    lines.append("<!-- progress:end -->")
    return "\n".join(lines)


def update_readme(path, block):
    with open(path, encoding="utf-8") as f:
        s = f.read()
    a, b = s.find("<!-- progress:start -->"), s.find("<!-- progress:end -->")
    if a >= 0 and b >= 0:
        s = s[:a] + block + s[b + len("<!-- progress:end -->"):]
    else:
        anchor = "\n## Status\n"
        i = s.find(anchor)
        s = s[:i + len(anchor)] + "\n" + block + "\n" + s[i + len(anchor):] if i >= 0 else s + "\n" + block + "\n"
    with open(path, "w", encoding="utf-8") as f:
        f.write(s)


def append_history(path, data):
    line = f"| {data['updated']} | {overall(data)}% | " + ", ".join(
        f"{it['name']} {it['percent']}%" for g in data["groups"] for it in g["items"] if it["percent"] > 0) + " |\n"
    new = not os.path.exists(path)
    with open(path, "a", encoding="utf-8") as f:
        if new:
            f.write("# Progress history\n\nOne row per regeneration of the progress map.\n\n| Date | Overall | Non-zero areas |\n|---|---|---|\n")
        f.write(line)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", default=os.path.join(ROOT, "docs", "progress", "progress.json"))
    ap.add_argument("--svg", default=os.path.join(ROOT, "docs", "progress", "progress-map.svg"))
    ap.add_argument("--readme", default=os.path.join(ROOT, "README.md"))
    ap.add_argument("--history", default=os.path.join(ROOT, "docs", "progress", "history.md"))
    ap.add_argument("--no-history", action="store_true")
    args = ap.parse_args()
    with open(args.json, encoding="utf-8") as f:
        data = json.load(f)
    data.setdefault("updated", datetime.date.today().isoformat())
    render_svg(data, args.svg)
    update_readme(args.readme, render_readme_block(data))
    if not args.no_history:
        append_history(args.history, data)
    print(f"overall {overall(data)}%  ->  {args.svg}, README block, history")


if __name__ == "__main__":
    main()

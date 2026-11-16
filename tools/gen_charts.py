#!/usr/bin/env python3
"""Draw the charts of README.md and docs/DESIGN.md: docs/<name>-light.svg and -dark.svg.

    python3 tools/gen_charts.py

The numbers are the ones in those files' tables and text; change them in both places.
"""
import math
import os

FONT = 'system-ui, -apple-system, "Segoe UI", sans-serif'
WIDTH = 760

THEMES = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", muted="#898781", grid="#e1e0d9", axis="#c3c2b7",
                  border="rgba(11,11,11,0.10)", s1="#2a78d6", s2="#eb6834", s3="#1baf7a", soft="#86b6ef"),
    "dark": dict(surface="#1a1a19", ink="#f0efec", ink2="#c3c2b7", muted="#898781", grid="#2c2c2a", axis="#383835",
                 border="rgba(255,255,255,0.10)", s1="#3987e5", s2="#d95926", s3="#199e70", soft="#184f95"),
}


class Svg:
    def __init__(self, theme, height, title, subtitle):
        self.t = theme
        self.h = height
        self.out = []
        self.rect(0.5, 0.5, WIDTH - 1, height - 1, theme["surface"], rx=10, stroke=theme["border"])
        self.text(28, 34, title, size=15, weight=600)
        self.text(28, 54, subtitle, size=12, fill=theme["ink2"])

    def rect(self, x, y, w, h, fill, rx=0, stroke=None, opacity=None):
        extra = (f' stroke="{stroke}"' if stroke else "") + (f' fill-opacity="{opacity}"' if opacity else "")
        self.out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" fill="{fill}"{extra}/>')

    def line(self, x1, y1, x2, y2, stroke, width=1):
        self.out.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{stroke}" '
                        f'stroke-width="{width}" stroke-linecap="round"/>')

    def dot(self, x, y, fill, r=5):
        # the ring in the surface colour keeps a marker legible where it sits on a line
        self.out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r}" fill="{fill}" stroke="{self.t["surface"]}" stroke-width="2"/>')

    def path(self, points, stroke=None, fill="none", opacity=None, width=2):
        d = "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in points) + ("Z" if fill != "none" else "")
        extra = f' fill-opacity="{opacity}"' if opacity else ""
        s = f' stroke="{stroke}" stroke-width="{width}" stroke-linejoin="round" stroke-linecap="round"' if stroke else ""
        self.out.append(f'<path d="{d}" fill="{fill}"{extra}{s}/>')

    def text(self, x, y, s, size=12, fill=None, weight=400, anchor="start"):
        s = s.replace("&", "&amp;").replace("<", "&lt;")
        self.out.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" font-weight="{weight}" text-anchor="{anchor}" '
                        f'fill="{fill or self.t["ink"]}">{s}</text>')

    def save(self, path, label):
        body = "\n".join(self.out)
        with open(path, "w") as f:
            f.write(f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{self.h}" viewBox="0 0 {WIDTH} {self.h}" '
                    f'font-family=\'{FONT}\' role="img" aria-label="{label}">\n{body}\n</svg>\n')


def speed(t):
    """Keys per second by device and API: positions on a log axis, ranges as two joined dots."""
    groups = [
        ("NVIDIA", [("RTX 4090, CUDA", 16.6e9, None, "16.6 G"), ("RTX 4090, Vulkan", 14.5e9, 15.3e9, "14.5 to 15.3 G"),
                    ("RTX 3090, CUDA", 6.84e9, None, "6.84 G"), ("RTX 3090, Vulkan", 6.09e9, None, "6.09 G"),
                    ("RTX 2060 SUPER, CUDA", 2.83e9, None, "2.83 G"), ("GTX 1650 SUPER, CUDA", 1.66e9, None, "1.66 G")]),
        ("Apple M5 Pro", [("Metal, first 20 s", 1.7e9, None, "1.7 G"), ("Metal, heat-soaked", 1.0e9, 1.5e9, "1.0 to 1.5 G"),
                          ("CPU, 18 cores", 150e6, None, "150 M")]),
        ("AMD", [("RX 6800 XT, ROCm", 3.82e9, None, "3.82 G"), ("RX 6700 XT, Vulkan", 1.95e9, None, "1.95 G"),
                 ("Radeon 610M, ROCm", 108e6, None, "108 M"), ("Radeon 610M, Vulkan", 93e6, None, "93 M")]),
    ]
    left, right, lo, hi = 190, 610, 7.5, 10.5
    rows = sum(len(items) for _, items in groups)
    top = 78
    height = top + 26 * len(groups) + 28 * rows + 50
    s = Svg(t, height, "Keys tested per second", "By device and API, measured with --bench. Each step on the axis is ten times the last.")
    x = lambda v: left + (math.log10(v) - lo) / (hi - lo) * (right - left)
    bottom = height - 50
    for decade, label in ((8, "100 M"), (9, "1 G"), (10, "10 G")):
        s.line(x(10 ** decade), top, x(10 ** decade), bottom, t["grid"])
        s.text(x(10 ** decade), bottom + 18, label, size=11, fill=t["muted"], anchor="middle")
    s.text((left + right) / 2, bottom + 36, "keys per second, logarithmic", size=11, fill=t["muted"], anchor="middle")
    y = top
    for name, items in groups:
        s.text(28, y + 17, name, size=12, weight=600)
        y += 26
        for label, a, b, value in items:
            cy = y + 14
            s.text(left - 14, cy + 4, label, size=12, fill=t["ink2"], anchor="end")
            s.line(left, cy, right, cy, t["grid"])
            if b:  # a range: a dot stretched from its low end to its high end
                s.line(x(a), cy, x(b), cy, t["surface"], width=14)
                s.line(x(a), cy, x(b), cy, t["s1"], width=10)
            else:
                s.dot(x(a), cy, t["s1"])
            s.text(x(b or a) + 12, cy + 4, value, size=12, weight=600)
            y += 28
    return s, "Keys tested per second by device and API"


def wait(t):
    """Average wait for one key against prefix length, with the band between the 50% and 95% waits."""
    series = [("Apple M5 Pro, 1.5 G keys/s", 1.5e9, t["s1"]), ("NVIDIA RTX 4090, 16.5 G keys/s", 16.5e9, t["s2"])]
    lengths = list(range(6, 13))
    left, right, top, bottom, lo, hi = 92, 620, 92, 352, -1.6, 9.9
    s = Svg(t, 412, "How long a prefix takes", "Lines: the average wait for one key. Bands: half of all searches end below the band, 95% below its top.")
    x = lambda n: left + (n - 6) / 6 * (right - left)
    y = lambda sec: bottom - (math.log10(sec) - lo) / (hi - lo) * (bottom - top)
    for sec, label in ((1, "1 second"), (60, "1 minute"), (3600, "1 hour"), (86400, "1 day"), (30 * 86400, "1 month"),
                       (365.25 * 86400, "1 year"), (100 * 365.25 * 86400, "100 years")):
        s.line(left, y(sec), right, y(sec), t["grid"])
        s.text(left - 10, y(sec) + 4, label, size=11, fill=t["muted"], anchor="end")
    for n in lengths:
        s.text(x(n), bottom + 20, str(n), size=11, fill=t["muted"], anchor="middle")
    s.text((left + right) / 2, bottom + 40, "prefix length, characters", size=11, fill=t["muted"], anchor="middle")
    lx = 28
    for name, rate, colour in series:
        s.line(lx, 72, lx + 18, 72, colour, width=2)
        s.dot(lx + 9, 72, colour, r=4)
        s.text(lx + 26, 76, name, size=12, fill=t["ink2"])
        lx += 26 + 6.4 * len(name) + 28
    for name, rate, colour in series:
        mean = [32.0 ** n / rate for n in lengths]
        band = [(x(n), y(m * math.log(20))) for n, m in zip(lengths, mean)] + \
               [(x(n), y(m * math.log(2))) for n, m in reversed(list(zip(lengths, mean)))]
        s.path(band, fill=colour, opacity=0.10)
        s.path([(x(n), y(m)) for n, m in zip(lengths, mean)], stroke=colour)
        for n, m in zip(lengths, mean):
            s.dot(x(n), y(m), colour, r=4)
        years = mean[-1] / (365.25 * 86400)
        s.text(right + 14, y(mean[-1]) + 4, f"{years:.0f} years" if years >= 10 else f"{years:.1f} years", size=12, weight=600)
    return s, "Average wait for one key by prefix length, for two GPUs"


def energy(t):
    """Where the energy of one key pair goes on the RTX 4090: one stacked bar."""
    parts = [("950 wide multiply-adds", 38, "38 nJ", t["s1"]), ("850 other instructions", 12, "12 nJ", t["s2"]),
             ("scratch memory traffic", 5, "5 to 8 nJ", t["s3"])]
    left, right, top = 28, 732, 78
    s = Svg(t, 176, "Where a key pair's energy goes on an RTX 4090", "About 55 nanojoules per pair at the card's 450 W limit.")
    total = sum(p[1] for p in parts)
    px = left
    for i, (name, value, label, colour) in enumerate(parts):
        w = value / total * (right - left) - (2 if i < len(parts) - 1 else 0)  # 2 px of surface between segments
        if i == len(parts) - 1:  # the data end is rounded, the baseline end square
            e = px + w
            s.out.append(f'<path d="M{px:.1f},{top} H{e - 4:.1f} Q{e:.1f},{top} {e:.1f},{top + 4} V{top + 20} '
                         f'Q{e:.1f},{top + 24} {e - 4:.1f},{top + 24} H{px:.1f} Z" fill="{colour}"/>')
        else:
            s.rect(px, top, w, 24, colour)
        px += w + 2
    lx = left
    for name, value, label, colour in parts:
        s.rect(lx, top + 44, 10, 10, colour, rx=2)
        s.text(lx + 16, top + 53, label, size=12, weight=600)
        s.text(lx + 16, top + 70, name, size=12, fill=t["ink2"])
        lx += 236
    return s, "Energy per key pair on an RTX 4090 by where it goes"


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs")
    os.makedirs(out, exist_ok=True)
    for name, draw in (("speed", speed), ("wait", wait), ("energy", energy)):
        for mode, theme in THEMES.items():
            chart, label = draw(theme)
            chart.save(os.path.join(out, f"{name}-{mode}.svg"), label)


if __name__ == "__main__":
    main()

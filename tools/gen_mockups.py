"""Render the UI spec's mockups (docs/uispec/ui-spec.md#making-a-mockup) from docs/uispec/mockups/screens.json.

Each screen is written as an SVG at 400 x 300 with the spec's layout and type (Inter 4.0, Material Design Icons
7.4.47 path data), rasterised with resvg and thresholded to 1 bit (above 150/255 is white). The accepted mockups
were rasterised with librsvg 2.58 (rsvg-convert); resvg matches them in every position and text extent, with a
pixel here and there on glyph edges. Fonts and icon paths are downloaded into a cache directory.

    python -m venv %TEMP%\\gl_gen\\venv
    %TEMP%\\gl_gen\\venv\\Scripts\\python -m pip install pillow resvg-py
    %TEMP%\\gl_gen\\venv\\Scripts\\python tools/gen_mockups.py [NAME ...] [--out DIR]

Without NAME it renders every screen in screens.json. PNGs (and the SVGs) go to OUT_DIR (default
%TEMP%/gl_mock/out), never over the accepted mockups; for each name that has an accepted mockup the number of
differing pixels is printed. A new or changed screen: add or edit its entry in screens.json, render it, and copy
the PNG into docs/uispec/mockups/ once it is accepted.

screens.json maps a screen name (the screen_kind_t value, house_* for the house screens) to one of:
  {"layout": "problems", "rows": [[icon, name, state], ...], "stale": bool}   up to 6 rows
  {"layout": "ok", "title": str, "lines": [str, str], "stale": bool}
  {"layout": "device", "icon": str, "text": str}
  {"layout": "unpaired", "icon": str, "title": str, "lines": [str, str]}
Icons are MDI names without the "mdi-" prefix.
"""

import argparse
import io
import json
import os
import re
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape

import resvg_py
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parent.parent
MOCKUPS = ROOT / "docs" / "uispec" / "mockups"
CACHE = Path(os.environ.get("GL_FONT_CACHE", Path(tempfile.gettempdir()) / "gl_gen" / "fonts"))
INTER_ZIP = "https://github.com/rsms/inter/releases/download/v4.0/Inter-4.0.zip"
INTER_FILES = ["Inter-Regular.otf", "Inter-SemiBold.otf", "Inter-Bold.otf"]  # extras/otf/ in the zip
MDI_JS = "https://cdn.jsdelivr.net/npm/@mdi/js@7.4.47/mdi.js"
WIDTH, HEIGHT = 400, 300
THRESHOLD = 150  # grey above this becomes white

# Layout and type: docs/uispec/ui-spec.md#layout-and-type
ROW_HEIGHT, ROW_ICON_X, ROW_ICON_PX, ROW_NAME_X, ROW_RIGHT_X, ROW_BASELINE_DY = 50, 12, 30, 56, 388, 33
NAME_FONT, STATE_FONT = (600, 24), (400, 22)
OK_ICON, OK_ICON_PX, OK_TITLE, OK_LINES, OK_TITLE_FONT, OK_LINE_FONT = (125, 22), 150, 214, (248, 274), (700, 32), (400, 18)
BADGE_CENTER, BADGE_RADIUS, BADGE_RING, BADGE_ICON_PX = (ROW_RIGHT_X - 17, 25), 17, 4, 24
DEVICE_ICON_TOP, DEVICE_ICON_PX, DEVICE_TEXT, DEVICE_FONT = 38, 150, 240, (700, 30)
PAIR_ICON_TOP, PAIR_ICON_PX, PAIR_TITLE, PAIR_LINES, PAIR_LINE_FONT = 24, 120, 180, (218, 250), (400, 22)


def cached(name, url):
    path = CACHE / name
    if not path.exists():
        CACHE.mkdir(parents=True, exist_ok=True)
        print(f"downloading {url}", file=sys.stderr)
        urllib.request.urlretrieve(url, path)
    return path


def inter_fonts():
    paths = [CACHE / "inter4" / f for f in INTER_FILES]
    if not all(p.exists() for p in paths):
        with zipfile.ZipFile(cached("Inter-4.0.zip", INTER_ZIP)) as z:
            for f in INTER_FILES:
                (CACHE / "inter4").mkdir(parents=True, exist_ok=True)
                (CACHE / "inter4" / f).write_bytes(z.read(f"extras/otf/{f}"))
    return [str(p) for p in paths]


_mdi = None


def mdi_path(name):
    global _mdi
    if _mdi is None:
        text = cached("mdi-7.4.47.js", MDI_JS).read_text(encoding="utf-8")
        _mdi = dict(re.findall(r'var (mdi\w+) = "([^"]+)"', text))
    key = "mdi" + "".join(part.capitalize() for part in name.split("-"))
    if key not in _mdi:
        sys.exit(f"unknown icon: {name}")
    return _mdi[key]


def icon(name, x, y, size, fill="#000"):
    return f'<path transform="translate({x} {y}) scale({size / 24})" fill="{fill}" d="{mdi_path(name)}"/>'


def text(s, x, baseline, font, anchor="start"):
    weight, size = font
    return (f'<text x="{x}" y="{baseline}" font-family="Inter" font-weight="{weight}" font-size="{size}" '
            f'text-anchor="{anchor}" fill="#000">{escape(s)}</text>')


def problems(screen):
    out = []
    rows = screen["rows"]
    if len(rows) > 6:
        sys.exit("at most 6 rows: the sixth is the overflow row")
    for i, (icon_name, name, state) in enumerate(rows):
        top = i * ROW_HEIGHT
        out.append(icon(icon_name, ROW_ICON_X, top + (ROW_HEIGHT - ROW_ICON_PX) // 2, ROW_ICON_PX))
        out.append(text(name, ROW_NAME_X, top + ROW_BASELINE_DY, NAME_FONT))
        if state:
            out.append(text(state, ROW_RIGHT_X, top + ROW_BASELINE_DY, STATE_FONT, "end"))
        if i < len(rows) - 1:  # dotted separator below, between rows only: 2 px on, 3 off
            y = top + ROW_HEIGHT - 1
            out += [f'<rect x="{x}" y="{y}" width="2" height="1" fill="#000"/>' for x in range(ROW_NAME_X, ROW_RIGHT_X, 5)]
    return out


def all_ok(screen):
    out = [icon("check-circle", *OK_ICON, OK_ICON_PX), text(screen["title"], WIDTH // 2, OK_TITLE, OK_TITLE_FONT, "middle")]
    out += [text(s, WIDTH // 2, y, OK_LINE_FONT, "middle") for s, y in zip(screen["lines"], OK_LINES)]
    return out


def device(screen):
    return [icon(screen["icon"], (WIDTH - DEVICE_ICON_PX) // 2, DEVICE_ICON_TOP, DEVICE_ICON_PX),
            text(screen["text"], WIDTH // 2, DEVICE_TEXT, DEVICE_FONT, "middle")]


def unpaired(screen):
    out = [icon(screen["icon"], (WIDTH - PAIR_ICON_PX) // 2, PAIR_ICON_TOP, PAIR_ICON_PX),
           text(screen["title"], WIDTH // 2, PAIR_TITLE, DEVICE_FONT, "middle")]
    out += [text(s, WIDTH // 2, y, PAIR_LINE_FONT, "middle") for s, y in zip(screen["lines"], PAIR_LINES)]
    return out


def stale_badge():
    cx, cy = BADGE_CENTER
    half = BADGE_ICON_PX / 2
    return [f'<circle cx="{cx}" cy="{cy}" r="{BADGE_RADIUS + BADGE_RING}" fill="#fff"/>',
            f'<circle cx="{cx}" cy="{cy}" r="{BADGE_RADIUS}" fill="#000"/>',
            icon("sync", cx - half, cy - half, BADGE_ICON_PX, "#fff")]


LAYOUTS = {"problems": problems, "ok": all_ok, "device": device, "unpaired": unpaired}


def to_svg(screen):
    body = LAYOUTS[screen["layout"]](screen)
    if screen.get("stale"):
        body += stale_badge()
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}">'
            f'<rect width="{WIDTH}" height="{HEIGHT}" fill="#fff"/>' + "".join(body) + "</svg>")


def rasterise(svg, fonts):
    png = resvg_py.svg_to_bytes(svg_string=svg, width=WIDTH, height=HEIGHT, skip_system_fonts=True, font_files=fonts)
    grey = Image.open(io.BytesIO(bytes(png))).convert("L")
    return grey.point(lambda v: 255 if v > THRESHOLD else 0).convert("1")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("names", nargs="*", help="screens to render (default: all in screens.json)")
    parser.add_argument("--out", default=str(Path(tempfile.gettempdir()) / "gl_mock" / "out"))
    args = parser.parse_args()
    screens = json.loads((MOCKUPS / "screens.json").read_text(encoding="utf-8"))
    names = args.names or list(screens)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    fonts = inter_fonts()
    for name in names:
        if name not in screens:
            sys.exit(f"{name} is not in screens.json")
        svg = to_svg(screens[name])
        (out / f"{name}.svg").write_text(svg, encoding="utf-8")
        image = rasterise(svg, fonts)
        image.save(out / f"{name}.png")
        accepted = MOCKUPS / f"{name}.png"
        if accepted.exists():
            diff = ImageChops.difference(image.convert("L"), Image.open(accepted).convert("L"))
            changed = sum(1 for v in diff.get_flattened_data() if v)
            print(f"{name:22} {changed:5} px differ from the accepted mockup")
        else:
            print(f"{name:22} new")
    print(f"written to {out}")


if __name__ == "__main__":
    main()

"""Generate main/bitmaps.c and main/bitmaps.h: the fonts and icons of the proper screen (DESIGN.md 8).

The text is Inter (SIL OFL), the icons are Material Design Icons 7.4.47 (the set HA uses, Apache 2.0),
both rendered with Pillow at the sizes of the accepted mockups and thresholded to 1 bit. The fonts are
downloaded into a cache directory; only the generated C is committed. Run it again after changing a size,
a charset or the icon table, e.g. in a venv with Pillow:

    python -m venv %TEMP%\\gl_gen\\venv
    %TEMP%\\gl_gen\\venv\\Scripts\\python -m pip install pillow
    %TEMP%\\gl_gen\\venv\\Scripts\\python tools/gen_bitmaps.py

Line icon ids are the 2 hex digits at the start of a dataset line (DESIGN.md 5.1). The table is append-only:
HA's templates (ha/custom_templates/greenlight.jinja) use the ids, so never reorder or remove an entry.
"""

import os
import sys
import tempfile
import urllib.request
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
CACHE = Path(os.environ.get("GL_FONT_CACHE", Path(tempfile.gettempdir()) / "gl_gen" / "fonts"))
FONTS = {
    "inter.ttf": "https://github.com/google/fonts/raw/main/ofl/inter/Inter%5Bopsz,wght%5D.ttf",
    "mdi.ttf": "https://cdn.jsdelivr.net/npm/@mdi/font@7.4.47/fonts/materialdesignicons-webfont.ttf",
}
THRESHOLD = 128  # anti-aliased coverage below this grey level becomes black

HUNGARIAN = "áéíóöőúüűÁÉÍÓÖŐÚÜŰ"
BASIC = "".join(chr(c) for c in range(0x20, 0x7F)) + HUNGARIAN
# HA names can hold any letter: Latin-1, Latin Extended-A and a few punctuation marks
FULL = BASIC + "".join(chr(c) for c in range(0xA0, 0x180)) + "…·–—‘’„“”"

# name: (weight, pixel size, charset). Sizes and weights fitted to the mockups.
TEXT_FONTS = {
    "name":   (700, 24, FULL),   # problem line name, "további N"
    "state":  (600, 22, FULL),   # problem line state
    "ok":     (700, 32, BASIC),  # "Minden OK"
    "big":    (700, 30, BASIC),  # device screen text
    "detail": (500, 18, BASIC),  # the two lines under "Minden OK"
}

# Line icons, indexed by icon id (append-only, see above). 30 px like the mockups.
LINE_ICON_PX = 30
LINE_ICONS = [
    ("alert-circle", 0xF0028),  # 00: fallback for an unknown id or a malformed line
    ("dots-horizontal", 0xF01D8),  # 01: overflow line, "további N"
    ("door-open", 0xF081C),
    ("door-sliding-open", 0xF1820),
    ("window-open", 0xF05B1),
    ("window-shutter-open", 0xF111E),
    ("garage-open-variant", 0xF12D4),
    ("gate-open", 0xF116A),
    ("lock-open-variant", 0xF0FC6),
    ("shield-off", 0xF099E),
    ("shield-home", 0xF068A),
    ("motion-sensor", 0xF0D91),
    ("cctv", 0xF07AE),
    ("lightbulb-on", 0xF06E8),
    ("ceiling-light", 0xF0769),
    ("lamp", 0xF06B5),
    ("led-strip-variant", 0xF1051),
    ("power-plug", 0xF06A5),
    ("television", 0xF0502),
    ("remote-tv", 0xF0EC5),
    ("speaker", 0xF04C3),
    ("fan", 0xF0210),
    ("air-conditioner", 0xF001B),
    ("air-humidifier", 0xF1099),
    ("radiator", 0xF0438),
    ("heat-pump", 0xF1A43),
    ("fireplace", 0xF0E2E),
    ("stove", 0xF04DE),
    ("iron", 0xF1824),
    ("coffee-maker", 0xF109F),
    ("kettle", 0xF05FA),
    ("printer", 0xF042A),
    ("washing-machine", 0xF072A),
    ("tumble-dryer", 0xF0917),
    ("dishwasher", 0xF0AAC),
    ("robot-vacuum", 0xF070D),
    ("robot-mower", 0xF11F7),
    ("ev-station", 0xF05F1),
    ("car", 0xF010B),
    ("water", 0xF058C),
    ("faucet", 0xF1B29),
    ("valve", 0xF1066),
    ("water-pump", 0xF058F),
    ("water-boiler", 0xF0F92),
    ("sprinkler-variant", 0xF1060),
    ("pool", 0xF0606),
    ("fridge", 0xF0290),
    ("snowflake", 0xF0717),
    ("thermometer", 0xF050F),
    ("water-percent", 0xF058E),
    ("weather-pouring", 0xF0596),
    ("weather-windy", 0xF059D),
    ("smoke-detector-variant", 0xF180B),
    ("fire", 0xF0238),
    ("molecule-co", 0xF12FE),
    ("molecule-co2", 0xF07E4),
    ("gas-burner", 0xF1A1B),
    ("trash-can", 0xF0A79),
    ("mailbox", 0xF06EE),
    ("battery-low", 0xF12A1),
    ("wifi-off", 0xF05AA),
]

# Other icons: C name, MDI codepoint, pixel size
ICONS = [
    ("sync_badge", 0xF04E6, 24),          # stale badge, drawn white on a black disc
    ("check_circle", 0xF05E0, 152),       # Minden OK
    ("access_point", 0xF0003, 150),       # Hálózatkeresés, Csatlakozás
    ("sync", 0xF04E6, 150),               # Szinkronizálás
    ("alert_outline", 0xF002A, 150),      # Csatlakozási hiba, Ismeretlen hiba
    ("battery_outline", 0xF008E, 150),    # Akkumulátor lemerült
]


def fetch(name):
    path = CACHE / name
    if not path.exists():
        CACHE.mkdir(parents=True, exist_ok=True)
        print(f"downloading {FONTS[name]}")
        urllib.request.urlretrieve(FONTS[name], path)
    return str(path)


def render(font, ch):
    """Ink of one character drawn at the origin on the baseline: (bitmap rows, x, y, w, h); x and y are the
    offsets of its top left corner from the pen position on the baseline."""
    size = font.size * 4 + 20
    im = Image.new("L", (size, size), 255)
    ox, oy = size // 4, size * 3 // 4
    ImageDraw.Draw(im).text((ox, oy), ch, font=font, fill=0, anchor="ls")
    px = im.load()
    ink = [(x, y) for y in range(size) for x in range(size) if px[x, y] < THRESHOLD]
    if not ink:
        return [], 0, 0, 0, 0
    x0, x1 = min(x for x, _ in ink), max(x for x, _ in ink)
    y0, y1 = min(y for _, y in ink), max(y for _, y in ink)
    w, h = x1 - x0 + 1, y1 - y0 + 1
    rows = []
    for y in range(y0, y1 + 1):
        row = bytearray((w + 7) // 8)
        for x in range(x0, x1 + 1):
            if px[x, y] < THRESHOLD:
                row[(x - x0) // 8] |= 0x80 >> ((x - x0) % 8)
        rows.append(bytes(row))
    return rows, x0 - ox, y0 - oy, w, h


def hexbytes(data, indent="    "):
    out = []
    for i in range(0, len(data), 16):
        out.append(indent + " ".join(f"0x{b:02X}," for b in data[i : i + 16]))
    return "\n".join(out)


def main():
    inter, mdi = fetch("inter.ttf"), fetch("mdi.ttf")
    c = ['/* Generated by tools/gen_bitmaps.py: do not edit. Inter (SIL OFL 1.1), Material Design Icons 7.4.47',
         ' * (Apache 2.0), rendered at the sizes of the accepted mockups (DESIGN.md 8). */', "",
         '#include "bitmaps.h"', ""]
    h = ['/* Generated by tools/gen_bitmaps.py: do not edit. */', "#pragma once", "", "#include <stdint.h>", "",
         "/* A glyph: its bitmap is h rows of (w + 7) / 8 bytes, MSB first, 1 = black, at bits + off. x, y: offset of",
         " * its top left corner from the pen position on the baseline. adv: advance in 1/16 px. */",
         "typedef struct {", "    uint16_t cp;", "    uint8_t  w, h;", "    int8_t   x, y;", "    uint16_t adv;",
         "    uint32_t off;", "} bm_glyph_t;", "",
         "/* A font: glyphs sorted by code point. cap: height of a capital letter (H), for vertical centring. */",
         "typedef struct {", "    const bm_glyph_t *glyph;", "    uint16_t          count;", "    uint8_t           cap;",
         "    const uint8_t    *bits;", "} bm_font_t;", "",
         "/* An icon, cropped to its ink: h rows of (w + 7) / 8 bytes, MSB first, 1 = black. */",
         "typedef struct {", "    uint8_t        w, h;", "    const uint8_t *bits;", "} bm_icon_t;", ""]

    for name, (weight, px, charset) in TEXT_FONTS.items():
        font = ImageFont.truetype(inter, px)
        font.set_variation_by_axes([min(max(px, 14), 32), weight])
        bits, glyphs = bytearray(), []
        for ch in sorted(set(charset)):
            rows, x, y, w, gh = render(font, ch)
            glyphs.append((ord(ch), w, gh, x, y, round(font.getlength(ch) * 16), len(bits)))
            bits += b"".join(rows)
        cap = render(font, "H")[4]
        c.append(f"static const uint8_t {name}_bits[] = {{\n{hexbytes(bits)}\n}};")
        c.append(f"static const bm_glyph_t {name}_glyphs[] = {{")
        c += [f"    {{0x{g[0]:04X}, {g[1]}, {g[2]}, {g[3]}, {g[4]}, {g[5]}, {g[6]}}}," for g in glyphs]
        c.append("};")
        c.append(f"const bm_font_t bm_font_{name} = {{{name}_glyphs, {len(glyphs)}, {cap}, {name}_bits}};\n")
        h.append(f"extern const bm_font_t bm_font_{name};  /* Inter {weight}, {px} px */")
        print(f"font {name}: {len(glyphs)} glyphs, {len(bits)} bytes, cap {cap}")
    h.append("")

    def icon(cname, cp, px):
        font = ImageFont.truetype(mdi, px)
        rows, _, _, w, ih = render(font, chr(cp))
        if not rows:
            sys.exit(f"icon {cname} (U+{cp:X}) is empty: not in this MDI version?")
        c.append(f"static const uint8_t {cname}_bits[] = {{\n{hexbytes(b''.join(rows))}\n}};")
        return f"{{{w}, {ih}, {cname}_bits}}"

    line = [icon(f"line_{i:02x}", cp, LINE_ICON_PX) for i, (_, cp) in enumerate(LINE_ICONS)]
    c.append("const bm_icon_t bm_line_icon[BM_LINE_ICONS] = {")
    c += [f"    {v},  /* {i:02X} {LINE_ICONS[i][0]} */" for i, v in enumerate(line)]
    c.append("};\n")
    h.append(f"/* Line icons by icon id (DESIGN.md 5.1), {LINE_ICON_PX} px. */")
    h.append(f"#define BM_LINE_ICONS {len(LINE_ICONS)}")
    h += [f"#define BM_ICON_{n.upper().replace('-', '_')} 0x{i:02X}" for i, (n, _) in enumerate(LINE_ICONS[:2])]
    h.append("extern const bm_icon_t bm_line_icon[BM_LINE_ICONS];\n")
    for cname, cp, px in ICONS:
        c.append(f"const bm_icon_t bm_icon_{cname} = {icon(cname, cp, px)};\n")
        h.append(f"extern const bm_icon_t bm_icon_{cname};  /* {px} px */")

    (ROOT / "main" / "bitmaps.c").write_text("\n".join(c) + "\n", encoding="utf-8", newline="\n")
    (ROOT / "main" / "bitmaps.h").write_text("\n".join(h) + "\n", encoding="utf-8", newline="\n")
    print(f"line icons: {len(LINE_ICONS)}; wrote main/bitmaps.c, main/bitmaps.h")


if __name__ == "__main__":
    main()

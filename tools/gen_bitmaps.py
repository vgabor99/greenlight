"""Generate main/screen/bitmaps.c and main/screen/bitmaps.h: the fonts and icons of the proper screen (docs/display.md).

The text is Inter (SIL OFL), the icons are Material Design Icons 7.4.47 (the set HA uses, Apache 2.0),
both rendered with Pillow at the sizes of the accepted mockups and thresholded to 1 bit. The fonts are
downloaded into a cache directory; only the generated C is committed. Run it again after changing a size,
a charset or the icon table, e.g. in a venv with Pillow:

    python -m venv %TEMP%\\gl_gen\\venv
    %TEMP%\\gl_gen\\venv\\Scripts\\python -m pip install pillow
    %TEMP%\\gl_gen\\venv\\Scripts\\python tools/gen_bitmaps.py

Line icon ids are the 2 hex digits at the start of a dataset line (docs/data-model.md#line-icons). The table is append-only:
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
    "hint":   (500, 22, BASIC),  # the two instruction lines of "Párosítás"
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
    ("remote", 0xF0454),
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
    ("gesture_tap_hold", 0xF0D77, 120),   # Párosítás
    ("access_point", 0xF0003, 150),       # Hálózatkeresés, Csatlakozás
    ("sync", 0xF04E6, 150),               # Szinkronizálás
    ("alert_outline", 0xF002A, 150),      # Csatlakozási hiba, Ismeretlen hiba
    ("battery_outline", 0xF008E, 150),    # Akkumulátor lemerült
]


def fetch(name):
    """Path of a font file, downloaded into the cache on first use."""
    path = CACHE / name
    if not path.exists():
        CACHE.mkdir(parents=True, exist_ok=True)
        print(f"downloading {FONTS[name]}")
        urllib.request.urlretrieve(FONTS[name], path)
    return str(path)


def render_ink(font, char):
    """Ink of one character drawn at the origin on the baseline: (bitmap rows, x, y, width, height); x and y are
    the offsets of its top left corner from the pen position on the baseline."""
    size = font.size * 4 + 20
    image = Image.new("L", (size, size), 255)
    origin_x, origin_y = size // 4, size * 3 // 4
    ImageDraw.Draw(image).text((origin_x, origin_y), char, font=font, fill=0, anchor="ls")
    pixels = image.load()
    ink = [(x, y) for y in range(size) for x in range(size) if pixels[x, y] < THRESHOLD]
    if not ink:
        return [], 0, 0, 0, 0
    left, right = min(x for x, _ in ink), max(x for x, _ in ink)
    top, bottom = min(y for _, y in ink), max(y for _, y in ink)
    width, height = right - left + 1, bottom - top + 1
    rows = []
    for y in range(top, bottom + 1):
        row = bytearray((width + 7) // 8)
        for x in range(left, right + 1):
            if pixels[x, y] < THRESHOLD:
                row[(x - left) // 8] |= 0x80 >> ((x - left) % 8)
        rows.append(bytes(row))
    return rows, left - origin_x, top - origin_y, width, height


def hex_bytes(data, indent="    "):
    """C initializer lines, 16 bytes each."""
    return "\n".join(indent + " ".join(f"0x{b:02X}," for b in data[i : i + 16]) for i in range(0, len(data), 16))


HEADER_PREAMBLE = """/* Generated by tools/gen_bitmaps.py: do not edit. */
#pragma once

#include <stdint.h>

/* A glyph: its bitmap is height rows of (width + 7) / 8 bytes, MSB first, 1 = black, at the font's
 * bits + bits_offset. x_offset, y_offset: of its top left corner from the pen position on the baseline. */
typedef struct {
    uint16_t code_point;
    uint8_t  width, height;
    int8_t   x_offset, y_offset;
    uint16_t advance_16ths;  /* pen advance in 1/16 px */
    uint32_t bits_offset;
} bitmap_glyph_t;

/* A font: glyphs sorted by code point. cap_height: height of a capital letter (H), for vertical centring. */
typedef struct {
    const bitmap_glyph_t *glyphs;
    uint16_t              glyph_count;
    uint8_t               cap_height;
    const uint8_t        *bits;
} bitmap_font_t;

/* An icon, cropped to its ink: height rows of (width + 7) / 8 bytes, MSB first, 1 = black. */
typedef struct {
    uint8_t        width, height;
    const uint8_t *bits;
} bitmap_icon_t;
"""

SOURCE_PREAMBLE = """/* Generated by tools/gen_bitmaps.py: do not edit. Inter (SIL OFL 1.1), Material Design Icons 7.4.47
 * (Apache 2.0), rendered at the sizes of the accepted mockups (docs/display.md). */

#include "screen/bitmaps.h"
"""


def write_fonts(inter_path, source, header):
    for name, (weight, pixel_size, charset) in TEXT_FONTS.items():
        font = ImageFont.truetype(inter_path, pixel_size)
        font.set_variation_by_axes([min(max(pixel_size, 14), 32), weight])  # optical size, weight
        bits, glyphs = bytearray(), []
        for char in sorted(set(charset)):
            rows, x, y, width, height = render_ink(font, char)
            glyphs.append((ord(char), width, height, x, y, round(font.getlength(char) * 16), len(bits)))
            bits += b"".join(rows)
        cap_height = render_ink(font, "H")[4]
        source.append(f"static const uint8_t {name}_bits[] = {{\n{hex_bytes(bits)}\n}};")
        source.append(f"static const bitmap_glyph_t {name}_glyphs[] = {{")
        source += [f"    {{0x{g[0]:04X}, {g[1]}, {g[2]}, {g[3]}, {g[4]}, {g[5]}, {g[6]}}}," for g in glyphs]
        source.append("};")
        source.append(f"const bitmap_font_t bitmap_font_{name} = "
                      f"{{{name}_glyphs, {len(glyphs)}, {cap_height}, {name}_bits}};\n")
        header.append(f"extern const bitmap_font_t bitmap_font_{name};  /* Inter {weight}, {pixel_size} px */")
        print(f"font {name}: {len(glyphs)} glyphs, {len(bits)} bytes, cap {cap_height}")
    header.append("")


def write_icons(mdi_path, source, header):
    def icon_initializer(c_name, code_point, pixel_size):
        rows, _, _, width, height = render_ink(ImageFont.truetype(mdi_path, pixel_size), chr(code_point))
        if not rows:
            sys.exit(f"icon {c_name} (U+{code_point:X}) is empty: not in this MDI version?")
        source.append(f"static const uint8_t {c_name}_bits[] = {{\n{hex_bytes(b''.join(rows))}\n}};")
        return f"{{{width}, {height}, {c_name}_bits}}"

    line_icons = [icon_initializer(f"line_{i:02x}", cp, LINE_ICON_PX) for i, (_, cp) in enumerate(LINE_ICONS)]
    source.append("const bitmap_icon_t bitmap_line_icons[BITMAP_LINE_ICON_COUNT] = {")
    source += [f"    {v},  /* {i:02X} {LINE_ICONS[i][0]} */" for i, v in enumerate(line_icons)]
    source.append("};\n")
    header.append(f"/* Line icons by icon id (docs/data-model.md#line-icons), {LINE_ICON_PX} px. */")
    header.append(f"#define BITMAP_LINE_ICON_COUNT {len(LINE_ICONS)}")
    header += [f"#define BITMAP_LINE_ICON_{name.upper().replace('-', '_')} 0x{i:02X}"
               for i, (name, _) in enumerate(LINE_ICONS[:2])]
    header.append("extern const bitmap_icon_t bitmap_line_icons[BITMAP_LINE_ICON_COUNT];\n")
    for c_name, code_point, pixel_size in ICONS:
        source.append(f"const bitmap_icon_t bitmap_icon_{c_name} = {icon_initializer(c_name, code_point, pixel_size)};\n")
        header.append(f"extern const bitmap_icon_t bitmap_icon_{c_name};  /* {pixel_size} px */")


def main():
    source, header = [SOURCE_PREAMBLE], [HEADER_PREAMBLE]
    write_fonts(fetch("inter.ttf"), source, header)
    write_icons(fetch("mdi.ttf"), source, header)
    (ROOT / "main" / "screen" / "bitmaps.c").write_text("\n".join(source) + "\n", encoding="utf-8", newline="\n")
    (ROOT / "main" / "screen" / "bitmaps.h").write_text("\n".join(header) + "\n", encoding="utf-8", newline="\n")
    print(f"line icons: {len(LINE_ICONS)}; wrote main/screen/bitmaps.c, main/screen/bitmaps.h")


if __name__ == "__main__":
    main()

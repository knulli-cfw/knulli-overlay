#!/usr/bin/env python3
"""Generate src/ov_atlas.h: an 8-bit alpha atlas with the digits, '%' and the
status icons used by the overlay.

Run on a build host (needs Pillow + DejaVu Sans Bold); the generated header is
committed so the target build has no python/font dependency.

    ./tools/gen_atlas.py [output.h]
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
]
# Glyphs are rasterised big and scaled down at draw time, so the overlay stays
# sharp on 720p panels without needing a runtime font rasteriser.
FONT_PX = 48
ICON_PX = 56
SS = 4           # icon supersampling factor
PAD = 2          # transparent padding between cells
ATLAS_W = 1024

# Printable ASCII: the digits and '%' for the panels, the rest for the
# notification text.  Anything outside this range is drawn as '?'.
TEXT_GLYPHS = "".join(chr(c) for c in range(0x20, 0x7f))


def find_font():
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return path
    raise SystemExit("DejaVu Sans Bold not found; edit FONT_CANDIDATES")


def render_text_glyphs(font):
    """Return [(name, image, advance, top)] for every printable ASCII glyph."""
    out = []
    for ch in TEXT_GLYPHS:
        # bbox of the inked area, so cells are tight and metrics are exact
        box = font.getbbox(ch)
        w, h = max(box[2] - box[0], 0), max(box[3] - box[1], 0)
        if w and h:
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((-box[0], -box[1]), ch, font=font, fill=255)
        else:
            img = Image.new("L", (1, 1), 0)      # space and friends
        out.append(("CH_%02X" % ord(ch), img, font.getlength(ch), box[1]))
    return out


def draw_speaker(d, s):
    """Speaker cone plus two sound waves, drawn at supersampled scale s."""
    def r(x0, y0, x1, y1):
        d.rectangle([x0 * s, y0 * s, x1 * s, y1 * s], fill=255)

    # body: a small rectangle that flares out into a triangle
    r(2, 20, 9, 36)
    d.polygon([(9 * s, 28 * s), (22 * s, 8 * s), (22 * s, 48 * s)], fill=255)
    # two arcs to the right of the cone
    for radius, width in ((10, 4), (20, 4)):
        d.arc([(16 - radius) * s, (28 - radius) * s,
               (16 + radius) * s, (28 + radius) * s],
              start=-52, end=52, fill=255, width=width * s)


def draw_sun(d, s):
    """Sun disc with eight rays (brightness)."""
    import math

    def circle(cx, cy, rad, fill=255, width=0):
        box = [(cx - rad) * s, (cy - rad) * s, (cx + rad) * s, (cy + rad) * s]
        if width:
            d.ellipse(box, outline=fill, width=width * s)
        else:
            d.ellipse(box, fill=fill)

    cx = cy = 28
    circle(cx, cy, 11, width=4)
    for i in range(8):
        a = math.radians(i * 45)
        x0, y0 = cx + 16 * math.cos(a), cy + 16 * math.sin(a)
        x1, y1 = cx + 25 * math.cos(a), cy + 25 * math.sin(a)
        d.line([x0 * s, y0 * s, x1 * s, y1 * s], fill=255, width=4 * s)


def draw_bolt(d, s):
    """Lightning bolt, drawn inside a 56x56 cell (charging indicator)."""
    d.polygon([(32 * s, 4 * s), (14 * s, 31 * s), (25 * s, 31 * s),
               (22 * s, 52 * s), (42 * s, 23 * s), (30 * s, 23 * s)], fill=255)


def draw_wifi(d, s):
    """Three arcs over a dot, all centred on the dot (wifi enabled)."""
    cx, cy = 28, 46
    for radius in (14, 24, 34):
        box = [(cx - radius) * s, (cy - radius) * s,
               (cx + radius) * s, (cy + radius) * s]
        # PIL measures from 3 o'clock clockwise: 270 is straight up.
        d.arc(box, start=228, end=312, fill=255, width=6 * s)
    d.ellipse([(cx - 5) * s, (cy - 5) * s, (cx + 5) * s, (cy + 5) * s], fill=255)


def draw_bluetooth(d, s):
    """The Bluetooth rune: a stem crossed by two triangles."""
    def line(x0, y0, x1, y1):
        d.line([x0 * s, y0 * s, x1 * s, y1 * s], fill=255, width=5 * s)

    line(28, 5, 28, 51)
    line(28, 5, 41, 18)
    line(41, 18, 15, 38)
    line(28, 51, 41, 38)
    line(41, 38, 15, 18)


def render_icons():
    out = []
    for name, fn in (("ICON_SPEAKER", draw_speaker), ("ICON_SUN", draw_sun),
                     ("ICON_BOLT", draw_bolt), ("ICON_WIFI", draw_wifi),
                     ("ICON_BLUETOOTH", draw_bluetooth)):
        big = Image.new("L", (ICON_PX * SS, ICON_PX * SS), 0)
        fn(ImageDraw.Draw(big), SS)
        img = big.resize((ICON_PX, ICON_PX), Image.LANCZOS)
        img = img.crop(img.getbbox())
        out.append((name, img, img.width, 0))
    return out


def pack(cells):
    """Row-packer; cells are already small so a shelf packer is plenty."""
    x = y = row_h = 0
    placed = []
    for name, img, advance, top in cells:
        if x + img.width + PAD > ATLAS_W:
            x, y, row_h = 0, y + row_h + PAD, 0
        placed.append((name, img, advance, top, x, y))
        x += img.width + PAD
        row_h = max(row_h, img.height)
    return placed, y + row_h


def main():
    src_dir = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src")
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        src_dir, "ov_atlas.h")
    out_c = out_path[:-2] + ".c" if out_path.endswith(".h") else out_path + ".c"

    font_path = find_font()
    font = ImageFont.truetype(font_path, FONT_PX)
    cells = render_text_glyphs(font) + render_icons()
    placed, atlas_h = pack(cells)

    atlas = Image.new("L", (ATLAS_W, atlas_h), 0)
    for _, img, _, _, x, y in placed:
        atlas.paste(img, (x, y))

    data = atlas.tobytes()
    lines = []
    for i in range(0, len(data), 20):
        lines.append("    " + " ".join("0x%02x," % b for b in data[i:i + 20]))

    entries = []
    for idx, (name, img, advance, top, x, y) in enumerate(placed):
        entries.append("    /* %-13s */ { %4d, %4d, %3d, %3d, %6.2ff, %3d },"
                       % (name, x, y, img.width, img.height, advance, top))

    # Only the icons get a named constant; text goes through the ASCII table.
    enum = "\n".join("    OV_G_%s = %d," % (n[0], i)
                      for i, n in enumerate(placed) if n[0].startswith("ICON_"))
    index = {n[0]: i for i, n in enumerate(placed)}
    ascii_rows = []
    for base in range(0x20, 0x7f, 16):
        row = ["%3d," % index["CH_%02X" % c]
               for c in range(base, min(base + 16, 0x7f))]
        ascii_rows.append("    " + " ".join(row))

    with open(out_path, "w") as f:
        f.write("""/* Generated by tools/gen_atlas.py -- do not edit.
 *
 * Font: %s @ %dpx, icons drawn procedurally.
 */
#ifndef OV_ATLAS_H_INCLUDED
#define OV_ATLAS_H_INCLUDED

#define OV_ATLAS_W %d
#define OV_ATLAS_H %d
/* Cap height of the digits, in atlas pixels: text is scaled from this. */
#define OV_ATLAS_CAP %d

typedef struct {
    short x, y, w, h;   /* cell in the atlas, in pixels */
    float advance;      /* pen advance for text glyphs, in atlas pixels */
    short top;          /* rows below the ascender line where the cell starts */
} ov_glyph;

enum {
%s
    OV_G_COUNT = %d
};

extern const ov_glyph ov_atlas_glyphs[OV_G_COUNT];

/* Printable ASCII, 0x20..0x7e, in order. */
#define OV_ASCII_FIRST 0x20
#define OV_ASCII_LAST  0x7e

extern const unsigned char ov_atlas_ascii[OV_ASCII_LAST - OV_ASCII_FIRST + 1];

/* Glyph index for a character, or the one for '?' when out of range.  A
 * function, not a macro: call sites pass expressions with side effects and a
 * macro would evaluate them more than once. */
static inline int ov_glyph_for(int c)
{
    if (c < OV_ASCII_FIRST || c > OV_ASCII_LAST)
        c = '?';
    return ov_atlas_ascii[c - OV_ASCII_FIRST];
}

#define OV_GLYPH_FOR(c) ov_glyph_for(c)
#define OV_G_D0      ov_glyph_for('0')
#define OV_G_PERCENT ov_glyph_for('%%')
#define OV_G_SPACE   ov_glyph_for(' ')

extern const unsigned char ov_atlas_pixels[OV_ATLAS_W * OV_ATLAS_H];

#endif /* OV_ATLAS_H_INCLUDED */
""" % (os.path.basename(font_path), FONT_PX, ATLAS_W, atlas_h,
       next(img.height for n, img, _, _, _, _ in placed if n == "CH_30"),
       enum, len(placed)))

    # The tables live in one translation unit: at 200KB, a copy per includer
    # would be a real cost in the injected library.
    with open(out_c, "w") as f:
        f.write("""/* Generated by tools/gen_atlas.py -- do not edit. */
#include "ov_atlas.h"

const ov_glyph ov_atlas_glyphs[OV_G_COUNT] = {
%s
};

const unsigned char ov_atlas_ascii[OV_ASCII_LAST - OV_ASCII_FIRST + 1] = {
%s
};

const unsigned char ov_atlas_pixels[OV_ATLAS_W * OV_ATLAS_H] = {
%s
};
""" % ("\n".join(entries), "\n".join(ascii_rows), "\n".join(lines)))

    print("wrote %s and %s (%dx%d, %d glyphs, %d bytes of pixels)"
          % (out_path, out_c, ATLAS_W, atlas_h, len(placed), len(data)))


if __name__ == "__main__":
    main()

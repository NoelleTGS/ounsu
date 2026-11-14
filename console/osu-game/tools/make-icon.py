#!/usr/bin/env python3
"""Make the launcher icon (96x96) from osu!'s own resources, composed like osu.Game OsuLogo: a disc with a
vertical #ff66ab -> #cc5289 gradient under the white Menu/logo texture. Nothing is shipped: the logo comes from
the resources extracted on this PC (fsroot/osu-resources).
usage: make-icon.py <osu-resources dir> <out.png>"""
import sys
from PIL import Image, ImageDraw

src, out = sys.argv[1], sys.argv[2]
logo = Image.open(f"{src}/osu.Game.Resources.Textures.Menu.logo.png").convert("RGBA")
w, h = logo.size
x0, y0, x1, y1 = logo.getbbox()

top, bottom = (0xff, 0x66, 0xab), (0xcc, 0x52, 0x89)
gradient = Image.new("RGBA", (w, h))
px = gradient.load()
for y in range(h):
    t = min(max((y - y0) / max(1, y1 - y0), 0.0), 1.0)
    c = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,)
    for x in range(w):
        px[x, y] = c

mask = Image.new("L", (w, h), 0)
inset = (x1 - x0) * 0.02  # keep the pink inside the white ring
ImageDraw.Draw(mask).ellipse((x0 + inset, y0 + inset, x1 - inset, y1 - inset), fill=255)
icon = Image.new("RGBA", (w, h), (0, 0, 0, 0))
icon.paste(gradient, (0, 0), mask)
icon.alpha_composite(logo)
icon.crop((x0, y0, x1, y1)).resize((96, 96), Image.LANCZOS).save(out)
print(f"icon: {out}")

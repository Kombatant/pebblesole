#!/usr/bin/env python3
"""Generate weather icon art for the Pebblesole watchface.

Outputs 60x60 RGBA PNGs into resources/images/. Palette restricted to colors
that survive Pebble's 64-color (2-bit-per-channel) quantization: pure white,
mid gray, light blue (cloud), steel blue (rain), red-ish (none used here),
yellow (sun/lightning). Transparent background.
"""
import math
import os
from PIL import Image, ImageDraw

SIZE = 60
OUT = os.path.join(os.path.dirname(__file__), "..", "resources", "images")
os.makedirs(OUT, exist_ok=True)

# Pebble-safe colors (each channel a multiple of 0x55).
WHITE = (255, 255, 255, 255)
CLOUD = (170, 170, 170, 255)        # mid gray cloud body
CLOUD_DARK = (85, 85, 85, 255)
CLOUD_HI = (255, 255, 255, 255)     # cloud highlight
RAIN = (85, 170, 255, 255)          # light blue rain streaks
SNOW = (255, 255, 255, 255)
SUN = (255, 255, 0, 255)            # yellow
BOLT = (255, 255, 0, 255)
FOG = (170, 170, 170, 255)
T = (0, 0, 0, 0)


def new():
    img = Image.new("RGBA", (SIZE, SIZE), T)
    return img, ImageDraw.Draw(img)


def draw_cloud(d, cx, cy, scale=1.0, fill=CLOUD, outline=CLOUD_HI):
    """A puffy cloud centered roughly at (cx, cy): gray base, white top lobes."""
    s = scale
    # gray base silhouette
    d.ellipse([cx - 22 * s, cy - 4 * s, cx - 2 * s, cy + 12 * s], fill=fill)
    d.ellipse([cx + 2 * s, cy - 6 * s, cx + 22 * s, cy + 12 * s], fill=fill)
    d.rectangle([cx - 16 * s, cy + 2 * s, cx + 16 * s, cy + 11 * s], fill=fill)
    # white fluffy top
    d.ellipse([cx - 12 * s, cy - 14 * s, cx + 10 * s, cy + 4 * s], fill=outline)
    d.ellipse([cx - 20 * s, cy - 7 * s, cx - 4 * s, cy + 6 * s], fill=outline)


def draw_reference_rain(d):
    """Clean wide cloud with thin slanted rain, tuned for the reference face."""
    # gray base
    d.ellipse([8, 24, 27, 41], fill=CLOUD)
    d.ellipse([31, 21, 52, 41], fill=CLOUD)
    d.rectangle([13, 27, 47, 40], fill=CLOUD)
    # white fluffy top lobes
    d.ellipse([18, 10, 40, 32], fill=CLOUD_HI)
    d.ellipse([10, 18, 27, 34], fill=CLOUD_HI)
    d.ellipse([33, 18, 48, 32], fill=CLOUD_HI)

    for i, x in enumerate(range(18, 46, 5)):
        y = 43 + (i % 2) * 2
        d.line([x, y, x - 4, y + 12], fill=RAIN, width=1)


def save(img, name):
    img.save(os.path.join(OUT, name))
    print("wrote", name)


# --- clear (sun) ---
img, d = new()
cx, cy, r = 30, 30, 13
for i in range(8):
    a = i * math.pi / 4
    x1 = cx + math.cos(a) * (r + 4)
    y1 = cy + math.sin(a) * (r + 4)
    x2 = cx + math.cos(a) * (r + 13)
    y2 = cy + math.sin(a) * (r + 13)
    d.line([x1, y1, x2, y2], fill=SUN, width=3)
d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=SUN)
save(img, "wx_clear.png")

# --- clouds ---
img, d = new()
draw_cloud(d, 30, 30, scale=1.15)
save(img, "wx_clouds.png")

# --- rain ---
img, d = new()
draw_reference_rain(d)
save(img, "wx_rain.png")

# --- snow ---
img, d = new()
draw_cloud(d, 30, 22, scale=1.05)
for i, x in enumerate(range(15, 50, 9)):
    y = 46 + (i % 2) * 4
    d.line([x - 3, y, x + 3, y], fill=SNOW, width=2)
    d.line([x, y - 3, x, y + 3], fill=SNOW, width=2)
    d.line([x - 2, y - 2, x + 2, y + 2], fill=SNOW, width=2)
    d.line([x - 2, y + 2, x + 2, y - 2], fill=SNOW, width=2)
save(img, "wx_snow.png")

# --- thunder ---
img, d = new()
draw_cloud(d, 30, 22, scale=1.05)
d.polygon([(32, 40), (22, 55), (29, 55), (24, 60),
           (38, 44), (31, 44), (36, 40)], fill=BOLT)
save(img, "wx_thunder.png")

# --- fog ---
img, d = new()
draw_cloud(d, 30, 18, scale=0.95, fill=FOG, outline=FOG)
for i, y in enumerate(range(38, 56, 6)):
    x0 = 12 + (i % 2) * 6
    d.line([x0, y, x0 + 34, y], fill=WHITE, width=3)
save(img, "wx_fog.png")

print("done")

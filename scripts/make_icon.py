"""Generate the keysynth app icon (assets/icon/): a rounded dark tile with a piano-keys glyph.

Each size is drawn separately (fewer keys at <= 24 px so the glyph stays legible) at 8x and downsampled.
Outputs keysynth.ico (16..256), keysynth-<n>.png and the macOS menu-bar template keysynth-tray-template.png (black
glyph on transparent: macOS tints template images by their alpha). Requires Pillow. Re-run after changing the design:

    python scripts/make_icon.py
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "icon"
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
PNG_SIZES = [16, 32, 48, 64, 128, 256, 512]
SS = 8  # supersampling factor

BG_TOP = (52, 66, 120)
BG_BOTTOM = (24, 28, 52)
WHITE = (246, 243, 236)
BLACK = (18, 19, 26)
ACCENT = (245, 165, 36)


def draw(size: int) -> Image.Image:
    s = size * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    # Background tile: vertical gradient inside a rounded square.
    grad = Image.new("RGBA", (s, s))
    gd = ImageDraw.Draw(grad)
    for y in range(s):
        t = y / max(1, s - 1)
        c = tuple(round(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM))
        gd.line([(0, y), (s, y)], fill=c + (255,))
    mask = Image.new("L", (s, s), 0)
    margin = 0 if size <= 24 else round(s * 0.04)
    ImageDraw.Draw(mask).rounded_rectangle([margin, margin, s - 1 - margin, s - 1 - margin],
                                           radius=round(s * 0.22), fill=255)
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img)
    small = size <= 24
    n_white = 3 if small else 4
    black_after = [0, 1] if small else [0, 1, 2]  # black key between white i and i+1
    # Keyboard area.
    kx0, kx1 = s * (0.16 if small else 0.18), s * (0.84 if small else 0.82)
    ky0, ky1 = s * (0.20 if small else 0.22), s * (0.80 if small else 0.74)
    gap = max(SS, round(s * (0.05 if small else 0.028)))
    kw = (kx1 - kx0 - gap * (n_white - 1)) / n_white
    r = round(s * (0.04 if small else 0.035))
    for i in range(n_white):
        x0 = kx0 + i * (kw + gap)
        d.rounded_rectangle([x0, ky0, x0 + kw, ky1], radius=r, fill=WHITE)
    bw = kw * (0.62 if small else 0.58)
    bh = (ky1 - ky0) * 0.58
    for i in black_after:
        cx = kx0 + (i + 1) * (kw + gap) - gap / 2
        # Black keys sit on top with a background-coloured outline so they read at small sizes.
        d.rounded_rectangle([cx - bw / 2, ky0 - SS * 0.5, cx + bw / 2, ky0 + bh], radius=round(r * 0.8), fill=BLACK)
    if not small:  # amber accent bar under the keys
        ay = ky1 + s * 0.06
        d.rounded_rectangle([kx0, ay, kx1, ay + s * 0.045], radius=round(s * 0.02), fill=ACCENT)
    return img.resize((size, size), Image.LANCZOS)


def draw_template(size: int) -> Image.Image:
    """Menu-bar template image: the keys as an opaque silhouette, black keys cut out, no tile."""
    s = size * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    n_white = 4
    kx0, kx1 = s * 0.06, s * 0.94
    ky0, ky1 = s * 0.12, s * 0.88
    gap = round(s * 0.06)
    kw = (kx1 - kx0 - gap * (n_white - 1)) / n_white
    r = round(s * 0.06)
    for i in range(n_white):
        x0 = kx0 + i * (kw + gap)
        d.rounded_rectangle([x0, ky0, x0 + kw, ky1], radius=r, fill=(0, 0, 0, 255))
    bw = kw * 0.62 + gap
    bh = (ky1 - ky0) * 0.56
    for i in range(n_white - 1):
        cx = kx0 + (i + 1) * (kw + gap) - gap / 2
        d.rounded_rectangle([cx - bw / 2, ky0 - SS, cx + bw / 2, ky0 + bh], radius=r, fill=(0, 0, 0, 0))
    return img.resize((size, size), Image.LANCZOS)


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    for n in PNG_SIZES:
        draw(n).save(OUT / f"keysynth-{n}.png", optimize=True)
    draw_template(36).save(OUT / "keysynth-tray-template.png", optimize=True)  # 18 pt menu bar @2x
    imgs = [draw(n) for n in ICO_SIZES]
    big = imgs[-1]
    big.save(OUT / "keysynth.ico", format="ICO", sizes=[(n, n) for n in ICO_SIZES], append_images=imgs[:-1])
    with Image.open(OUT / "keysynth.ico") as ico:
        got = sorted(ico.info.get("sizes", set()))
    print(f"wrote {OUT / 'keysynth.ico'} sizes={got} and {len(PNG_SIZES)} PNGs")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Turn any image (PNG/JPG/WebP/...) into a Windows .ico with every size Explorer uses.

    python make_icon.py my_art.png            -> writes launcher/icon.ico
    python make_icon.py my_art.png out.ico
    python make_icon.py my_art.png --round    -> circular mask with transparent corners
    python make_icon.py my_art.png --pad 0.1  -> 10% transparent margin around the art

Needs Pillow:  pip install pillow
Square sources look best; non-square ones are centre-cropped to a square first.
"""
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is missing: pip install pillow")

SIZES = [256, 128, 64, 48, 40, 32, 24, 20, 16]


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    src = Path(args[0])
    dst = Path(args[1]) if len(args) > 1 else Path(__file__).resolve().parent.parent / "launcher" / "icon.ico"
    pad = 0.0
    if "--pad" in flags:
        pad = float(flags[flags.index("--pad") + 1])

    img = Image.open(src).convert("RGBA")

    # centre-crop to square
    w, h = img.size
    side = min(w, h)
    img = img.crop(((w - side) // 2, (h - side) // 2, (w - side) // 2 + side, (h - side) // 2 + side))

    # master at 1024 px, then downscale with Lanczos for every icon size
    img = img.resize((1024, 1024), Image.LANCZOS)

    if "--round" in flags:
        mask = Image.new("L", img.size, 0)
        ImageDraw.Draw(mask).ellipse((0, 0, img.size[0] - 1, img.size[1] - 1), fill=255)
        img.putalpha(Image.composite(img.getchannel("A"), mask, mask))

    if pad > 0:
        inner = int(1024 * (1 - 2 * pad))
        canvas = Image.new("RGBA", (1024, 1024), (0, 0, 0, 0))
        canvas.paste(img.resize((inner, inner), Image.LANCZOS), ((1024 - inner) // 2, (1024 - inner) // 2))
        img = canvas

    frames = [img.resize((s, s), Image.LANCZOS) for s in SIZES]
    dst.parent.mkdir(parents=True, exist_ok=True)
    # Pillow writes the 256 px frame as PNG-compressed (what Vista+ expects) and the rest as BMP.
    frames[0].save(dst, format="ICO", sizes=[(s, s) for s in SIZES], append_images=frames[1:])
    print(f"wrote {dst} ({dst.stat().st_size} bytes, sizes {SIZES})")


if __name__ == "__main__":
    main()

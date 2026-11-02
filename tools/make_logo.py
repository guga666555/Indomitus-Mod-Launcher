#!/usr/bin/env python3
"""Turn a PNG with transparency into launcher/res/logo.bmp (32-bit, premultiplied alpha), which the
launcher draws top-left instead of the text title.

    python make_logo.py my_logo.png                 whole image
    python make_logo.py my_logo.png --crop L T R B  crop first (pixels)
    python make_logo.py my_logo.png --white         treat a white background as transparent

Output is 720 px wide (drawn at 360 logical px, so it stays sharp at 200 % DPI).  Needs Pillow.
"""
import struct, sys
from pathlib import Path
import numpy as np
from PIL import Image

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = sys.argv[1:]
if not args: sys.exit(__doc__)
im = Image.open(args[0]).convert("RGBA")
if "--crop" in flags:
    i = flags.index("--crop"); l, t, r, b = map(int, flags[i + 1:i + 5]); im = im.crop((l, t, r, b))
if "--white" in flags:
    a = np.asarray(im).astype(int)
    lum = a[:, :, :3].min(axis=2)
    alpha = np.clip((255 - lum) * 1.0, 0, 255).astype(np.uint8)
    a[:, :, 3] = alpha
    im = Image.fromarray(a.astype(np.uint8), "RGBA")
a = np.asarray(im); al = a[:, :, 3]
cols = np.where((al > 20).sum(axis=0) > 0)[0]; rows = np.where((al > 20).sum(axis=1) > 0)[0]
im = im.crop((cols[0], rows[0], cols[-1] + 1, rows[-1] + 1))
im = im.resize((720, int(im.size[1] * 720 / im.size[0])), Image.LANCZOS)
W, H = im.size[0] + 16, im.size[1] + 16
canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0)); canvas.paste(im, (8, 8)); im = canvas
px = np.asarray(im).astype(np.uint32)
r, g, b, al = px[:, :, 0], px[:, :, 1], px[:, :, 2], px[:, :, 3]
pr, pg, pb = (r * al + 127) // 255, (g * al + 127) // 255, (b * al + 127) // 255
data = b"".join(np.stack([pb[y], pg[y], pr[y], al[y]], axis=1).astype(np.uint8).tobytes() for y in range(H - 1, -1, -1))
hdr = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 32, 0, len(data), 2835, 2835, 0, 0)
out = Path(__file__).resolve().parent.parent / "launcher" / "res" / "logo.bmp"
out.write_bytes(hdr + data)
print(f"wrote {out} ({W}x{H})")

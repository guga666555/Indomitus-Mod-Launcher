#!/usr/bin/env python3
"""
Indomitus unit-preview backdrop generator  (GEM ConquestUI companion, data side)

Renders a seamless looping "burning hive city at dusk" scene and encodes it as the
AVI that the Gates of Hell unit preview plays behind the 3D unit.

How the engine uses it (game 1.067, checked in call_to_arms.exe):
  * set/library.set  {library {background {video "/video/library"} ...}}
    is read by eLibraryData (0x1401BA680) into a string.
  * eGUI_MultiplayerLibrary creates an eVideoDialog (0x140C08150) as the preview
    background, sets its loop flag (+0x1D8 = 1) and opens that path (0x140C083B0).
  * eVideoPlayer appends ".avi" itself (0x14054C7A0) and decodes with the game's
    xvidcore.dll. At the end of the file the dialog reopens it (loop).
  So the file is  mods/<mod>/resource/video/library.avi , MPEG-4 Part 2 video
  (FMP4 tag, no B-frames, no audio), exactly like the mod's current one.

Every moving element is periodic over the loop length, so the last frame flows
straight into the first.

Needs: Python 3, numpy, Pillow, scipy, and ffmpeg on PATH.
  python make_library_backdrop.py                      -> library.avi (1920x1080, 40 s, 24 fps)
  python make_library_backdrop.py --preview 0,240,480  -> PNG stills only
  python make_library_backdrop.py --pan 0              -> static camera (ambient only)
  python make_library_backdrop.py --pan 2              -> faster tracking ("march")
  python make_library_backdrop.py --direction right    -> scenery moves to the right
"""
import argparse, math, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import gaussian_filter, map_coordinates

ap = argparse.ArgumentParser()
ap.add_argument("--width", type=int, default=1920)
ap.add_argument("--height", type=int, default=1080)
ap.add_argument("--fps", type=int, default=24)
ap.add_argument("--seconds", type=int, default=40)
ap.add_argument("--pan", type=int, default=1,
                help="camera tracking speed as whole layer periods per loop (0 = static)")
ap.add_argument("--direction", choices=["left", "right"], default="left",
                help="direction the scenery moves on screen")
ap.add_argument("--seed", type=int, default=40000)
ap.add_argument("--quality", type=int, default=3, help="ffmpeg mpeg4 -q:v (2 = best, 31 = worst)")
ap.add_argument("--out", default="library.avi")
ap.add_argument("--preview", default="", help="comma separated frame numbers: write PNGs, no video")
args = ap.parse_args()

W, H, FPS = args.width, args.height, args.fps
N = FPS * args.seconds                      # frames per loop
S = H / 1080.0                              # sizes below are authored for 1080p
DIR = -1.0 if args.direction == "left" else 1.0
PAN = max(0, args.pan)
HZ = int(0.58 * H)                          # horizon row
rng = np.random.default_rng(args.seed)

X = np.arange(W, dtype=np.float32)[None, :]
Y = np.arange(H, dtype=np.float32)[:, None]

def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)

def pnoise(h, w, beta, seed):
    """Tileable fBm noise (periodic in both axes), 0..1."""
    r = np.random.default_rng(seed)
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.rfftfreq(w)[None, :]
    f = np.sqrt(fx * fx + fy * fy); f[0, 0] = 1
    spec = (r.normal(size=f.shape) + 1j * r.normal(size=f.shape)) / f ** beta
    spec[0, 0] = 0
    n = np.fft.irfft2(spec, s=(h, w)).astype(np.float32)
    n -= n.min(); n /= n.max()
    return n

def crop(layer, off):
    """W-wide window of a horizontally periodic layer at a fractional offset."""
    P = layer.shape[1]
    o0 = math.floor(off); f = off - o0
    c = (np.arange(W) + o0) % P
    a = layer[:, c]
    return a if f < 1e-4 else a * (1 - f) + layer[:, (c + 1) % P] * f

def scroll(t, period, cycles):
    """Offset for a layer that moves `cycles` whole periods per loop."""
    return -DIR * period * cycles * t / N

def over(img, col, a):
    a = a[..., None]
    return img * (1 - a) + np.asarray(col, np.float32) * a

# ------------------------------------------------------------------ silhouettes
def draw_wrapped(P, painter):
    """Paint on a 3P-wide canvas and keep the middle third so shapes wrap."""
    img = Image.new("L", (3 * P, H), 0)
    win = Image.new("L", (3 * P, H), 0)
    painter(ImageDraw.Draw(img), ImageDraw.Draw(win))
    m = np.asarray(img, np.float32)[:, P:2 * P] / 255
    w = np.asarray(win, np.float32)[:, P:2 * P] / 255
    return m, w

def gothic_tower(d, dw, r, x0, base, bw, bh, win_p):
    top = base - bh
    d.rectangle([x0, top, x0 + bw, base], fill=255)
    # buttress fins down the sides
    for side in (0, 1):
        fx = x0 - bw * 0.12 if side == 0 else x0 + bw
        d.polygon([(fx, base), (fx + bw * 0.12 * (1 - side) + 0 * side, top + bh * 0.35),
                   (fx + bw * 0.12, base)], fill=255)
    cx, tw, ty = x0 + bw / 2, bw, top
    for _ in range(r.integers(2, 5)):            # stepped tiers
        tw *= r.uniform(0.55, 0.78); th = bh * r.uniform(0.07, 0.18)
        d.rectangle([cx - tw / 2, ty - th, cx + tw / 2, ty], fill=255)
        for side in (-1, 1):                     # pinnacles at each tier
            px = cx + side * tw / 2
            d.polygon([(px - 2.5 * S, ty - th), (px, ty - th - bh * 0.06), (px + 2.5 * S, ty - th)], fill=255)
        ty -= th
    d.polygon([(cx - tw * 0.3, ty), (cx, ty - bh * r.uniform(0.2, 0.5)), (cx + tw * 0.3, ty)], fill=255)
    if win_p:
        cols = max(1, int(bw / (8 * S))); rows = max(1, int(bh / (11 * S)))
        for j in range(rows):
            for i in range(cols):
                if r.random() < win_p:
                    wx = x0 + (i + 0.5) * bw / cols; wy = top + (j + 0.5) * bh / rows
                    dw.rectangle([wx - 1 * S, wy - 2 * S, wx + 1 * S, wy + 2 * S], fill=int(r.uniform(80, 255)))

def ruin(d, dw, r, x0, base, bw, bh, win_p):
    """Bombed-out block: stepped broken roof, window holes, a bent girder."""
    top = base - bh
    pts = [(x0, base), (x0, top + r.uniform(0, 0.3) * bh)]
    x = x0
    while x < x0 + bw:
        step = r.uniform(0.08, 0.25) * bw
        y = top + r.uniform(0, 0.55) * bh
        pts += [(x, y), (min(x + step, x0 + bw), y)]
        x += step
    pts += [(x0 + bw, top + r.uniform(0.1, 0.6) * bh), (x0 + bw, base)]
    d.polygon(pts, fill=255)
    # punch window holes (see-through)
    cols = max(1, int(bw / (26 * S))); rows = max(1, int(bh / (30 * S)))
    for j in range(rows):
        for i in range(cols):
            if r.random() < 0.55:
                wx = x0 + (i + 0.5) * bw / cols; wy = top + (j + 0.7) * bh / rows
                if wy > top + 0.45 * bh and wy < base - 12 * S:
                    hw, hh = 5 * S, 9 * S
                    d.rectangle([wx - hw, wy - hh, wx + hw, wy + hh], fill=0)
                    if r.random() < win_p:
                        dw.rectangle([wx - hw, wy - hh, wx + hw, wy + hh], fill=int(r.uniform(120, 255)))
    if r.random() < 0.5:                         # twisted girder sticking out
        gx = x0 + r.uniform(0.2, 0.8) * bw; gy = top + 0.3 * bh
        d.line([(gx, gy + 20 * S), (gx + r.uniform(-30, 30) * S, gy - r.uniform(20, 60) * S)],
               fill=255, width=max(1, int(3 * S)))

def build_layer(P, base, hmin, hmax, wmin, wmax, kind, win_p, seed, gap=(0.6, 1.1)):
    r = np.random.default_rng(seed)
    def paint(d, dw):
        x = 0.0
        while x < P:
            bw = r.uniform(wmin, wmax) * S; bh = r.uniform(hmin, hmax) * S
            st = r.bit_generator.state
            for k in range(3):
                r.bit_generator.state = st      # identical shape in all three copies
                (gothic_tower if kind == "gothic" else ruin)(d, dw, r, x + k * P, base, bw, bh, win_p)
            x += bw * r.uniform(*gap)
    return draw_wrapped(P, paint)

def rubble_layer(P, seed):
    """Foreground debris heaps along the bottom edge (noise profile + chunks)."""
    r = np.random.default_rng(seed)
    prof = pnoise(8, P, 1.4, seed)[0]
    hgt = (40 + 150 * prof ** 1.6) * S
    m = (Y >= (H - hgt[None, :])).astype(np.float32)
    img = Image.fromarray((m * 255).astype(np.uint8))
    d = ImageDraw.Draw(img)
    for _ in range(int(P / (60 * S))):          # angular chunks and rebar on the heaps
        x = r.uniform(0, P); y = H - hgt[int(x) % P]
        s = r.uniform(8, 30) * S
        pts = [(x + r.uniform(-s, s), y + r.uniform(-s, s * 0.3)) for _ in range(5)]
        d.polygon(pts, fill=255)
        if r.random() < 0.25:
            d.line([(x, y), (x + r.uniform(-40, 40) * S, y - r.uniform(30, 90) * S)],
                   fill=255, width=max(1, int(2 * S)))
    m = np.asarray(img, np.float32) / 255
    # keep the centre low so it never covers the unit's feet
    return m

print("building layers...", flush=True)

# sky gradient
sky_top = np.array([14, 14, 20], np.float32)
sky_mid = np.array([66, 38, 36], np.float32)
sky_hz = np.array([196, 92, 40], np.float32)
yn = Y / H; hy = HZ / H
a1 = smooth(0.0, hy * 0.7, yn)[..., None]; a2 = smooth(hy * 0.45, hy, yn)[..., None]
SKY = np.broadcast_to(sky_top * (1 - a1) + sky_mid * a1, (H, W, 3)) * (1 - a2) + sky_hz * a2
SKY = SKY.astype(np.float32)

CW = 2 * W
cloud_a = pnoise(H, CW, 1.1, args.seed + 1)
cloud_b = pnoise(H, CW, 1.45, args.seed + 2)

# megahive: one colossal tiered spire, far and hazy (static)
hive_img = Image.new("L", (W, H), 0); dh = ImageDraw.Draw(hive_img)
cx = W * 0.5; base = HZ + 4 * S; tw = 430 * S; ty = base
for i in range(7):
    th = (95 - i * 6) * S
    dh.rectangle([cx - tw / 2, ty - th, cx + tw / 2, ty], fill=255)
    for side in (-1, 1):
        bx = cx + side * tw / 2
        dh.rectangle([min(bx, bx + side * 18 * S), ty - th * 1.25, max(bx, bx + side * 18 * S), ty], fill=255)
        dh.polygon([(bx - 9 * S, ty - th * 1.25), (bx, ty - th * 1.25 - 40 * S), (bx + 9 * S, ty - th * 1.25)], fill=255)
    ty -= th; tw *= 0.84
dh.polygon([(cx - tw * 0.25, ty), (cx, ty - 170 * S), (cx + tw * 0.25, ty)], fill=255)
HIVE = gaussian_filter(np.asarray(hive_img, np.float32) / 255, 1.5)
hive_lights = (pnoise(H, W, 0.3, args.seed + 5) > 0.72).astype(np.float32) * HIVE

P_FAR, P_MID, P_NEAR = 3 * W, 2 * W, 2 * W
far_m, far_w = build_layer(P_FAR, HZ + 6 * S, 140, 360, 40, 110, "gothic", 0.05, args.seed + 10, (0.7, 1.4))
far2_m, _ = build_layer(P_FAR, HZ + 6 * S, 90, 230, 50, 140, "gothic", 0.0, args.seed + 13, (0.5, 1.0))
mid_m, mid_w = build_layer(P_MID, HZ + 46 * S, 70, 250, 90, 260, "ruin", 0.25, args.seed + 11, (0.9, 1.6))
near_m = rubble_layer(P_NEAR, args.seed + 12)
far_m = gaussian_filter(far_m, 0.9); far2_m = gaussian_filter(far2_m, 0.7)
emb_prof = pnoise(8, P_MID, 1.3, args.seed + 14)[0]
emb_top = HZ - (4 + 34 * emb_prof) * S
emb_m = (Y >= emb_top[None, :]).astype(np.float32) * (1 - smooth(HZ + 50 * S, HZ + 190 * S, Y))
mid_m = np.maximum(mid_m, emb_m)
mid_m = gaussian_filter(mid_m, 0.6); near_m = gaussian_filter(near_m, 1.2)
mid_rim = np.clip(gaussian_filter(mid_m, 3.0 * S) - mid_m, 0, 1)

# perspective ground: world texture sampled per row, so near rows move faster
GT = pnoise(256, 1024, 0.95, args.seed + 20) * 0.7 + pnoise(256, 1024, 0.5, args.seed + 21) * 0.3
G_ROWS = np.arange(HZ + 1, H, dtype=np.float32)
G_D = G_ROWS - HZ
G_F = 1100.0 * S                           # focal length (px)
G_H = 0.5 * 450.0 * S                      # camera height (texture px)
G_Z = G_H / G_D                            # texture px per screen px along a row
G_V = (G_H * G_F / G_D) % 256              # texture row (world depth)
G_MIP = smooth(0, 120 * S, G_D)[:, None]   # far rows -> average (hides aliasing)
G_FADE = smooth(0, 70 * S, G_D)[:, None]   # blend into the horizon haze

fog_n = pnoise(H, CW, 1.25, args.seed + 30)
smoke_n = pnoise(512, 512, 1.2, args.seed + 31)

# fires sit on the mid layer (layer coordinates, scroll with it), each with a smoke plume
fires = []
for _ in range(12):
    fires.append(dict(x=rng.uniform(0, P_MID), y=rng.uniform(HZ - 10 * S, HZ + 50 * S),
                      s=rng.uniform(35, 100) * S, a=int(rng.integers(2, 7)), b=int(rng.integers(7, 15)),
                      p=rng.uniform(0, 6.28), plume=rng.random() < 0.6))

NE = 150                                   # embers
E_x = rng.uniform(0, W, NE); E_y = rng.uniform(0, H, NE)
E_cyc = rng.integers(1, 5, NE); E_wa = rng.uniform(6, 40, NE) * S
E_wf = rng.integers(1, 8, NE); E_ph = rng.uniform(0, 6.28, NE)
E_br = rng.uniform(0.3, 1.0, NE) ** 2; E_par = rng.integers(1, 4, NE)

flashes = [(int(N * f), rng.uniform(0.08, 0.92) * W, rng.uniform(0.5, 1.0))
           for f in (0.06, 0.19, 0.21, 0.42, 0.58, 0.60, 0.63, 0.81, 0.93)]
beams = [(0.16 * W, 3, 0.0, 0.40), (0.86 * W, 2, 1.7, 0.34)]

vign = np.clip(1 - 0.6 * (((X / W - 0.5) ** 2) * 1.5 + ((Y / H - 0.5) ** 2) * 2.0), 0.3, 1)[..., None]
focus = (1 - 0.18 * np.exp(-(((X / W - 0.5) / 0.2) ** 2 + ((Y / H - 0.64) / 0.3) ** 2)))[..., None]

def fire_k(f, ph):
    return 0.72 + 0.16 * math.sin(ph * f["a"] * 3 + f["p"]) + 0.12 * math.sin(ph * f["b"] * 5 + 2 * f["p"])

# ----------------------------------------------------------------------- render
def render(t, noise_rng):
    ph = 2 * math.pi * t / N
    img = SKY.copy()

    flash = np.zeros((1, W), np.float32); ftot = 0.0
    for (ft, fx, fs) in flashes:
        dt = (t - ft) % N
        if dt < 2 * FPS:
            k = fs * math.exp(-dt / (0.22 * FPS))
            flash += k * np.exp(-((X - fx) / (0.2 * W)) ** 2); ftot += k
    flick = 0.86 + 0.08 * math.sin(ph * 7) + 0.06 * math.sin(ph * 13 + 1.3)

    hg = np.exp(-((Y - HZ) / (170 * S)) ** 2)
    img += (hg * flick)[..., None] * np.array([60, 22, 5], np.float32)
    img += (hg * flash * 1.5)[..., None] * np.array([255, 185, 120], np.float32)

    # smoke clouds lit from below
    cs = max(PAN, 1)
    ca = crop(cloud_a, scroll(t, CW, cs)); cb = crop(cloud_b, scroll(t, CW, 2 * cs))
    dens = smooth(0.40, 0.70, 0.6 * ca + 0.4 * cb) * smooth(0.0, 0.1, yn) * (1 - smooth(hy * 0.85, hy * 1.0, yn))
    under = smooth(0.1, hy, yn) ** 1.5
    ccol = np.array([46, 40, 44], np.float32) * (1 - under)[..., None] + np.array([150, 68, 30], np.float32) * under[..., None]
    ccol = ccol + (flash * under)[..., None] * np.array([220, 160, 110], np.float32)
    img = img * (1 - 0.88 * dens[..., None]) + ccol * (0.88 * dens[..., None])

    # searchlights
    for (bx, cyc, p0, wd) in beams:
        ang = -math.pi / 2 + wd * math.sin(ph * cyc + p0)
        dx, dy = X - bx, Y - HZ
        dang = np.abs((np.arctan2(dy, dx) - ang + math.pi) % (2 * math.pi) - math.pi)
        dist = np.sqrt(dx * dx + dy * dy) + 1
        beam = np.exp(-(dang / (0.02 + 5 * S / dist)) ** 2) * np.exp(-dist / (650 * S)) * (dy < 0)
        img += (beam * 0.45 * (1 - dens * 0.6))[..., None] * np.array([150, 160, 180], np.float32)

    # megahive (static, very hazy) and far spires
    haze_col = np.array([105, 55, 38], np.float32)
    img = over(img, haze_col * 0.35 + np.array([26, 22, 30], np.float32) * 0.65, HIVE * 0.8)
    img += (hive_lights * (0.6 + 0.4 * math.sin(ph * 4)))[..., None] * np.array([255, 180, 90], np.float32) * 0.25
    fo = scroll(t, P_FAR, 1 if PAN else 0)
    fm = crop(far_m, fo); fw = crop(far_w, fo)
    fade = smooth(HZ - 380 * S, HZ, Y)                 # lighter towards the base (haze)
    far_col = np.array([28, 24, 32], np.float32) * (1 - fade[..., None] * 0.6) + haze_col * fade[..., None] * 0.55
    img = img * (1 - fm[..., None] * 0.9) + far_col * fm[..., None] * 0.9
    img += (fw * fm * (0.7 + 0.3 * np.sin(ph * 5 + X / 41.0)))[..., None] * np.array([255, 170, 80], np.float32) * 0.45
    f2 = crop(far2_m, scroll(t, P_FAR, 2 * PAN if PAN else 0))
    img = over(img, np.array([24, 18, 20], np.float32) * 0.6 + haze_col * 0.25, f2 * 0.92)

    # ground plane (perspective scroll; one texture period per pan cycle)
    shift = -DIR * 1024 * PAN * t / N
    U = ((X - W / 2) * G_Z[:, None] + shift)
    Vg = np.broadcast_to(G_V[:, None], U.shape)
    tex = map_coordinates(GT, [Vg.ravel(), U.ravel() % 1024], order=1, mode="grid-wrap").reshape(U.shape)
    tex = 0.5 + (tex - 0.5) * G_MIP
    shade = 0.25 + 0.95 * tex ** 1.6
    gcol = np.array([26, 20, 19], np.float32) * shade[..., None]
    gl = np.exp(-(G_D / (110 * S)) ** 2)[:, None]
    gcol += (gl * flick)[..., None] * np.array([70, 28, 8], np.float32)
    gcol += (gl * flash * 0.8)[..., None] * np.array([200, 140, 90], np.float32)
    gsec = img[HZ + 1:]
    img[HZ + 1:] = gsec * (1 - G_FADE[..., None]) + gcol * G_FADE[..., None]

    # mid ruins with fire-lit rims and burning windows
    mo = scroll(t, P_MID, PAN)
    mm = crop(mid_m, mo); mw = crop(mid_w, mo); mr = crop(mid_rim, mo)
    img = over(img, np.array([15, 12, 13], np.float32), mm)
    img += (mr * 1.6 * flick)[..., None] * np.array([130, 52, 14], np.float32)
    img += (mw * (0.75 + 0.25 * np.sin(ph * 9 + X / 23.0)))[..., None] * np.array([255, 120, 40], np.float32) * 0.7

    # fires + smoke plumes
    fire = np.zeros((H, W), np.float32); smoke = np.zeros((H, W), np.float32)
    for f in fires:
        sx = (f["x"] - mo) % P_MID
        if sx > W + 400 * S: sx -= P_MID
        if not (-400 * S < sx < W + 400 * S): continue
        k = fire_k(f, ph); s = f["s"]
        x0, x1 = int(max(0, sx - 3 * s)), int(min(W, sx + 3 * s))
        y0, y1 = int(max(0, f["y"] - 4 * s)), int(min(H, f["y"] + 2 * s))
        if x1 > x0 and y1 > y0:
            gx = np.arange(x0, x1, dtype=np.float32)[None, :]; gy = np.arange(y0, y1, dtype=np.float32)[:, None]
            g = np.exp(-(((gx - sx) / s) ** 2 + ((gy - f["y"]) / (1.3 * s)) ** 2) * 1.2)
            core = np.exp(-(((gx - sx) / (0.22 * s)) ** 2 + ((gy - f["y"] + 0.25 * s) / (0.5 * s)) ** 2))
            fire[y0:y1, x0:x1] += g * k * 0.7 + core * k * 1.5
        if f["plume"]:
            # rising column, widening and leaning with the wind; noise scrolls up 2 periods per loop
            x0, x1 = int(max(0, sx - 260 * S)), int(min(W, sx + 260 * S))
            y1 = int(min(H, f["y"]))
            if x1 > x0 and y1 > 0:
                gx = np.arange(x0, x1, dtype=np.float32)[None, :]; gy = np.arange(0, y1, dtype=np.float32)[:, None]
                up = (f["y"] - gy) / (f["y"] + 1)                   # 0 at the fire, 1 at the top
                cxp = sx + DIR * up * 160 * S
                width = (18 + 190 * up) * S
                col = np.exp(-((gx - cxp) / width) ** 2) * smooth(0.0, 0.06, up) * (1 - up * 0.7)
                nv = ((gy / (2 * S) + 512 * 2 * t / N) % 512).astype(int)
                nu = ((gx / (2 * S) + f["p"] * 80) % 512).astype(int)
                nz = smoke_n[nv, nu]
                smoke[0:y1, x0:x1] = np.maximum(smoke[0:y1, x0:x1], col * smooth(0.3, 0.75, nz))
    plume_col = np.array([58, 44, 42], np.float32) + (smooth(HZ - 300 * S, HZ, Y) * flick)[..., None] * np.array([70, 30, 10], np.float32)
    img = img * (1 - smoke[..., None] * 0.75) + plume_col * smoke[..., None] * 0.75
    img += fire[..., None] * np.array([255, 105, 28], np.float32) * 0.55
    img += np.clip(fire - 0.9, 0, 2)[..., None] * np.array([255, 225, 160], np.float32) * 0.5

    # drifting ground fog
    fg = crop(fog_n, scroll(t, CW, 3 * max(PAN, 1)))
    fog = (0.35 + 0.65 * smooth(0.3, 0.8, fg)) * np.exp(-((Y - (HZ + 48 * S)) / (70 * S)) ** 2)
    img = img * (1 - 0.6 * fog[..., None]) + np.array([96, 54, 36], np.float32) * (0.6 * fog[..., None])

    # foreground rubble
    nm = crop(near_m, scroll(t, P_NEAR, 2 * PAN))
    img = over(img, np.array([7, 6, 7], np.float32), nm * 0.97)

    # embers
    emb = np.zeros((H, W), np.float32)
    ey = (E_y - E_cyc * (H + 40) * t / N) % (H + 40) - 20
    ex = (E_x + E_wa * np.sin(ph * E_wf + E_ph) + DIR * E_par * PAN * W * t / N) % W
    ok = (ey >= 0) & (ey < H)
    np.add.at(emb, (ey[ok].astype(int), ex[ok].astype(int)), E_br[ok])
    emb = gaussian_filter(emb, 1.0 * S) * 14 + gaussian_filter(emb, 4.5 * S) * 10
    img += emb[..., None] * np.array([255, 140, 50], np.float32)

    img *= vign * focus * (1.15 + 0.05 * ftot)
    img += noise_rng.normal(0, 2.0, (H, W, 1)).astype(np.float32)
    return np.clip(img, 0, 255).astype(np.uint8)

# ----------------------------------------------------------------------- output
if args.preview:
    for f in [int(v) for v in args.preview.split(",")]:
        Image.fromarray(render(f % N, np.random.default_rng(f))).save(f"preview_{f:04d}.png")
        print("wrote", f"preview_{f:04d}.png", flush=True)
    sys.exit(0)

cmd = ["ffmpeg", "-y", "-loglevel", "error",
       "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-",
       "-an", "-c:v", "mpeg4", "-vtag", "FMP4", "-bf", "0", "-g", str(FPS * 2),
       "-q:v", str(args.quality), "-pix_fmt", "yuv420p", args.out]
proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
nr = np.random.default_rng(1)
for t in range(N):
    proc.stdin.write(render(t, nr).tobytes())
    if t % 48 == 0:
        print(f"frame {t}/{N}", flush=True)
proc.stdin.close()
proc.wait()
print("done:", args.out, os.path.getsize(args.out) // 1024, "KiB")

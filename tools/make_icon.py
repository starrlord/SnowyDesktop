"""Generate the snowystarr application/tray icons (res/snowystarr.ico, res/snowystarr_paused.ico)
and the MSIX logos (installer/msix/Assets/*.png).

Renders an anti-aliased six-armed snowflake with numpy (signed distance to
capsules, supersampled) at every standard icon size and packs the images into
.ico files. Small sizes use simplified geometry so they stay crisp.

Usage:  python tools/make_icon.py [--preview PATH]   (PATH: optional PNG strip of all sizes)
"""
import math
import os
import struct
import sys
import zlib

import numpy as np

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SS = 4  # supersampling factor per axis


def capsule_sd(px, py, ax, ay, bx, by, r):
    """Signed distance from points (px, py) to a capsule a-b with radius r."""
    pax, pay = px - ax, py - ay
    bax, bay = bx - ax, by - ay
    h = np.clip((pax * bax + pay * bay) / (bax * bax + bay * bay), 0.0, 1.0)
    dx, dy = pax - bax * h, pay - bay * h
    return np.sqrt(dx * dx + dy * dy) - r


def hexagon_sd(px, py, r):
    """Signed distance to a regular hexagon (pointy sides left/right) of circumradius r."""
    k = (-0.866025404, 0.5, 0.577350269)
    x, y = np.abs(px), np.abs(py)
    d = 2.0 * np.minimum(k[0] * x + k[1] * y, 0.0)
    x, y = x - d * k[0], y - d * k[1]
    inr = r * 0.866025404
    x = x - np.clip(x, -k[2] * inr, k[2] * inr)
    y = y - inr
    return np.sqrt(x * x + y * y) * np.sign(y)


def flake_sd(px, py, size):
    """Signed distance field of the snowflake in a unit space centred at 0."""
    small = size <= 24
    medium = size <= 48
    arm = 0.86
    w = 0.115 if small else (0.085 if medium else 0.055)
    d = np.full(px.shape, 1e9)
    for i in range(6):
        a = math.radians(90 + i * 60)
        ca, sa = math.cos(a), math.sin(a)
        d = np.minimum(d, capsule_sd(px, py, 0, 0, ca * arm, sa * arm, w))
        if small:
            branches = [(0.55, 0.30)]
        elif medium:
            branches = [(0.42, 0.30), (0.66, 0.20)]
        else:
            branches = [(0.34, 0.30), (0.54, 0.25), (0.72, 0.15)]
        for t, length in branches:
            bx, by = ca * arm * t, sa * arm * t
            for s in (-1, 1):
                b = a + s * math.radians(60)
                d = np.minimum(d, capsule_sd(px, py, bx, by, bx + math.cos(b) * length,
                                             by + math.sin(b) * length, w * 0.9))
    if not small:
        d = np.minimum(d, hexagon_sd(px, py, 0.22 if medium else 0.18))
    return d


def render(size, paused=False):
    n = size * SS
    coords = (np.arange(n) + 0.5) / n * 2.0 - 1.0
    px, py = np.meshgrid(coords, coords)
    py = -py  # +y up
    d = flake_sd(px, py, size)

    pixel = 2.0 / size  # one output pixel in unit space
    outline = pixel * (1.0 if size <= 32 else 1.4)
    inside = (d < 0).astype(np.float32)
    core = (d < -outline).astype(np.float32)

    # Vertical gradient for the fill, deep blue rim for contrast on light and dark taskbars.
    t = (py + 1.0) * 0.5  # 0 bottom .. 1 top
    top = np.array([0.88, 0.97, 1.00])
    bottom = np.array([0.42, 0.74, 0.98])
    rim = np.array([0.10, 0.36, 0.70])
    fill = bottom[None, None, :] * (1 - t[..., None]) + top[None, None, :] * t[..., None]
    rgb = rim[None, None, :] * (inside - core)[..., None] + fill * core[..., None]

    if paused:
        lum = rgb @ np.array([0.30, 0.59, 0.11])
        rgb = np.repeat(lum[..., None], 3, axis=2) * 0.85 + 0.08 * inside[..., None]

    # Downsample (box filter) premultiplied colour + alpha.
    def down(a):
        return a.reshape(size, SS, size, SS, *a.shape[2:]).mean(axis=(1, 3))

    alpha = down(inside)
    if paused:
        alpha = alpha * 0.75
    prgb = down(rgb * inside[..., None])
    if paused:
        prgb = prgb * 0.75
    with np.errstate(invalid="ignore", divide="ignore"):
        straight = np.where(alpha[..., None] > 0, prgb / alpha[..., None], 0)
    rgba = np.concatenate([straight, alpha[..., None]], axis=2)
    return (np.clip(rgba, 0, 1) * 255 + 0.5).astype(np.uint8)


def png_bytes(rgba):
    h, w, _ = rgba.shape
    raw = b"".join(b"\x00" + rgba[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def dib_bytes(rgba):
    """32-bit BMP icon image (BITMAPINFOHEADER + BGRA bottom-up + AND mask)."""
    h, w, _ = rgba.shape
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    bgra = rgba[::-1, :, [2, 1, 0, 3]].tobytes()
    row_bytes = ((w + 31) // 32) * 4
    mask = bytearray()
    for y in range(h - 1, -1, -1):
        bits = 0
        row = bytearray(row_bytes)
        for x in range(w):
            if rgba[y, x, 3] == 0:
                row[x // 8] |= 0x80 >> (x % 8)
        mask += row
    return header + bgra + bytes(mask)


def write_ico(path, images):
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for size, rgba in images:
        blob = png_bytes(rgba) if size >= 256 else dib_bytes(rgba)
        dim = 0 if size >= 256 else size
        entries.append(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)))
        f.write(b"".join(entries))
        f.write(b"".join(blobs))


def msix_logos():
    """(file name, canvas px, flake px) for every logo the MSIX manifest uses.

    Square44x44Logo is the app list / taskbar / Start icon: target sizes, in the
    plated, unplated and light-theme unplated forms (without all three Windows
    puts the icon on a plate), plus scales. Square150x150Logo is the Start tile,
    with room around the flake. StoreLogo is shown by the Store and App Installer.
    """
    px = lambda v: int(v + 0.5)  # halves up, as Windows sizes them (StoreLogo at 125% is 63)
    logos = []
    for size in (16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 256):
        for form in ("", "_altform-unplated", "_altform-lightunplated"):
            logos.append((f"Square44x44Logo.targetsize-{size}{form}.png", size, size))
    for scale in (100, 125, 150, 200, 400):
        s = scale / 100
        logos.append((f"Square44x44Logo.scale-{scale}.png", px(44 * s), px(44 * s)))
        logos.append((f"Square150x150Logo.scale-{scale}.png", px(150 * s), px(75 * s)))
        logos.append((f"StoreLogo.scale-{scale}.png", px(50 * s), px(50 * s)))
    return logos


def centred(canvas, flake):
    img = np.zeros((canvas, canvas, 4), np.uint8)
    o = (canvas - flake) // 2
    img[o:o + flake, o:o + flake] = render(flake)
    return img


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.join(here, "..", "res")
    os.makedirs(root, exist_ok=True)
    for name, paused in (("snowystarr.ico", False), ("snowystarr_paused.ico", True)):
        images = [(s, render(s, paused)) for s in SIZES]
        write_ico(os.path.join(root, name), images)
        print("wrote", os.path.normpath(os.path.join(root, name)))
    assets = os.path.join(here, "..", "installer", "msix", "Assets")
    os.makedirs(assets, exist_ok=True)
    for name, canvas, flake in msix_logos():
        with open(os.path.join(assets, name), "wb") as f:
            f.write(png_bytes(centred(canvas, flake)))
    print("wrote", len(msix_logos()), "logos to", os.path.normpath(assets))
    if "--preview" in sys.argv[1:-1]:
        # Strip of every size for eyeballing.
        strip = np.zeros((256, sum(SIZES) + 8 * len(SIZES), 4), np.uint8)
        x = 0
        for s in SIZES:
            strip[:s, x:x + s] = render(s)
            x += s + 8
        path = sys.argv[sys.argv.index("--preview") + 1]
        with open(path, "wb") as f:
            f.write(png_bytes(strip))
        print("wrote", path)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate 16x16, 32x32, and 64x64 HowBoyMatsu launcher icons — no external dependencies."""
import zlib, struct

def make_png_rgba(pixels):
    w, h = len(pixels[0]), len(pixels)
    sig = b'\x89PNG\r\n\x1a\n'
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)
    ihdr = chunk(b'IHDR', struct.pack('>II', w, h) + bytes([8, 6, 0, 0, 0]))
    raw  = b''.join(b'\x00' + bytes([v for px in row for v in px]) for row in pixels)
    idat = chunk(b'IDAT', zlib.compress(raw, 9))
    iend = chunk(b'IEND', b'')
    return sig + ihdr + idat + iend

# ── Palette ──────────────────────────────────────────────────────────────────
T = (  0,   0,   0,   0)   # transparent
B = ( 26,  22,  30, 255)   # body  (dark purple-black)
S = ( 12,  12,  12, 255)   # screen bezel
G = (139, 172,  15, 255)   # screen green (classic GBC colour)
P = ( 80,  75,  90, 255)   # d-pad grey
R = (200,  30,  30, 255)   # A button (red, matches app UI theme)
r = (150,  20,  20, 255)   # B button (darker red)
W = (180, 175, 185, 255)   # start / select

# ── Canvas ───────────────────────────────────────────────────────────────────
g = [[T] * 32 for _ in range(32)]

def fill(r0, r1, c0, c1, col):
    for row in range(r0, r1):
        for col_ in range(c0, c1):
            g[row][col_] = col

# Body
fill(1, 30, 3, 29, B)

# Screen bezel (dark border) — cols 9–22, center = 15.5 to match body
fill(3, 15, 9, 23, S)

# Screen (green interior) — cols 10–21, center = 15.5
fill(4, 14, 10, 22, G)

# Start / Select — two small bars at row 17
for c in range(10, 13): g[17][c] = W   # SELECT
for c in range(15, 18): g[17][c] = W   # START

# D-pad cross (left side, rows 19-23)
for row in range(19, 24): g[row][7]  = P   # vertical
for col in range(5,  10): g[21][col] = P   # horizontal

# A button (3×3, upper-right)
fill(19, 22, 22, 25, R)

# B button (3×3, lower-left of A — classic GBC diagonal layout)
fill(22, 25, 19, 22, r)

# ── Scale helpers ─────────────────────────────────────────────────────────────
def scale_down(grid, factor):
    """Nearest-neighbour downsample: take top-left pixel of each NxN block."""
    return [[grid[r * factor][c * factor]
             for c in range(len(grid[0]) // factor)]
            for r in range(len(grid) // factor)]

def scale_up(grid, factor):
    """Nearest-neighbour upsample: each pixel becomes an NxN block."""
    out = []
    for row in grid:
        expanded = [px for px in row for _ in range(factor)]
        for _ in range(factor):
            out.append(expanded)
    return out

# ── Write all three sizes ─────────────────────────────────────────────────────
for size, pixels in [
    (16, scale_down(g, 2)),
    (32, g),
    (64, scale_up(g, 2)),
]:
    data = make_png_rgba(pixels)
    fname = f'icon-{size}x{size}.png'
    with open(fname, 'wb') as f:
        f.write(data)
    print(f"{fname} written ({len(data)} bytes)")

# Also keep icon.png (32x32) for the badge /int/apps path
import shutil
shutil.copy('icon-32x32.png', 'icon.png')
print("icon.png (32x32 copy) written")

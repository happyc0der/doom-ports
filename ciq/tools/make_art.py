#!/usr/bin/env python3
"""Generate the launcher icon and title logo.  Original pixel art, drawn from
primitives here - nothing traced or copied.  No PIL needed."""
import zlib, struct, math, sys, os

def png(path, w, h, px):
    rows = b''.join(b'\x00' + bytes(v for x in range(w) for v in px[y][x]) for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))
    open(path, 'wb').write(data)

# 5x7 blocky font for the letters we need
FONT = {
 'D': ["1111.", "1...1", "1...1", "1...1", "1...1", "1...1", "1111."],
 'O': [".111.", "1...1", "1...1", "1...1", "1...1", "1...1", ".111."],
 'M': ["1...1", "11.11", "1.1.1", "1.1.1", "1...1", "1...1", "1...1"],
 'C': [".1111", "1....", "1....", "1....", "1....", "1....", ".1111"],
 'E': ["11111", "1....", "1....", "1111.", "1....", "1....", "11111"],
}

def fire(t):
    """0..1 -> colour ramp: deep red -> orange -> yellow"""
    stops = [(0.0, (120, 10, 10)), (0.45, (215, 70, 20)), (0.8, (245, 160, 40)), (1.0, (255, 230, 120))]
    for (a, ca), (b, cb) in zip(stops, stops[1:]):
        if t <= b:
            u = (t - a) / (b - a)
            return tuple(int(ca[i] + (cb[i] - ca[i]) * u) for i in range(3))
    return stops[-1][1]

def draw_word(word, scale, gap=1):
    """Beveled fire-gradient letters with a 1-cell dark outline and drop shadow.
    Returns (w, h, pixels) with alpha."""
    cols = len(word) * (5 + gap) - gap
    W = (cols + 2) * scale
    H = (7 + 2) * scale
    px = [[(0, 0, 0, 0) for _ in range(W)] for _ in range(H)]
    mask = [[0] * (cols + 2) for _ in range(9)]
    for i, ch in enumerate(word):
        for r, row in enumerate(FONT[ch]):
            for c, v in enumerate(row):
                if v == '1':
                    mask[r + 1][1 + i * (5 + gap) + c] = 1
    def m(r, c): return 0 <= r < 9 and 0 <= c < cols + 2 and mask[r][c]
    for r in range(9):
        for c in range(cols + 2):
            if not m(r, c):
                # outline: any lit neighbour
                if any(m(r + dr, c + dc) for dr in (-1, 0, 1) for dc in (-1, 0, 1)):
                    col = (20, 6, 4, 255)
                else:
                    continue
                for y in range(scale):
                    for x in range(scale):
                        px[r * scale + y][c * scale + x] = col
                continue
            top = not m(r - 1, c); bot = not m(r + 1, c); left = not m(r, c - 1); right = not m(r, c + 1)
            for y in range(scale):
                for x in range(scale):
                    t = 1.0 - (r * scale + y - scale) / (7.0 * scale)      # bright top, dark bottom
                    base = fire(max(0.0, min(1.0, t)))
                    # bevel: light edge top/left, dark edge bottom/right
                    edge = scale // 4 or 1
                    if (top and y < edge) or (left and x < edge):
                        col = tuple(min(255, v + 70) for v in base)
                    elif (bot and y >= scale - edge) or (right and x >= scale - edge):
                        col = tuple(max(0, v - 70) for v in base)
                    else:
                        col = base
                    px[r * scale + y][c * scale + x] = (*col, 255)
    return W, H, px

def blit(dst, src, ox, oy):
    for y, row in enumerate(src):
        for x, p in enumerate(row):
            if p[3] and 0 <= oy + y < len(dst) and 0 <= ox + x < len(dst[0]):
                dst[oy + y][ox + x] = p

def icon(size):
    px = [[(0, 0, 0, 255) for _ in range(size)] for _ in range(size)]
    cx = cy = size / 2
    # dark radial vignette with a blood-red glow at the centre
    for y in range(size):
        for x in range(size):
            d = math.hypot(x - cx, y - cy) / (size / 2)
            g = max(0.0, 1.0 - d * 1.05)
            px[y][x] = (int(110 * g * g), int(16 * g * g), int(10 * g * g), 255)
    # crosshair ticks at the four edges
    L = size // 8
    for i in range(L):
        for k in (-1, 0):
            for (x, y) in ((int(cx) + k, i), (int(cx) + k, size - 1 - i), (i, int(cy) + k), (size - 1 - i, int(cy) + k)):
                px[y][x] = (235, 205, 85, 255)
    # one big beveled "D"
    s = size // 8
    w, h, letter = draw_word('D', s)
    blit(px, letter, int(cx - w / 2), int(cy - h / 2))
    return px

def logo():
    W, H, px = draw_word('DOOMCE', 7)
    # soft shadow: offset copy, dark
    sh = [[(0, 0, 0, 0) for _ in range(W + 6)] for _ in range(H + 6)]
    for y in range(H):
        for x in range(W):
            if px[y][x][3]:
                sh[y + 5][x + 4] = (0, 0, 0, 160)
    blit(sh, px, 0, 0)
    return len(sh[0]), len(sh), sh

out = os.path.join(os.path.dirname(__file__), '..', 'resources', 'drawables')
png(os.path.join(out, 'launcher_icon.png'), 65, 65, icon(65))
w, h, p = logo()
png(os.path.join(out, 'logo.png'), w, h, p)
print("icon 65x65, logo %dx%d" % (w, h))

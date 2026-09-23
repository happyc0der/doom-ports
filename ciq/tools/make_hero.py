#!/usr/bin/env python3
"""Store images the Connect IQ form asks for, drawn from the logo and the
simulator screenshots: the 1440x720 hero image and the 500x500 cover image.
No PIL; reads/writes PNG itself."""
import zlib, struct, math, os

def readpng(path):
    d = open(path, 'rb').read(); pos = 8; idat = b''
    while pos < len(d):
        n = struct.unpack('>I', d[pos:pos+4])[0]; t = d[pos+4:pos+8]; c = d[pos+8:pos+8+n]
        if t == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', c[:10])
        if t == b'IDAT': idat += c
        pos += 12 + n
    raw = zlib.decompress(idat); bpp = 4 if ct == 6 else 3; stride = w * bpp
    px = []; prev = bytearray(stride); p = 0
    for y in range(h):
        f = raw[p]; line = bytearray(raw[p+1:p+1+stride]); p += 1 + stride
        for i in range(stride):
            a = line[i-bpp] if i >= bpp else 0; b = prev[i]; c = prev[i-bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        prev = line
        px.append([tuple(line[x*bpp:x*bpp+bpp]) + ((255,) if bpp == 3 else ()) for x in range(w)])
    return w, h, px

def writepng(path, w, h, px):
    rows = b''.join(b'\x00' + bytes(v for x in range(w) for v in px[y][x][:3]) for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))

def blit(dst, src, ox, oy, scale=1.0):
    sh = len(src); sw = len(src[0]); dw = int(sw * scale); dh = int(sh * scale)
    for y in range(dh):
        sy = min(sh - 1, int(y / scale))
        for x in range(dw):
            p = src[sy][min(sw - 1, int(x / scale))]
            if p[3] and 0 <= oy + y < len(dst) and 0 <= ox + x < len(dst[0]):
                dst[oy + y][ox + x] = p

here = os.path.dirname(os.path.abspath(__file__))
W, H = 1440, 720
img = [[(0, 0, 0, 255)] * W for _ in range(H)]
for y in range(H):                                   # dark vignette, ember glow low centre
    for x in range(W):
        d = math.hypot((x - W / 2) / (W / 2), (y - H * 0.75) / (H * 0.9))
        g = max(0.0, 1.0 - d) ** 2
        img[y][x] = (int(12 + 90 * g), int(6 + 18 * g), int(4 + 8 * g), 255)

lw, lh, logo = readpng(os.path.join(here, '..', 'resources', 'drawables', 'logo.png'))
blit(img, logo, (W - lw * 2) // 2, 52, 2.0)         # 866 x 138, pixel-doubled

shots = ['2-close-encounter.png', '1-corridor.png']
sc = 0.9; sw, sh = int(448 * sc), int(486 * sc)     # 403 x 437
gap = 60; x0 = (W - (2 * sw + gap)) // 2; y0 = 240
for i, name in enumerate(shots):
    _, _, s = readpng(os.path.join(here, '..', 'store', 'screenshots', name))
    ox = x0 + i * (sw + gap)
    for y in range(-6, sh + 6):                      # dark bezel
        for x in range(-6, sw + 6):
            if 0 <= y0 + y < H and 0 <= ox + x < W: img[y0 + y][ox + x] = (18, 14, 12, 255)
    blit(img, s, ox, y0, sc)

out = os.path.join(here, '..', 'store', 'hero.png')
writepng(out, W, H, img)
print(out, os.path.getsize(out), 'bytes')

# ---- cover image, 500x500: logo on top, the close encounter below ----
CW = 500
cov = [[(0, 0, 0, 255)] * CW for _ in range(CW)]
for y in range(CW):
    for x in range(CW):
        d = math.hypot((x - CW / 2) / (CW / 2), (y - CW * 0.7) / (CW * 0.9))
        g = max(0.0, 1.0 - d) ** 2
        cov[y][x] = (int(12 + 90 * g), int(6 + 18 * g), int(4 + 8 * g), 255)
blit(cov, logo, (CW - lw) // 2, 34, 1.0)
_, _, shot = readpng(os.path.join(here, '..', 'store', 'screenshots', '2-close-encounter.png'))
sc2 = 0.72; sw2, sh2 = int(448 * sc2), int(486 * sc2)
ox, oy = (CW - sw2) // 2, 140
for y in range(-5, sh2 + 5):
    for x in range(-5, sw2 + 5):
        if 0 <= oy + y < CW and 0 <= ox + x < CW: cov[oy + y][ox + x] = (18, 14, 12, 255)
blit(cov, shot, ox, oy, sc2)
out2 = os.path.join(here, '..', 'store', 'cover.png')
writepng(out2, CW, CW, cov)
print(out2, os.path.getsize(out2), 'bytes')

#!/usr/bin/env python3
"""Write !AddrSpace's sprite files: '!addrspace' (the icon bar and Filer
icon) and 'sm!addrspace' (small), 32 bpp (TBGR) new-format sprites with a
1 bpp mask.

  mksprites.py OUT        !Sprites: 34 x 34 and 17 x 17 at 90 dpi
  mksprites.py --hi OUT   !Sprites11: 68 x 68 and 34 x 34 at 180 dpi (RISC OS
                          5 uses these in high-resolution modes)

The picture is our own, drawn from shapes: a column of memory, the address
space, with blocks of colour taken from it (the dynamic areas) and gaps
between them (what's free), and a magnifying glass looking at it. Each
pixel is the average of 6 x 6 samples, so the edges inside it are smooth;
the mask keeps the pixels more than half covered, and those on the outer
edge are blended with the icon bar's grey.
"""
import math
import struct
import sys

CASE = (40, 44, 56)                     # the column's edge
FREE = (236, 240, 246)                  # free space: pale
BLOCKS = [                              # (top, bottom, colour): the areas, in 34ths, y down
    (4.0, 8.5, (70, 130, 220)),
    (9.5, 12.0, (90, 180, 90)),
    (15.5, 21.0, (230, 170, 40)),
    (22.0, 24.0, (200, 70, 70)),
    (27.0, 30.0, (120, 100, 200)),
]
GLASS_RIM = (60, 60, 60)
GLASS = (200, 225, 255)
HANDLE = (110, 70, 40)
ICONBAR = (221, 221, 221)
SS = 6


def rounded(fx, fy, x0, y0, x1, y1, r):
    if not (x0 <= fx <= x1 and y0 <= fy <= y1):
        return False
    cx = min(max(fx, x0 + r), x1 - r)
    cy = min(max(fy, y0 + r), y1 - r)
    return (fx - cx) ** 2 + (fy - cy) ** 2 <= r * r


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def column(fx, fy):
    """The address space: a tall rounded box, left of middle"""
    x0, x1, y0, y1 = 4.0, 19.0, 1.5, 32.5
    if not rounded(fx, fy, x0, y0, x1, y1, 2.5):
        return None
    if not rounded(fx, fy, x0 + 1.2, y0 + 1.2, x1 - 1.2, y1 - 1.2, 1.6):
        return CASE
    for top, foot, c in BLOCKS:
        if top <= fy <= foot:
            # lit from the left
            return mix(c, (255, 255, 255), 0.25 * (1 - (fx - x0) / (x1 - x0)))
    return FREE


def glass(fx, fy):
    """A magnifying glass over the column's free part, its handle down to the right"""
    cx, cy, r = 19.0, 16.5, 8.0
    d = math.hypot(fx - cx, fy - cy)
    # the handle: a thick line from the rim, down and right
    ux, uy = math.cos(math.radians(45)), math.sin(math.radians(45))
    t = (fx - cx) * ux + (fy - cy) * uy
    n = abs(-(fx - cx) * uy + (fy - cy) * ux)
    if r - 0.5 <= t <= r + 9.0 and n <= 2.2:
        return mix(HANDLE, (0, 0, 0), 0.3 if n > 1.4 else 0.0)
    if d <= r:
        if d >= r - 1.6:
            return GLASS_RIM
        under = column(fx, fy)
        base = under if under else GLASS
        # magnified look: a lighter, bluish tint, with a highlight
        c = mix(base, GLASS, 0.35)
        if math.hypot(fx - (cx - 3), fy - (cy - 3)) < 2.0:
            c = mix(c, (255, 255, 255), 0.7)
        return c
    return None


def sample(fx, fy):
    return glass(fx, fy) or column(fx, fy)


def draw(size):
    s = size / 34.0
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            acc = [0, 0, 0]
            n = 0
            for j in range(SS):
                for i in range(SS):
                    c = sample((x + (i + 0.5) / SS) / s, (y + (j + 0.5) / SS) / s)
                    if c:
                        n += 1
                        for k in range(3):
                            acc[k] += c[k]
            if n * 2 < SS * SS:
                row.append(None)
            else:
                miss = SS * SS - n
                row.append(tuple((acc[k] + ICONBAR[k] * miss) // (SS * SS) for k in range(3)))
        rows.append(row)
    return rows


def sprite(name, size, dpi):
    rows = draw(size)
    w = h = size
    img = bytearray()
    mask = bytearray()
    mask_row_words = (w + 31) // 32
    for y in range(h):                  # sprites are stored top row first
        mbits = [0] * (mask_row_words * 32)
        for x in range(w):
            px = rows[y][x]
            if px:
                r, g, b = px
                img += bytes((r, g, b, 0))      # TBGR: 0x00BBGGRR
                mbits[x] = 1
            else:
                img += b"\0\0\0\0"
        for wd in range(mask_row_words):
            v = 0
            for bit in range(32):
                if mbits[wd * 32 + bit]:
                    v |= 1 << bit
            mask += struct.pack("<I", v)
    mode = (6 << 27) | (dpi << 14) | (dpi << 1) | 1
    img_off = 44
    mask_off = img_off + len(img)
    total = mask_off + len(mask)
    hdr = struct.pack("<I12siiiiiii", total, name.encode().ljust(12, b"\0"),
                      w - 1, h - 1, 0, 31, img_off, mask_off, mode)
    return hdr + img + mask


def png(path, size):
    """A preview (for checking the drawing)"""
    import zlib
    rows = draw(size)
    raw = b"".join(b"\0" + b"".join(bytes(p or ICONBAR) for p in r) for r in rows)
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    open(path, "wb").write(data)


def main():
    args = sys.argv[1:]
    if args and args[0] == "--png":
        png(args[1], int(args[2]) if len(args) > 2 else 136)
        return
    hi = args and args[0] == "--hi"
    if hi:
        args = args[1:]
        sprites = [sprite("!addrspace", 68, 180), sprite("sm!addrspace", 34, 180)]
    else:
        sprites = [sprite("!addrspace", 34, 90), sprite("sm!addrspace", 17, 90)]
    body = b"".join(sprites)
    # the file is a sprite area without its first word: count, first, free
    area = struct.pack("<iii", len(sprites), 16, 16 + len(body)) + body
    open(args[0], "wb").write(area)


if __name__ == "__main__":
    main()

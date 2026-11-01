#!/usr/bin/env python3
# Convert GDroid touch_preview VTFs (RGBA8888 / BGRA8888 / DXT1/3/5) to PNG (largest mip, downscaled to <=256).
import struct, sys, zlib, os

def png(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def ch(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 9)) + ch(b'IEND', b''))

def c565(c): return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)

def dxt(d, w, h, kind):
    out = bytearray(w * h * 4); bs = 8 if kind == 1 else 16; o = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = d[o:o + bs]; o += bs
            alpha = None
            if kind == 3:
                a64 = int.from_bytes(blk[:8], 'little'); alpha = [((a64 >> (4 * i)) & 15) * 17 for i in range(16)]; cb = blk[8:]
            elif kind == 5:
                a0, a1 = blk[0], blk[1]; bits = int.from_bytes(blk[2:8], 'little')
                if a0 > a1: tab = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                else: tab = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
                alpha = [tab[(bits >> (3 * i)) & 7] for i in range(16)]; cb = blk[8:]
            else: cb = blk
            k0, k1, idx = struct.unpack('<HHI', cb[:8]); p0, p1 = c565(k0), c565(k1)
            if k0 > k1 or kind != 1:
                pal = [p0 + (255,), p1 + (255,), tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,), tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
            else:
                pal = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
            for i in range(16):
                x, y = bx + (i & 3), by + (i >> 2)
                if x >= w or y >= h: continue
                r, g, b, a = pal[(idx >> (2 * i)) & 3]
                if alpha: a = alpha[i]
                q = (y * w + x) * 4; out[q:q + 4] = bytes((r, g, b, a))
    return bytes(out)

def size(fmt, w, h):
    if fmt in (0, 12, 1, 11): return w * h * 4
    if fmt == 13: return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8
    if fmt in (14, 15): return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16
    raise ValueError('fmt %d' % fmt)

def conv(src, dst):
    d = open(src, 'rb').read()
    vmin = struct.unpack_from('<I', d, 8)[0]; hs = struct.unpack_from('<I', d, 12)[0]
    w, h = struct.unpack_from('<HH', d, 16); fmt = struct.unpack_from('<i', d, 52)[0]; mips = d[56]
    lfmt = struct.unpack_from('<i', d, 57)[0]; lw, lh = d[61], d[62]
    frames = struct.unpack_from('<H', d, 24)[0]
    hi = None
    if vmin >= 3:
        n = struct.unpack_from('<I', d, 68)[0]
        for i in range(n):
            tag, fl, off = struct.unpack_from('<3sBI', d, 80 + i * 8)
            if tag == b'\x30\0\0': hi = off
    if hi is None:
        hi = hs + (size(lfmt, lw, lh) if lfmt >= 0 and lw else 0)
    off = hi
    for m in range(mips - 1, 0, -1): off += size(fmt, max(1, w >> m), max(1, h >> m)) * frames
    px = d[off:off + size(fmt, w, h)]
    if fmt == 0: rgba = px
    elif fmt == 12: rgba = bytes(b for i in range(0, len(px), 4) for b in (px[i + 2], px[i + 1], px[i], px[i + 3]))
    elif fmt in (13, 14, 15): rgba = dxt(px, w, h, {13: 1, 14: 3, 15: 5}[fmt])
    else: raise ValueError('fmt %d' % fmt)
    while w > 256:  # box downscale 2x
        nw, nh = w // 2, h // 2; o = bytearray(nw * nh * 4)
        for y in range(nh):
            for x in range(nw):
                for c in range(4):
                    s = sum(rgba[((2 * y + dy) * w + 2 * x + dx) * 4 + c] for dy in (0, 1) for dx in (0, 1))
                    o[(y * nw + x) * 4 + c] = s // 4
        rgba, w, h = bytes(o), nw, nh
    png(dst, w, h, rgba)

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]; os.makedirs(dst, exist_ok=True)
    for f in sorted(os.listdir(src)):
        if f.endswith('.vtf'):
            try: conv(os.path.join(src, f), os.path.join(dst, f[:-4] + '.png'))
            except Exception as e: print('skip', f, e)

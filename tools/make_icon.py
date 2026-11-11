# Draws the app icon (a small chart: blue water stepping lighter towards a tan island).
#   python tools/make_icon.py src/app.ico      (also writes docs/images/icon.png)
import math, os, struct, sys, zlib

def lerp(a, b, t): return tuple(a[i] + (b[i] - a[i]) * t for i in range(len(a)))

def render(n):
    ss = 4                                   # supersampling for smooth edges
    N = n * ss
    px = []
    rad = 0.22 * N                            # rounded-square corner
    def inside_round(x, y):
        cx = min(max(x, rad), N - rad); cy = min(max(y, rad), N - rad)
        return (x - cx) ** 2 + (y - cy) ** 2 <= rad * rad
    # island: wobbly blob centred a little off-centre; depth rings follow its distance field
    def island_r(a): return 0.17 * N * (1 + 0.18 * math.sin(3 * a + 0.6) + 0.10 * math.sin(5 * a + 1.9))
    ix, iy = 0.56 * N, 0.46 * N
    ramp = [(0x20, 0xB4, 0xFC), (0x4C, 0xC2, 0xFC), (0x8A, 0xD2, 0xFC), (0xB6, 0xE2, 0xF8), (0xE4, 0xF3, 0xFB)]
    out = bytearray()
    for y in range(n):
        for x in range(n):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    X, Y = x * ss + sx + 0.5, y * ss + sy + 0.5
                    if not inside_round(X, Y): continue
                    dx, dy = X - ix, Y - iy
                    d = math.hypot(dx, dy) - island_r(math.atan2(dy, dx))   # distance from the coast
                    if d < -0.035 * N: c = (0xFF, 0xE0, 0x6E)                   # land
                    elif d < 0: c = (0x9C, 0xCD, 0x00)                          # drying strip
                    else:
                        step = min(int(d / (0.075 * N)), len(ramp) - 1)
                        c = ramp[step]
                        if abs(d - (step + 1) * 0.075 * N) < 0.006 * N + 0.5 and step < len(ramp) - 1: c = lerp(c, (0x33, 0x4A, 0x66), 0.55)
                    acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]; acc[3] += 255
            k = ss * ss
            a = acc[3] / k
            if a > 0: out += bytes((int(acc[0] / k * 255 / a + 0.5) if a else 0, int(acc[1] / k * 255 / a + 0.5), int(acc[2] / k * 255 / a + 0.5), int(a + 0.5)))
            else: out += b'\0\0\0\0'
    return bytes(out)

def png(n, rgba):
    raw = b''.join(b'\0' + rgba[r * n * 4:(r + 1) * n * 4] for r in range(n))
    ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    return b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 9)) + ch(b'IEND', b'')

sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
imgs = [png(n, render(n)) for n in sizes]
ico = struct.pack('<HHH', 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
for n, im in zip(sizes, imgs):
    ico += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(im), off); off += len(im)
open(sys.argv[1], 'wb').write(ico + b''.join(imgs))
open(os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), '..', 'docs', 'images', 'icon.png'), 'wb').write(imgs[-1])
print('ok', sys.argv[1])

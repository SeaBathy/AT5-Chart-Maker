# Packs land polygons from ESRI shapefiles (lon/lat) into sat2at5's compact .land coastline file.
#   python make_land.py out.land in1.shp [in2.shp ...]
# Polygons whose DBF FEAT_CODE / FEATURE / TYPE / CLASS field says sea, water or ocean are left out.
#
# .land layout (little endian):
#   "S2ALAND1", uint32 polygon count, then per polygon:
#     int32 west, south, east, north (degrees * 1e7), uint32 payload bytes, payload:
#       varint ring count; per ring: varint point count, then zigzag-varint lon/lat steps (degrees * 1e7)
#       from 0,0 for the first point. Ring order and direction are kept (outer rings clockwise, holes anticlockwise).
import struct, sys

def varint(out, v):
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80); v >>= 7
    out.append(v)

def zz(v): return (v << 1) ^ (v >> 63)

def sea_filter(dbf_path):
    try: d = open(dbf_path, 'rb').read()
    except OSError: return None
    n, hl, rl = struct.unpack_from('<IHH', d, 4)
    o, off, fo, fl = 32, 1, -1, 0
    while o + 32 <= len(d) and d[o] != 0x0D:
        name = d[o:o + 11].split(b'\0')[0].decode('latin1').lower(); ln = d[o + 16]
        if name in ('feat_code', 'feature', 'type', 'class') and fo < 0: fo, fl = off, ln
        off += ln; o += 32
    if fo < 0: return None
    keep = []
    for i in range(n):
        p = hl + i * rl + fo
        v = d[p:p + fl].decode('latin1').strip().lower()
        keep.append(v not in ('sea', 'water', 'ocean'))
    return keep

def polygons(shp_path):
    d = open(shp_path, 'rb').read()
    assert d[:4] == b'\x00\x00\x27\x0a', 'not a shapefile: ' + shp_path
    keep = sea_filter(shp_path[:-4] + '.dbf')
    o, rec = 100, 0
    while o + 8 <= len(d):
        ln = struct.unpack_from('>I', d, o + 4)[0] * 2
        c = o + 8; o = c + ln
        if o > len(d): break
        idx = rec; rec += 1
        t = struct.unpack_from('<i', d, c)[0]
        if t not in (5, 15, 25): continue
        if keep is not None and idx < len(keep) and not keep[idx]: continue
        parts, points = struct.unpack_from('<ii', d, c + 36)
        starts = list(struct.unpack_from('<%di' % parts, d, c + 44)) + [points]
        base = c + 44 + parts * 4
        xy = struct.unpack_from('<%dd' % (points * 2), d, base)
        rings = []
        for k in range(parts):
            r = [(round(xy[2 * j] * 1e7), round(min(85.0, max(-85.0, xy[2 * j + 1])) * 1e7)) for j in range(starts[k], starts[k + 1])]
            if len(r) >= 4: rings.append(r)
        if rings: yield rings

def pack(out_path, ins):
    body = bytearray(); n = 0; pts = 0
    for path in ins:
        for rings in polygons(path):
            pay = bytearray(); varint(pay, len(rings))
            w = min(p[0] for r in rings for p in r); e = max(p[0] for r in rings for p in r)
            s = min(p[1] for r in rings for p in r); nn = max(p[1] for r in rings for p in r)
            for r in rings:
                varint(pay, len(r)); px = py = 0
                for x, y in r:
                    varint(pay, zz(x - px)); varint(pay, zz(y - py)); px, py = x, y
                pts += len(r)
            body += struct.pack('<iiiiI', w, s, e, nn, len(pay)) + pay; n += 1
    open(out_path, 'wb').write(b'S2ALAND1' + struct.pack('<I', n) + body)
    print('%s: %d polygons, %d points, %.1f MB' % (out_path, n, pts, (len(body) + 12) / 1e6))

if __name__ == '__main__':
    pack(sys.argv[1], sys.argv[2:])

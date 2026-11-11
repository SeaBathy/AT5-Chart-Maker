# Downloads the free coastline data and packs it into coastlines/world_coast.land and coastlines/australia_coast.land
# (needed for building the release package; the finished release zip already contains them).
#   python tools/fetch_coastlines.py
# Sources (both free to redistribute, see package/Licences and credits):
#   GSHHG 2.3.7, full resolution, levels 1 (land) and 5 (Antarctic ice front) - LGPL 3
#   Geoscience Australia GEODATA COAST 100K 2004, layer australia/cstauscd_r - CC BY 4.0
import os, sys, tempfile, urllib.request, zipfile

GSHHG = 'https://www.soest.hawaii.edu/pwessel/gshhg/gshhg-shp-2.3.7.zip'
GA = 'https://d28rz98at9flks.cloudfront.net/61395/61395_shp.zip'

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import make_land

def fetch(url, path):
    if os.path.exists(path):
        return
    print('downloading', url)
    with urllib.request.urlopen(url) as r, open(path + '.part', 'wb') as f:
        while True:
            b = r.read(1 << 20)
            if not b: break
            f.write(b)
    os.replace(path + '.part', path)

def main():
    out = os.path.join(os.path.dirname(here), 'coastlines')
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        g, a = os.path.join(tmp, 'gshhg.zip'), os.path.join(tmp, 'ga.zip')
        fetch(GSHHG, g)
        fetch(GA, a)
        with zipfile.ZipFile(g) as z:
            for n in z.namelist():
                if n.startswith(('GSHHS_shp/f/GSHHS_f_L1.', 'GSHHS_shp/f/GSHHS_f_L5.')): z.extract(n, tmp)
        with zipfile.ZipFile(a) as z:
            for n in z.namelist():
                if n.startswith('australia/cstauscd_r.'): z.extract(n, tmp)
        make_land.pack(os.path.join(out, 'world_coast.land'),
                       [os.path.join(tmp, 'GSHHS_shp', 'f', 'GSHHS_f_L1.shp'), os.path.join(tmp, 'GSHHS_shp', 'f', 'GSHHS_f_L5.shp')])
        make_land.pack(os.path.join(out, 'australia_coast.land'), [os.path.join(tmp, 'australia', 'cstauscd_r.shp')])
    print('done:', out)

if __name__ == '__main__':
    main()

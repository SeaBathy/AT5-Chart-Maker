#!/bin/sh
# Builds the release: dist/AT5 Chart Maker/ (the folder people download) and dist/AT5-Chart-Maker-<version>.zip.
#   build.bat                         (builds bin/sat2at5.exe and bin/sat2at5_gui.exe)
#   python tools/fetch_coastlines.py  (once: makes coastlines/*.land)
#   sh tools/make_package.sh
set -e
SRC=$(cd "$(dirname "$0")/.." && pwd)
VERSION=1.4.1
DIST="$SRC/dist"
OUT="$DIST/AT5 Chart Maker"
for f in bin/sat2at5.exe bin/sat2at5_gui.exe coastlines/world_coast.land coastlines/australia_coast.land; do
  [ -f "$SRC/$f" ] || { echo "missing $f - run build.bat and python tools/fetch_coastlines.py first"; exit 1; }
done
rm -rf "$OUT"
mkdir -p "$OUT/program/coastlines" "$OUT/Licences and credits"
cp "$SRC/bin/sat2at5_gui.exe" "$OUT/AT5 Chart Maker.exe"
cp "$SRC/bin/sat2at5.exe" "$OUT/program/sat2at5.exe"
cp "$SRC/coastlines/world_coast.land" "$SRC/coastlines/australia_coast.land" "$OUT/program/coastlines/"
cp "$SRC/package/READ ME FIRST.txt" "$OUT/"
cp "$SRC/package/Licences and credits/"* "$OUT/Licences and credits/"
cp "$SRC/LICENSE" "$OUT/Licences and credits/AT5 Chart Maker LICENSE.txt"
{
  echo "AT5 CHART MAKER - COMMAND LINE"
  echo "=============================="
  echo "The window program runs program\\sat2at5.exe. You can run it yourself from a command prompt"
  echo "(for batch jobs), for example:"
  echo "  program\\sat2at5.exe \"D:\\Maps\\Reef 5m.mbtiles\" --colours navionics --land auto"
  echo "  program\\sat2at5.exe preview \"D:\\Maps\\Reef 5m_AT5\" picture.png"
  echo "  program\\sat2at5.exe verify \"D:\\Maps\\Reef 5m_AT5\""
  echo
  "$OUT/program/sat2at5.exe" || true
} > "$OUT/Command line guide.txt"
# Windows line endings for the text files
for f in "$OUT"/*.txt "$OUT/Licences and credits"/*.txt "$OUT/Licences and credits"/*.TXT; do [ -f "$f" ] && sed -i 's/\r$//; s/$/\r/' "$f"; done
cd "$DIST"
rm -f "AT5-Chart-Maker-$VERSION.zip"
python -I -c "
import os, zipfile
with zipfile.ZipFile('AT5-Chart-Maker-$VERSION.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, fs in os.walk('AT5 Chart Maker'):
        for f in sorted(fs): z.write(os.path.join(d, f))
"
ls -la "$OUT" "$OUT/program" "$OUT/program/coastlines" "$OUT/Licences and credits"
ls -la "$DIST/AT5-Chart-Maker-$VERSION.zip"

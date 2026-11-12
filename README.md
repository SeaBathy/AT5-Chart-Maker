<p align="center"><img src="docs/images/icon.png" width="96" alt=""></p>

<h1 align="center">AT5 Chart Maker</h1>

<p align="center">Make your own charts for <b>Simrad, Lowrance and B&amp;G</b> chartplotters, from depth contours, satellite pictures or bathymetric maps.<br>
Free, works offline.</p>

<p align="center">
  <img alt="Version 1.4.1" src="https://img.shields.io/badge/version-1.4.1-0f766e">
  <img alt="Windows 10 and 11" src="https://img.shields.io/badge/Windows-10%20%7C%2011-0078d4?logo=windows&logoColor=white">
  <img alt="Simrad, Lowrance, B&G" src="https://img.shields.io/badge/plotters-Simrad%20%7C%20Lowrance%20%7C%20B%26G-1f6feb">
  <img alt="Tested on Simrad" src="https://img.shields.io/badge/tested%20on-Simrad-success">
  <img alt="MIT licence" src="https://img.shields.io/badge/licence-MIT-blue">
</p>

<p align="center"><img src="docs/images/03-depth-chart.png" width="820" alt="AT5 Chart Maker window"></p>

## What it does

- **Depth charts from contour lines.** 1 m, 5 m or any spacing, with or without depth areas. They're coloured like Navionics: tan land, a green drying strip, blue getting lighter with each contour, and white deep water. The colour edges sit exactly on the contour lines.
- **Satellite photos and coloured bathymetry.** These become the plotter's imagery / shaded-relief layer.
- **Bathymetry surveys on your sounder.** Open a depth GeoTIFF (for example from BathyPicker) and get a coloured, hillshaded depth picture, with contour lines on top if you like. You choose the colours, the depth range and the shading: see [Bathymetry charts on your sounder](#bathymetry-charts-on-your-sounder).
- **Built-in world coastline.** Land is drawn and the water is coloured right up to the shore, with extra detail around Australia. It switches off by itself for lakes and dams.
- **Fast.** All of Australia's 5 m contours convert in about 15 minutes; a bay takes seconds.
- **Same files as Navico's Insight Map Creator.** The output is `.at5` charts with the same names and layout, so you copy the folder to an SD card and the plotter reads it.

> **Not for navigation.** These are home-made charts. Always carry official charts.

**New here?** The [how-to guide](https://seabathy.com/guide) shows the whole path in a few minutes: get a free depth file with BathyPicker, open it on your phone or PC, and put it on your chartplotter with AT5 Chart Maker.

## Download

1. Go to the **[Releases](../../releases)** page and download `AT5-Chart-Maker-1.4.1.zip`.
2. Unzip it anywhere and open the folder.
3. Double-click **`AT5 Chart Maker.exe`**.

You need Windows 10 or 11 (64-bit). The first time, Windows may say *"Windows protected your PC"*, because the program isn't signed by a big company. Click **More info**, then **Run anyway**.

## Make a chart in 4 steps

| | |
|---|---|
| **1. Choose your map.** Drag the file onto the window, or press Browse. The program works out what kind of map it is, and fills in where to save it and the chart's name. | <img src="docs/images/03-depth-chart.png" width="420"> |
| **2. Pick the look.** The defaults suit almost every file. Every setting has a **?**: hover for a hint, click for the full explanation. | <img src="docs/images/04-help.png" width="300"> |
| **3. Press Convert.** Progress shows on the right, and the button turns into **Stop** while it works. | <img src="docs/images/06-converting.png" width="420"> |
| **4. Copy the folder onto the SD card** (FAT32, 32 GB or smaller). On the plotter, choose **Lowrance / Insight** as the chart source. | <img src="docs/images/07-done.png" width="420"> |

The program also has a step-by-step guide built in (**How to use**, top right). The full guide with every setting explained is in **[docs/HOW-TO.md](docs/HOW-TO.md)**.

## What files work

| Your file | Becomes |
|---|---|
| **Vector MBTiles** (`.mbtiles` with contour lines and/or depth areas), for example made with QGIS, GDAL or tippecanoe, or downloaded survey data | A coloured depth chart: `Large.at5` (detail) + `Medium.at5` (zoomed-out overview) |
| **Depth GeoTIFF** (`.tif` of depths or heights, for example from BathyPicker or a survey download) | A coloured, shaded depth picture for the imagery layer, plus contour lines as a depth chart if you tick the box |
| **Picture MBTiles** (jpg/png tiles), **GeoTIFF** (`.tif`), or **JPG/PNG with a world file** (`.jgw` / `.pgw`) | The plotter's imagery layer: `_3DTexture_16.at5`, `_3DTexture_32.at5` … |

Contour values can be depths (5, 10, 15) or heights (−5, −10, −15). It works out which on its own.

## Bathymetry charts on your sounder

<p align="center"><img src="docs/images/13-survey-both-layers.webp" width="720" alt="Reefs on the central Great Barrier Reef made from one GeoTIFF: the shaded depth picture on the left, the contour chart on the right"></p>

Have a high-detail depth survey? Put it on your plotter. Open the GeoTIFF straight in AT5 Chart
Maker: for example one made with [BathyPicker](https://github.com/SeaBathy/BathyPicker) (10 m detail,
anywhere in Australian waters) or downloaded from a survey portal. It is recognised as a
**depth survey**, and you get two layers in one folder:

| | |
|---|---|
| **A coloured, shaded depth picture.** Pick the colours (Spectrum, Lagoon, Thermal, Chart blues or Grey), the **depth range** the colours are spread over, and how strong the **hillshade** is (Off to 10x, the same scale as the SeaBathy apps). A small range such as 0–30 m shows the reefs in fine detail; 0–2000 m shows the whole drop-off. The plotter shows it as its imagery / shaded-relief layer. | <img src="docs/images/11-depth-survey-picture.png" width="360" alt="A reef survey coloured by depth with hillshade"> |
| **Contour lines** close together in the shallows (2, 5, 10, 15, 20 …), every metre if you type 1, or at depths you list. Zoomed out, the plotter shows the lines every 5 m; the ones in between appear as you zoom in. They're drawn into the picture, so they show with the photo overlay on, and also come as a depth chart with labels and coloured water for when it's off. | <img src="docs/images/12-depth-survey-contours.png" width="360" alt="Contour lines made from the same survey"> |

The picture is made one step sharper than the survey itself, so zooming in on the plotter stays smooth; **Detail** sets it finer still (for example 1 m), with bigger files.

Both examples are a reef in the central Great Barrier Reef (about 18.06°S 146.56°E), from a BathyPicker GeoTIFF. Most
GeoTIFFs work: longitude/latitude, Web Mercator, UTM / MGA zones and Australian Albers; 16-bit,
32-bit and 64-bit values; uncompressed, LZW, Deflate or PackBits; BigTIFF too.

**The other way, with GDAL:** you can still make the MBTiles yourself, for full control over the
colour ramp or a depth chart with the water coloured between the contours. Those MBTiles also
open in [SeaBathy](https://github.com/SeaBathy/seabathy-android) on your phone or PC.

<details>
<summary><b>How to make the MBTiles (GDAL commands)</b></summary>

| Depth chart from contours, water coloured between the lines | Coloured bathymetry picture with your own colour ramp |
|---|---|
| <img src="docs/images/10-bathymetry-chart.png" width="360" alt="A depth chart of Whitsunday and Hook Islands made from contours"> | <img src="docs/images/09-bathymetry-imagery.png" width="360" alt="The same area as a coloured bathymetry picture"> |

These use [GDAL](https://gdal.org), which comes free with QGIS and OSGeo4W (and BathyPicker
installs a copy). Depths below sea level are negative in IMAS and most surveys.

**Depth chart (contours):**

```bash
gdal_contour -a depth -fl -120 -100 -80 -60 -50 -40 -30 -25 -20 -15 -10 -5 -2 0 survey.tif contours.gpkg -nln contours
ogr2ogr -f MBTILES contours.mbtiles contours.gpkg -dsco MINZOOM=8 -dsco MAXZOOM=14
```

Open `contours.mbtiles` in AT5 Chart Maker; it is recognised as a depth chart. Pick your
contour levels to suit the area: closer together where it matters to you. On the command
line, add `--land auto` for the built-in coastline (the window does this by itself).

**Shaded-relief picture:** write a colour ramp file, `ramp.txt` (depth in metres, then red,
green, blue, alpha):

```text
nv 0 0 0 0
200 0 0 0 0
0.01 0 0 0 0
0 179 0 45
-3 255 59 48
-8 255 149 0
-14 255 224 0
-20 52 199 89
-26 0 200 180
-33 10 132 255
-45 27 42 107
-150 12 18 55
```

```bash
gdalwarp -t_srs EPSG:3857 -r bilinear survey.tif survey_3857.tif
gdaldem color-relief survey_3857.tif ramp.txt coloured.tif -alpha
gdal_translate -of MBTILES -co TILE_FORMAT=PNG coloured.tif bathymetry.mbtiles
gdaladdo -r average bathymetry.mbtiles 2 4 8 16 32
```

Open `bathymetry.mbtiles` in AT5 Chart Maker; it is recognised as a picture map. The same
two MBTiles files also open in [SeaBathy](https://github.com/SeaBathy/seabathy-android) on
your phone or PC.
</details>

## On the plotter

- **Chart source:** in the chart settings, choose **Lowrance / Insight** (not Navionics or C-MAP), or your chart's name.
- **Pictures** (satellite or depth pictures) are the plotter's photo / imagery layer. On **Simrad** (NSS, NSO, GO …): **Chart options → Photo overlay → Full**. *Land only* hides the picture over water. **Photo transparency** lets the chart show through the picture. On Lowrance and older units the setting is called **Imagery** or **Shaded relief**.
- **SD card:** FAT32, 32 GB or smaller. Newer units are reported to read bigger cards formatted NTFS; exFAT is reported not to work.
- **Several charts:** give each chart its own folder on the card. Or tick **Also put the name in the file names** before converting, and then charts can share a folder.
- **Nothing shows?** Some older units only read files at the top of the card: copy the `.at5` files themselves (not the folder) to the top. Try a small chart first.

## Questions

**Is it safe?** It works offline and sends nothing anywhere. Each chart file stores only its own file name, the date and the chart name. Pick a neutral chart name before sharing charts.

**Does it unlock paid charts?** No. It makes charts from your own files and maps only.

**Which plotters?** Simrad, Lowrance and B&G units that read Insight / AT5 charts. The charts use Atlas version 12 with true-colour fills; units that don't support that may show different colours. Tested on a Simrad unit, where the charts load and show. Other models haven't been tried yet, so try a small chart on your own unit first.

**Can it run from the command line?** Yes. `program\sat2at5.exe` does everything the window does, for batch jobs. See **[docs/TECHNICAL.md](docs/TECHNICAL.md)**.

## Building from source

You need Visual Studio 2022 or its free Build Tools ("Desktop development with C++"), and Python 3 for the coastline data.

```bat
build.bat
python tools\fetch_coastlines.py
sh tools\make_package.sh
```

- **`build.bat`** makes `bin\sat2at5.exe` (the converter) and `bin\sat2at5_gui.exe` (the window program).
- **`fetch_coastlines.py`** downloads and packs the free coastlines (about 170 MB of downloads, once).
- **`make_package.sh`** builds the release folder and zip in `dist\`.

The code uses only what Windows already has: WIC for images, the built-in SQLite for MBTiles. There's nothing else to install.

## Credits and licences

- **Code:** MIT licence ([LICENSE](LICENSE)).
- **Example pictures** of reefs on the central Great Barrier Reef use IMAS bathymetry: Flukes, E. (2026), *Multi-resolution bathymetry composite surface for Australian waters*, IMAS, [doi:10.25959/Y9VF-V723](https://doi.org/10.25959/Y9VF-V723), CC BY 4.0.
- **World coastline:** [GSHHG](https://www.soest.hawaii.edu/pwessel/gshhg/) 2.3.7, Wessel & Smith (University of Hawaii / NOAA), LGPL 3.
- **Australian coastline:** [GEODATA COAST 100K 2004](https://pid.geoscience.gov.au/dataset/ga/61395), © Commonwealth of Australia (Geoscience Australia), CC BY 4.0.

AT5 Chart Maker is an independent project. It is not affiliated with or endorsed by Navico, Simrad, Lowrance, B&G or Navionics; those names are trademarks of their owners and are used only to say which devices the charts are for.

Also from SeaBathy: [SeaBathy for Android](https://github.com/SeaBathy/seabathy-android), [SeaBathy Desktop](https://github.com/SeaBathy/seabathy-desktop) and [BathyPicker](https://github.com/SeaBathy/BathyPicker).

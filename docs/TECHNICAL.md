# AT5 Chart Maker: technical reference

The window program (`AT5 Chart Maker.exe`, built as `bin\sat2at5_gui.exe`) runs the converter `sat2at5.exe` (`program\sat2at5.exe` in a release). Everything it does also works from the command line, described here. For the step-by-step guide see [HOW-TO.md](HOW-TO.md).

`bin\sat2at5.exe` makes the same files Insight Map Creator (IMC) produces, without IMC, Keyhole/KML steps or project files. It uses every CPU core, and its only dependencies are Windows built-ins (WIC images, `winsqlite3`).

| Input | Output |
|---|---|
| Raster MBTiles (jpg/png/webp), GeoTIFF, or JPG/PNG/TIF + world file | Satellite/3D texture layers: `_3DTexture_<N>.at5` (N = metres per pixel, 0.5 … 8192) |
| **Depth** GeoTIFF (one band of depths or heights) | A coloured, hillshaded depth picture as texture layers, plus (with `--contours`) a contour-line depth chart, `Large*.at5` + `Medium.at5` |

**Depth charts from a survey.** The contour lines and the coloured depth areas between them come from one surface: each grid cell is split into four triangles around its centre, so depth is linear inside each triangle and cutting it at a depth is exact. A band's outline is only its cuts at its two depths plus the survey's edge, so neighbouring areas share their edges point for point and the lines lie exactly on the colour edges. The areas are written as real depth areas (`drval1`/`drval2`), not rebuilt from the lines. When the chart is written, area rings rounded to whole units are cleaned: back-tracks are dropped and a ring that comes back to a point it passed is split into simple loops, because plotters fill areas by cutting them into triangles and can't do that with rings that meet or cross themselves (the missed triangles show as land). Rings of one area that only touch at a point trip the same fill, as dots of the map underneath, so a hole cut by a tile edge is joined to the outer ring as the notch it really is, and in the detail chart other touching rings are moved a unit or two apart (holes only shrink and outer parts only grow, so nothing is uncovered). Loops smaller than about one survey cell (survey noise on a flat bottom) are left to the water around them, in the lines and the areas alike. `sat2at5 check-areas <file.at5>` counts any such shapes left in a chart. The zoomed-out overview (`Medium.at5`, 80 m units) is built from survey cells of at least 160 m, so its outlines don't round into staircases of squares meeting at their corners; thin drying strips narrower than that show only once the detail chart takes over. The plotter swaps from the overview to the detail chart at scale step 11425, half of Insight Map Creator's 22851, so the overview stays on one zoom step further in. In the detail chart a survey's water is written twice: in bands at every contour, and in bands at the 5 m contours only (MBTiles layer `depare_out`, own area styles). The 5 m bands show until the finer contours appear (`--fine-contours`) and the finer bands from then on, so a colour step never shows without its line.
| **Vector** MBTiles (`format=pbf`, Mapbox Vector Tiles) | Vector depth chart in IMC's layout, coloured like a real chart (S-52 or Navionics style, true RGB): `Large*.at5` (depth areas, land, depth contours) + `Medium.at5` (zoomed-out overview) |

The tool detects which kind of MBTiles you give it.

## Building

`build.bat` builds both programs with Visual Studio 2022 or its Build Tools (it finds them itself). `python tools/fetch_coastlines.py` downloads and packs the coastlines into `coastlines/`, and `sh tools/make_package.sh` assembles the release in `dist/`. `tools/make_icon.py` redraws the app icon.

## Putting charts on the card

**Card format.** Use a microSD/SD card of **32 GB or smaller, formatted FAT32**. This is what Lowrance/Simrad support.
- **Bigger cards:** newer units (for example HDS Gen3 and later, NSS evo3) are reported to read larger cards formatted **NTFS**.
- **exFAT** (how most 64 GB+ cards come from the shop) is reported **not** to be read.
- **FAT32's 4 GB per-file limit** is no problem, because the converter splits its output into files of about 450 MB.
- **Size check:** an Australia-wide satellite or bathymetry chart is about 18 GB, so it needs a 32 GB card.

**File names: the same as Insight Map Creator.**
- **Vector charts:** IMC writes `BoundAT5s\Large.at5`, `Medium.at5` and `Small.at5` (`_Overlay` variants for overlays). When a chart is too big for one file it becomes `Large_0.at5`, `Large_1.at5` ….
- **Textures:** IMC writes `_3DTexture_<level>.at5`, split the same way, for example `_3DTexture_16_0.at5`. Levels finer than 1 m are named `1D2` (0.5 m), `1D4` and so on.
- **sat2at5 output:** follows the same rules exactly, so its files can be used just like IMC's.

**Several charts on one card.** Every chart has the same file names by default, so two charts in one place overwrite each other. There are two ways round it, and both are IMC's own:
1. **One folder per chart** (the default). Each conversion writes to its own folder, `<input name>_AT5`. Copy each folder to the card as its own folder, for example `Cairns Sat\` and `AUS Bathy\`. Name the folder, not the files. Navico's own Insight Genesis downloads go on the card the same way, one folder per map.
2. **Project naming.** This is IMC's "Project Based Naming" option, which "Customizes the output filenames with the work folder name". It puts the chart name inside every file name:
   - `Cairns_Large.at5`, `Cairns_Medium.at5`, `Cairns_Large_0.at5` …
   - `_3DTexture_Cairns_16.at5`, `_3DTexture_Cairns_16_0.at5` …

   Different charts can then share one folder. Turn it on with `--project NAME` on the command line, or the window program's **Chart name in file names** box, which uses the chart name. Navico's Insight Genesis downloads are named this way too, `Vector_Large.at5`.

**Other naming rules**
- **Don't rename files by hand.** Each file stores its own name inside, so a renamed copy no longer matches.
- **Chart (atlas) name:** each chart gets its own, `M=<input name>` by default. IMC calls this the "Atlas Description", the "String Used to Identify an Atlas Dataset by the Chart Plotter". Set it with `--desc NAME` (stored as `M=NAME`) or the **Chart name** box.
- **`at5.xml`:** IMC can write one into the card's base folder. It only sets how see-through vector areas look on top of satellite imagery, and it's optional, so sat2at5 doesn't write one.
- **Reusing a folder:** the window program offers to clear that chart's old `.at5` files first. The command line warns about any left over, such as `Large_3.at5` after a run that needed fewer parts.

**On the unit.** Set the chart source to **Lowrance** (Insight), not Navionics or C-MAP. For satellite or bathymetry textures, turn on the imagery / shaded-relief overlay. Units that find several charts on a card ask about them or list them under **Chart options → Chart source**, and usually show one at a time.

These card rules come from user reports and third-party guides, not an official Navico document. Units differ, so test with a small chart first:
- some older units need the files in the card's root folder;
- one ReefMaster guide puts textures in a root folder named `ShadedRelief`.

## Satellite / raster

```
sat2at5 my_area.mbtiles -o E:\
sat2at5 ortho.tif -o out              (GeoTIFF in EPSG:4326 or 3857)
sat2at5 photo.jpg -o out --crs 4326   (image + .jgw/.pgw/.tfw/.wld world file)
```

| option | default | meaning |
|---|---|---|
| `--finest M` | auto | Finest level in metres/pixel (0.5, 1, 2, 4 …). Auto picks the largest power of two ≤ the source resolution. |
| `--coarsest M` | 8192 | Coarsest level. IMC always goes up to 8192. |
| `--zoom Z` | max | MBTiles zoom to read. Missing tiles fall back to up to 4 parent zooms. |
| `--crs 4326\|3857` | auto | CRS for world-file images. GeoTIFFs are read from their GeoKeys. |
| `--desc NAME` | `M=<input name>` | Chart (atlas) name the plotter tells charts apart by, stored as `M=NAME`. |
| `--project NAME` | off | IMC "Project Based Naming": `_3DTexture_NAME_16.at5`, `NAME_Large.at5` …, so charts can share a folder. |
| `--split-mb N` | 450 | Maximum file size before splitting into `_0`, `_1`, …. |
| `--threads N` | all cores | Worker threads. |

## Depth GeoTIFFs (bathymetry surveys)

```
sat2at5 survey.tif -o out                                   (coloured, shaded picture)
sat2at5 survey.tif -o out --contours 5 --land auto          (picture + contour lines every 5 m)
sat2at5 survey.tif -o out --depth-range 0-30 --palette lagoon --shade 2
sat2at5 survey.tif -o out --contours 2,5,10,20,50 --contours-only
```

A GeoTIFF with one band of 16/32/64-bit numbers is read as depths (`sat2at5 info` says `bathymetry`); an
8-bit or colour GeoTIFF stays a picture. Values that are mostly negative are taken as heights (depth =
−value). Readable: classic TIFF and BigTIFF, tiled or striped, uncompressed / LZW / Deflate / PackBits,
with the integer and floating-point predictors; longitude/latitude, Web Mercator, UTM / MGA (GDA94,
GDA2020, WGS84) and Australian Albers (EPSG:3577, 9473). Other projections: re-project first
(`gdalwarp -t_srs EPSG:4326`). Grids over 300 million cells are read at a reduced resolution.

| option | default | meaning |
|---|---|---|
| `--depth-range A-B` | 0 … about the deepest 2 % | Depths the colours are spread over; deeper water shows in the deepest colour. |
| `--palette P` | spectrum | spectrum, lagoon, thermal, chart or grey. |
| `--shade X` | 1 | Shading strength on the SeaBathy apps' scale: 0 (flat colours), 0.5, 1, 2 … 10. Light 45° up, mostly from the north-west (60 %), with the west, north (15 % each) and south-west (10 %). Dark slopes go down to about 45 % brightness, as in the apps. |
| `--hillshade N` | 100 | Older form: how much of the shading is blended in, 0 … 100, at the `--relief` strength. |
| `--relief N` | 5 | Slope exaggeration for the shading; 5 is `--shade 1`. |
| `--finest M` | one level finer than the survey | Finest detail in m per pixel. The default doubles the survey's detail so zoomed-in views stay smooth. |
| `--hide-above-water` | off | Leave ground above chart datum clear instead of sand-coloured. |
| `--contours S` | off | Also make the contour-line chart: a spacing (`5`; used in the shallows and widened with depth, or the same everywhere down to `--depth-range` with `--fixed-contours`), a list (`2,5,10,20,50`) or `auto` (the same as `1`: every 1 m to 50 m, then widening). The vector options below apply (`--land`, `--colours`, `--safety-depth` …). |
| `--contours-only` | off | Only the contour chart, no picture. |
| `--contour-range A-B` | all | Contour lines (and the chart's colour steps) only from A to B m, for example `0-50`. Separate from `--depth-range`, which only sets the picture's colours. |
| `--only-ranges` | off | Leave out water outside the ranges: the picture shows only `--depth-range` and the chart colours only `--contour-range`. Left-out picture pixels use DXT1's transparent index (a plotter without transparency shows them black, as no-data always was). |
| `--elevation` / `--no-elevation` | detected | Values are heights / depths. |
| `--as-image` | off | Treat the file as an ordinary picture. |

The picture uses the texture writer below (finest level from the survey's cell size). The contours are
traced on the grid (lightly smoothed, marching squares), simplified, written to a temporary vector
MBTiles and converted by the vector writer. The chart is lines only by default: the picture already
colours the water, and filling between contour lines would have to guess beyond the survey's edge
(`--areas-from-contours` fills it anyway).

## Vector depth charts (vector MBTiles)

```
sat2at5 depths.mbtiles --list                      (show the layers/attributes in your tiles first)
sat2at5 depths.mbtiles -o E:\                      (writes Large.at5 or Large_0..N.at5, plus Medium.at5)
sat2at5 depths.mbtiles -o out --colours navionics
```

How features are classified (all of this is adjustable):
- **Depth areas.** Polygons with a depth attribute are coloured by depth band. The attribute is auto-detected from `DRVAL1`, `depth_min`, `mindepth`, `min_depth`, `depth`, `DRVAL2`, `depth_max`, `value`; override with `--depth-attr`. Restrict layers with `--depth-layers`.
- **Land.** Polygons in layers named like `land`, `LNDARE` or `island`; override with `--land-layers`.
- **Depth contours.** Lines with `VALDCO`, `depth`, `contour`, `elevation`, `ele` … (`--contour-attr`, `--contour-layers`). Each line is labelled "`N m`" exactly as in IMC's own files.
- **Contours by zoom.** As in IMC (and Navionics HD contours / ENC "SCAMIN"), finer contours only appear when you zoom in:
  - Multiples of 5 m show at every zoom of the detail chart; change this with `--major-contours 10`.
  - The other contours (1, 2, 3, 4 m …) appear only when zoomed in close.
  - `--fine-contours close|medium|always|never` moves that point: `close` is IMC's default, `medium` zooms out further, `always` shows them as soon as the detail chart shows, and `never` leaves them out.
  - So with 1 m data you see 5 m lines zoomed out and every metre zoomed in. With 5 m data every line is a "5 m" line and always shows.
  - The window program has the same two settings.
- **Elevation data.** Files whose contour values are mostly negative (heights, e.g. -5 … -200 m) are detected automatically and flipped to depths. Force this with `--elevation`, or turn it off with `--no-elevation`.
- **Coastline (land) file, `--land FILE.shp`.** This gives the full Navionics look: tan land, a green drying strip along the shore, and water coloured right up to the coast, with islands drawn as land.
  - **Built-in coastlines:** `--land auto` (the window program's default) uses `coastlines\world_coast.land` (GSHHG 2.3.7 full resolution, LGPL) with `coastlines\australia_coast.land` (Geoscience Australia GEODATA COAST 100K 2004: mainland and 8,700 islands at mean high water, 1:100 000, CC BY 4.0) taking over around Australia. Only land near the chart is loaded (under a second). Charts that lie inland (most sampled contours on land: lakes, dams, rivers) get no coastline. `--land world`, `--land australia` or your own `.shp` / `.land` file are also accepted; `tools/make_land.py` packs shapefiles into `.land`.
  - **Other coastlines:** any lon/lat polygon shapefile works, for example OpenStreetMap land polygons. Polygons coded `sea` are ignored.
  - **Drying strip:** when the data has no 0 m contour, the coastline is taken as `--drying-height` (default 1.5 m) above chart datum. The strip between the coast and that depth is shown green: wider on flat shores, thin on steep ones. This is an estimate, not surveyed intertidal data. Use `--drying-height 0` for none.
  - **Where land is drawn:** across every chart block that has data, so there are no gaps inland. Empty tiles near the data (inlets, harbours, flat bottom) are filled from the coast and the surrounding contours.
- **Files without a coastline** (and no `--land`): the program can't know where the shore is. The water between the shallowest contour and the unknown shore is left clear, and so are empty tiles (islands, flat bottom). Your plotter's own map shows there. Enclosed shallow patches such as reef tops are still coloured.
- **Speed:** empty tiles are skipped, not filled from lower zooms. Australia-wide 5 m contours (94,000 tiles) now convert in about 11 minutes on 16 threads.
- **Points** (soundings, labels) are not converted.

**Contour-only data, such as 1 m contours.** Many depth MBTiles have only contour lines and no depth-area polygons. Without depth areas there would be nothing to colour. The tool detects this and builds the depth areas from the contour lines themselves, so every colour edge sits on its contour line:
- **Between two lines.** Water between two contour lines is coloured by its depth, estimated from its distance to the two lines. Distances are measured through the water, never across another line, so a colour can't jump past a line.
- **Inside a single ring.** A hole or a shoal enclosed by a single contour ring takes the opposite side of that contour from the water around it.
- **Next to land.** Land counts as the shoreline: 0 m, or just above the lowest drying contour.
- **Exact edges.** Charts are built this way too: in ENC/S-57 data, a depth area's edge *is* its depth contour. The colour edge is moved onto the real contour line, at the depth where one band turns into the next, and coastal edges onto the real land outline. Dark blue therefore stops exactly on the 10 m line and the lighter blue starts on the other side. On test data, the edges sit a median 0.25 m from the true line, which is the precision of the tile coordinates themselves. A band start with no contour in the data (for example a 2 m band edge in 5 m data) is estimated between the neighbouring lines.
- **Where the data stops.** Nothing is coloured beyond the data, so missing tiles and cut edges stay empty.
- **Choosing the mode.** `--areas-from-contours` forces this mode; `--no-areas-from-contours` turns it off. The window program has the same choice under "Colour between contours".

**Colours.** Depth areas are written with **true RGB colours**. This is the Atlas v12 RGBA fill IMC supports through its `*CLRRGB` attributes, plus a palette number as a fallback for older units. The colouring follows paper charts, ECDIS and Navionics: **drying = green, shallow = darker blue, getting lighter with depth, deep = white**, and land is buff.
- **Standard chart (S-52), the default:** IHO S-52 day colours (DEPIT, DEPVS, DEPMS, DEPMD, DEPDW, LANDA from the OpenCPN S-52 tables). Bands: below 0 m green `83B295`, 0–2 m `73B6EF`, 2–5 m `98C5F2`, 5–10 m `BAD5E1`, 10 m+ `D4EAEE`.
- **Navionics (the window program's default)** (`--colours navionics`, plus `--deep-depth 20`). This is the look of Navionics charts, measured from Navionics web-app and Garmin Navionics Vision Australia screenshots:
  - yellow land `FFEF73` and lime-green drying areas `9CCD00`;
  - water in one blue step per contour, strong to pale (`20B4FC` → `4CC2FC` → `6ACCFC` → `8AD2FC` → `A2DAFC` → `B6E2F8` → `C8EAF8`), white from the deep depth;
  - dark-grey main contours `404040`, and with 1 m data light-grey 1 m contours `A8A8A8`, like Navionics HD.

  With 5 m data you get a few bold steps (0–5 m strong blue, 5–10, 10–15, 15–20 m paler, white beyond). With 1 m data every metre is its own step. Change the land or line colours with `--land-colour`, `--contour-colour` or `--fine-contour-colour`.
- **Navionics classic** (`--colours navionics-classic`): a simpler fixed scheme: green `96C878`, `5096E1`, `7DB9F0`, `AFD7F5`, then white from 10 m, on buff land.
- **Soft blue gradient** (`--colours gradient`, plus `--deep-depth 20`): also one shade per contour step, but in softer blues on buff land, with more contrast in the shallows. The steps are taken from the depths that actually have contours in your file (0, 1, 2 … 5, 6, 8, 10, 12, 15 m with 1 m data; 0, 5, 10, 15 m with 5 m data), so every shade edge is a real contour line. For files that have their own depth areas, the steps follow those areas' depths instead.
- **Notes when an edge can't be exact:**
  - If the safety depth or a highlight edge has no contour in your data, the converter prints a note and estimates that edge.
  - For files with their own depth areas, an edge that isn't an area boundary falls on the next one. The note says at which depth, for example: "edge is at the next area boundary, 5 m".
- **Safety depth** (`--safety-depth 3`): like a plotter's shallow-water / safety-depth setting.
  - Water from 0 m to that depth is shown in a warning colour (`--shallow-colour`, default soft red `F2A0A0`; Raymarine/Navionics use red hatching).
  - The contour at that depth is drawn bold, 3 px wide and dark (`--safety-colour 1E1E1E`), at every zoom, like the safety contour in Navionics and ECDIS.
  - If your data has no contour at that depth, the tool says so and the edge is estimated.
- **Highlight a depth range** (`--highlight 10-20` or `--highlight 10-20:FFB347`): Garmin "depth range shading" / C-MAP custom shading for a fishing depth.
- These combine with any scheme. The window program has all of them under the colour choice: Gradient "white from", Safety depth, and Highlight.
- **Custom:** `--colours min:83B295,0:73B6EF,2:98C5F2,5:BAD5E1,10:FFFFFF`, as "start depth in metres : RRGGBB", 1–30 bands. Land colour: `--land-colour C9B97A`; contour colour: `--contour-colour 7D898C`.
- **Previews:** in the window program, press **Preview** after Convert, or from the command line run `sat2at5 preview Large.at5 out.png --no-lines`. This shows the chart in its colours. With dense contours, leave the lines off or they hide the colours.
- **Choosing bands for detailed data.** With fine data (1 m contours), you can use more bands, for example `--colours min:96C878,0:1F5FC8,2:3A80D8,5:5A9EE6,10:7AB6EE,20:9ACDF2,30:BADFF6,50:FFFFFF`.

Other vector options:
- `--name Large` sets the output base name.
- `--no-overview` skips `Medium.at5`.
- `--overview-zoom Z` sets the MBTiles zoom used for the overview (default 9). When colours are built from contours, finer tiles are merged to make zoom 9.
- `--overview-name Medium` sets the overview file name.
- `--zoom Z` sets the detail zoom (default max).
- Missing detail tiles fall back to up to 6 parent zooms.

## Other commands
- `sat2at5 verify <file|folder> [-v]` checks magic, size fields, header checksum, atlas CRC-16, the key block, and the full body structure: every texture tile, or every vector block, node, cell and feature for files using IMC's own style libraries.
- `sat2at5 preview <file.at5|folder> <out.png> [maxpx] [--no-lines] [--bbox W,S,E,N]` renders (give the output folder for a picture of the whole chart: a big level is split into `_0`, `_1`… files and each one is only a strip; `--bbox` limits it to an area in degrees, e.g. `145.6,-17.1,146.1,-16.6` for Cairns), and a texture level or a vector `Large*.at5` / `Medium.at5` chart (in its colours) north-up.
- `sat2at5 rebuild <in.at5> <out.at5>` re-encodes a texture file through this writer (byte-identical self-test).

## How it works
1. **Projection.** Lowrance Mercator is a sphere of radius 6356752.3142: x = R·lon, y = R·ln tan(π/4 + φ/2). EPSG:3857 differs only by the scale factor R_lowrance/R_web, so MBTiles map exactly with no warping.
2. **Raster.** The pyramid uses 256×256 DXT1 tiles (row 0 = south) at N m/px on a global grid, built depth-first in parallel.
3. **Vector.**
   - **Detail (Large):** 480 km blocks, each 20×20 cells of 24 km, each holding a 4×4 sub-node of 6 km cells. Features are clipped to the 6 km cells, in 1 m units. Rings are stored as an i32 start point followed by i8 / `0x80`+i16 deltas. IMC's polygon convention is clockwise exteriors, opposite-orientation holes, non-zero fill.
   - **Overview (Medium):** 4800 km blocks of 20×20 cells of 240 km, in 80 m units.
   - **Styling:** the type libraries are generated from real `Large_0.at5` / `Medium.at5` entries as templates: one depth-area style per colour band (palette number + RGBA fill), land, and two contour styles with the chosen pen colour.
4. **Container.** The exact Atlas v12 layout, with header checksum, CRC-16 and the 4-word key (`"UnrestrictedUse"` salt).

## Validation done
- **Textures:**
  - `rebuild` reproduces a full set of 32 texture files byte-for-byte (about 11 GB).
  - Georeferencing round trips (MBTiles and EPSG:4326 PNG) land in the exact same cells with a (0, 0) shift.
  - About 3,100 tiles/s on 16 threads.
- **Vector format:**
  - Sample detail and overview charts re-encode byte-for-byte through the writer.
  - `verify` walks a full detail chart (9.16 M polygons, 598 k contours) and an overview chart.
- **Vector conversion end to end:**
  - Chart data was exported as standard MVT tiles (layers `DEPARE`/`DRVAL1`, `LNDARE`, `DEPCNT`/`VALDCO`) and converted back.
  - 24 km window: every colour's area within 0.01% of the original, contour length within 0.001%, all 37 depth names.
  - Full 480 km block (9,900 tiles, 586 k features): output equals the MVT input within 0.001% per colour, contours within 0.003%, in **2.6 s**.
  - Overzoom fallback tested with missing tiles.
  - Every output parses with the independent Python parser.
- **Depth colours** (synthetic seabed with a coast, island, drying reef, deep hole and shoal; colours checked against the true depth at 3,000 random points, plus 1,000 points just outside the data):

  | Test | Correct colour band | Colour outside the data |
  |---|---|---|
  | Depth areas in the file | 100% | none |
  | Contours only (0/2/5/10/20 m) | 100% | none |
  | **1 m contours**, 14 colour bands, steep 25 m drop-off with lines closer together than a pixel | **99.8%**; the misses are one band off, right on a band edge | none |

- **Distance from colour edges to the true contour line:**

  | Data | Raster edges only | With edges snapped to the contours |
  |---|---|---|
  | Contours every few metres, zoom 14 | median 2.9 m, 95% within 6.8 m | **median 0.24 m, 95% within 0.6 m, max 1.0 m** |
  | 1 m contours, zoom 13 | median 5.9 m, 95% within 15 m | **median 0.28 m, 95% within 0.8 m**; 0.5% over 2 m, all at the artificial vertical wall |

- **Navionics gradient:** 100% of 3,000 sample points have the correct shade on both test charts, and shade edges are a median 0.24 m from the contour lines. Safety depth, bold safety contour and highlight range were checked in the files and previews, from the command line and the window program.
- **A real 480 km chart** (1–200 m contours every metre), rebuilt from its contours alone:
  - It converts in about 3 minutes on 16 threads and passes `verify`.
  - Points along its real 2, 5 and 10 m contours were checked over a 24 km reef area. Where the colour changes at the line, the colour edge is a median 0.21 m from it, and 95% are within 0.9 m.
  - About 4% of water points show no change. They cluster around thin sliver-shaped land polygons in this test data, which are narrower than one 13 m raster pixel.

- **Full feature check:**
  - **Vector option combinations:** 14 combinations covering every colour scheme, contour-only and depth-area files, safety depth and highlight, fine-contour zoom (close, medium, always, never), contour intervals of 2.5, 5 and 10 m, a custom overview zoom, and split files.
  - **What each output was checked for:**
    - `verify` passes, and an independent parser reads every feature.
    - Every colour style holds exactly the reported RGB colour.
    - Zoom limits are right for fine contours, zoomed-out contours and their label points; the overview zoom range is 1785–89.
    - Every contour depth is in the right zoom group, and the safety style is width 3 and holds only the safety depth.
    - Colour bands are 99.75–100% correct, except edges placed at depths with no contour line, which are estimates.
    - Nothing is coloured outside the data.
  - **Real 1 m chart:** with the gradient, safety depth and highlight all on, it converts in 3 min 20 s and passes every check.
  - **Bad option values** now give a clear error (for example "--safety-depth needs a number, got 'abc'" or "--deep-depth must be greater than 0"). A reversed highlight such as 20-10 is accepted.

## Not yet verified / limits
- **Plotter testing so far: one Simrad unit.** The AT5 files load and show on it. The files follow the Atlas v12 format and its integrity checks, but other Simrad, Lowrance and B&G models haven't been tried yet. Try a small area first.
- **True-RGB colours need a unit that supports Atlas v12 RGBA** (IMC calls these NOS-software units). Older units fall back to the palette numbers, so their colours may differ.
- **Vector output covers only depth areas, land and depth contours.** No points, soundings, buoys, lights, aviation, search index or text labels other than the contour "N m" names. IMC's 18–23 MB contour-name search index is written empty, as in IMC's Medium file.
- **No-data and naming:** raster no-data is black, as in IMC. Keep each chart in its own folder on the card, since every chart uses the same file names (see "Putting charts on the card").
- **Colouring from contours:**
  - Which colour each area gets is decided on a raster of about 6 m (zoom 14 tiles) or 13 m (zoom 13). The edges are then moved onto the real lines.
  - Features smaller than about one raster pixel can be lost or stay stair-stepped: tiny contour rings, land slivers narrower than a pixel, or many contours stacked inside one pixel at a vertical wall.
  - The overview file (`Medium.at5`, zoomed far out) has colours but no contour lines, as in IMC.
- **Plotter-specific display:**
  - The bold safety contour uses the style's line-width field (`w2`; 1 in IMC's own styles, 3 here). IMC itself writes line widths (its "WIDTH" attribute), but a plotter test is still needed.
  - Shallow areas are a solid colour. The AT5 fill records seen in IMC's styles have no hatch pattern.
  - In a tile where the data stops partway, the last stretch beyond the final contour line is still coloured, up to the edge of that tile.
  - Very large areas with no contour at all are treated as deep water.
- **Elevation tiles (Terrain-RGB / Terrarium DEM MBTiles):** these are refused with an explanation, because they aren't pictures. Shaded-relief bathymetry *images* convert like satellite maps.
- **Encryption:** files are "Unencrypted" (`UnrestrictedUse`).
- **Memory and geography limits:** images are loaded fully into RAM, and the antimeridian isn't handled.

## Source files
- `src/main.cpp`: command line, raster pyramid, previews
- `src/gui.cpp`: the window program (`gui.manifest`, `app.rc`, `app.ico`)
- `src/at5.*`: AT5 container, keys, verifier
- `src/dxt1.hpp`: BC1 texture encoder
- `src/sources.*`: picture inputs (MBTiles, GeoTIFF, world files)
- `src/depth.*`: depth GeoTIFFs: TIFF reader, map projections, coloured hillshaded imagery, contour lines
- `src/vector.*`: vector tiles, depth areas from contours, coastline, vector chart encoder
- `src/inflate.hpp`: gzip decoder
- `src/styles.hpp`: the default chart styles (land, depth areas, contours, contour labels), written field by field
- `tools/`: coastline download and packing, icon, release packaging

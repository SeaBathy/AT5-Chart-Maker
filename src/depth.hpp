// Depth GeoTIFFs (bathymetry surveys, BathyPicker files): single-band grids of depths or heights.
//  - read: classic TIFF and BigTIFF, tiled or striped, uncompressed / LZW / Deflate / PackBits, with the
//    integer and floating-point predictors; geographic, Web Mercator, UTM / MGA and Australian Albers grids
//  - imagery: coloured by depth over a chosen range, with hillshade, as an imagery source for the texture writer
//  - contours: contour lines every N metres, written as vector MBTiles for the vector chart writer
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "sources.hpp"

namespace depth {

struct Options {
  double rangeMin = 0, rangeMax = -1;   // colours spread over this depth range (m); rangeMax < 0 = automatic
  std::string palette = "spectrum";     // spectrum, lagoon, thermal, chart, grey
  // Shading strength as in the SeaBathy apps: 0 = flat colours, 1x = the slopes as they are, 2x and up = steeper
  // and darker. relief is the slope exaggeration (5 per 1x); hillshade blends the shading in (1 = fully).
  double hillshade = 1;
  double relief = 5;
  bool showAboveWater = true;           // ground above chart datum (islands, drying reef) in a sand colour
  int elevation = -1;                   // -1 auto, 1 values are heights (depth = -value), 0 values are depths
  int crs = 0;                          // force an EPSG code (0 = from the GeoTIFF)
  std::vector<double> pictureContours;  // contour depths drawn into the picture itself (empty = none)
};

struct Grid;   // the loaded depth grid (depths in metres, positive down)

// Is this a depth GeoTIFF (one band of 16/32/64-bit numbers) rather than a picture?
bool IsDepthTiff(const std::string& path);

// Reads the grid. Prints what it found. Throws with a readable message for unsupported files.
std::shared_ptr<Grid> Load(const std::string& path, const Options& o);

// Imagery source: the depth grid coloured and shaded, for the texture (imagery) writer.
std::unique_ptr<src::Source> MakeImagery(std::shared_ptr<Grid> g, const Options& o);

// Contour levels: "auto", a spacing ("5") or a list ("2,5,10,20,50"). Empty result = no contours.
// A spacing applies in the shallows and widens with depth (5 m past 50 m, 10 m past 200 m, 50 m past 500 m,
// 100 m past 1000 m), never finer than asked. fixed = the same spacing down to maxDepth (the colour range) only.
std::vector<double> ContourLevels(const Grid& g, const std::string& spec, double maxDepth = 0, bool fixed = false);

// Writes contour lines at the given depths as vector MBTiles (layer "contours", attribute "depth" in metres).
// Returns the number of lines written.
size_t WriteContours(const Grid& g, const std::vector<double>& levels, const std::string& mbtiles);

// The automatic colour range, for messages: 0 .. a round depth just below the deepest 2 % of the data.
double AutoRangeMax(const Grid& g);

}  // namespace depth

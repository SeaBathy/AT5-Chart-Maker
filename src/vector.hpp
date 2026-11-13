// Vector MBTiles (Mapbox Vector Tiles) -> AT5 vector chart (depth areas, land, depth contours),
// in the standard "Large" chart layout, with the default depth-chart style library.
#pragma once
#include <string>
#include <vector>
#include <utility>

namespace vec {

struct Options {
  int zoom = -1;                      // MBTiles zoom to read (default max)
  std::string landLayers;             // comma list; empty = auto (land/lndare/coast/island)
  std::string depthLayers;            // comma list; empty = any polygon layer with a depth attribute
  std::string contourLayers;          // comma list; empty = any line layer with a contour attribute
  std::string depthAttr;              // empty = auto (drval1, depth_min, mindepth, min_depth, depth, ...)
  std::string contourAttr;            // empty = auto (valdco, depth, contour, elevation, ele ...)
  bool elevation = false;             // attribute values are elevations (depth = -value)
  bool elevationAuto = true;          // detect elevation-style data (mostly negative values) automatically
  double majorContourEvery = 5;       // contours that are multiples of this show at every zoom of the detail chart
  int fineContourZoom = 3808;         // zoom (x256) below which the other contours appear: 3808 IMC default, 22851 always, -1 never
  bool gradient = false;              // shade contour by contour (see deepDepth)
  int gradientRamp = 0;               // 0 soft blue, 1 Navionics chart palette
  double deepDepth = 20;              // gradient: white from this depth
  bool dataHasLand = true;            // set by Convert: the tiles contain land polygons
  std::vector<double> dataLevels;     // contour depths found in the input (set by Convert; gradient steps follow them)
  double safetyDepth = -1;            // >0: water 0..safetyDepth in shallowColour, that contour drawn bold
  uint32_t shallowColour = 0xF2A0A0;
  uint32_t safetyColour = 0x1E1E1E;   // bold safety contour line
  double highlightFrom = -1, highlightTo = -1;   // depth range shading (fishing)
  uint32_t highlightColour = 0xFFB347;
  std::string landFile;               // coastline / land polygons (ESRI shapefile, lon/lat): land drawn, shore known
  double dryingHeight = 1.5;          // coastline height above chart datum when the data has no 0 m contour (drying strip)
  int areasFromContours = -1;         // depth areas rebuilt from contour lines: -1 auto (when tiles have none), 0 off, 1 on
  std::vector<std::pair<double, uint32_t>> bands;  // (min depth, RGB 0xRRGGBB), ascending; empty = S-52 scheme
  uint32_t landColour = 0xC9B97A;      // S-52 LANDA
  uint32_t contourColour = 0x7D898C;   // S-52 DEPCN (zoomed-out contours)
  uint32_t minorContourColour = 0xFFFFFFFF;   // finer contours; 0xFFFFFFFF = same as contourColour
  std::string name = "Large";         // output base name -> Large.at5 / Large_0.at5 ...
  std::string desc;                   // atlas description
  double splitMb = 450;
  int threads = 0;
  bool list = false;                  // just print layers/attributes
  bool overview = true;               // also write a coarse overview (IMC 'Medium' layout)
  int overviewZoom = -1;              // MBTiles zoom for the overview (default 9, clamped to available)
  std::string overviewName = "Medium";
};

bool IsVectorMbtiles(const std::string& path);
std::string RasterDemEncoding(const std::string& path);  // "mapbox" / "terrarium" for encoded-height raster MBTiles, else ""
std::vector<std::pair<double, uint32_t>> ParseBands(const std::string& s);  // preset name or "min:RRGGBB,2:RRGGBB,..."
std::vector<std::pair<double, uint32_t>> DefaultBands();
std::vector<std::pair<double, uint32_t>> FinalBands(const Options& o);   // scheme + safety depth + highlight
uint32_t ParseRgb(const std::string& s);
int CheckAreas(const std::string& at5Path);   // counts area shapes a plotter fill can't triangulate
int PreviewVector(const std::string& at5Path, const std::string& png, int maxPx, bool lines = true, const double* lonLatBox = nullptr);  // 1 ok, -1 not a vector chart
int VerifyBody(const std::string& path);  // 1 ok, 0 broken, -1 not a vector file with the embedded style library
int Convert(const std::string& input, const std::string& outDir, const Options& o, const std::string& date);

}  // namespace vec

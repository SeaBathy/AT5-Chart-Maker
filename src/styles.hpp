// The default chart styles: how land, depth areas, depth contours and contour labels are drawn.
//
// A vector chart carries a small style library. Each style is a record keyed by a short style
// key; the chart's features point at a style by its index. The records are written field by
// field below. vector.cpp then fills in each chart's colours, palette numbers, label links and
// zoom limits (AreaEntry, ContourEntry, WithMinZoom) and assembles the library (AssembleLib).
//
// Field names follow what the values do where that is known. The rest are the values the
// plotters expect, given as plain constants.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace at5 {

struct Style {
  std::vector<uint8_t> key;    // style key (fixed length within a library)
  std::vector<uint8_t> data;   // style record
};

namespace styles {

inline void U8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
inline void U16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
inline void I32(std::vector<uint8_t>& b, int32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(uint32_t(v) >> (8 * i))); }
inline void Zeros(std::vector<uint8_t>& b, size_t n) { b.insert(b.end(), n, 0); }
inline void Str8(std::vector<uint8_t>& b, const std::string& s) { U8(b, uint8_t(s.size())); b.insert(b.end(), s.begin(), s.end()); }
inline std::vector<uint8_t> Key(const std::string& s) { return {s.begin(), s.end()}; }

// What a style draws, with the per-kind constants every style of that kind carries.
struct Kind {
  uint16_t geometry;     // 1 point, 2 line, 3 area
  const char* category;  // shown as the style's category
  uint16_t sizeX, sizeY;
  uint16_t symbol;
  uint16_t subtype;
  int32_t order0, order1;
};
inline constexpr Kind kArea{3, "(AREA) Nautical", 100, 100, 3990, 0, 4, 6};
inline constexpr Kind kLine{2, "(LINE) Nautical", 200, 200, 6040, 3, 2081, 2096};
inline constexpr Kind kPoint{1, "(POINT) Nautical", 200, 200, 6045, 2, 0, 4};

// Scale limits a style is shown between (the same units as the chart layouts' scale steps).
// The detail chart takes over from the overview at kAllScales. Insight Map Creator uses 22851; half that keeps the
// overview on one zoom step further in, so zooming out swaps to it sooner.
inline constexpr int32_t kAllScales = 11425;      // detail chart: every scale it covers
inline constexpr int32_t kFineScales = 3808;      // detail chart: only zoomed in (fine contours and their labels)
inline constexpr int32_t kOverviewScales = 457022;

// The part every style starts with: what it is, its name, and when it shows.
inline std::vector<uint8_t> Header(const Kind& k, const std::string& name, int32_t coarsest, int32_t finest) {
  std::vector<uint8_t> b;
  U16(b, k.geometry);
  Str8(b, k.category);
  Str8(b, name);
  I32(b, coarsest);
  I32(b, finest);
  U16(b, k.sizeX);
  U16(b, k.sizeY);
  U16(b, k.symbol);
  U8(b, 1);
  U16(b, k.subtype);
  I32(b, k.order0);
  I32(b, k.order1);
  I32(b, 3);
  U8(b, 0);
  I32(b, 15);
  I32(b, coarsest);   // repeated for the drawing part
  I32(b, finest);
  return b;
}

// A filled area in one palette colour (AreaEntry adds the true RGB colour per chart).
inline std::vector<uint8_t> AreaStyle(const std::string& name, int32_t coarsest, int32_t finest, uint8_t palette, uint16_t tail) {
  auto b = Header(kArea, name, coarsest, finest);
  U8(b, 0);
  I32(b, 0);
  I32(b, 1);          // fill flags: palette colour
  U8(b, palette);
  I32(b, 1);
  I32(b, 16);
  U16(b, tail);
  return b;
}

// A contour line, one pixel wide, linked to its label style (ContourEntry sets colour, width and the link).
inline std::vector<uint8_t> LineStyle(const std::string& name, int32_t coarsest, int32_t finest, uint8_t labelStyle) {
  auto b = Header(kLine, name, coarsest, finest);
  U8(b, 0);
  I32(b, 0);
  I32(b, 0x93);       // pen flags
  U8(b, 0);
  U16(b, 1);          // line width
  U8(b, 0);
  U8(b, 1);
  I32(b, 1);
  I32(b, 16);
  U8(b, 1);
  U8(b, 0);
  U8(b, 3);
  U8(b, labelStyle);  // index of the label style in the library
  return b;
}

// The point where a contour's "N m" label sits.
inline std::vector<uint8_t> LabelStyle(const std::string& name, int32_t coarsest, int32_t finest) {
  auto b = Header(kPoint, name, coarsest, finest);
  Zeros(b, 5);
  I32(b, 314);
  U16(b, 15);
  U16(b, 68);
  U8(b, 0xFF); U8(b, 0xFF); U8(b, 0xFF);
  return b;
}

}  // namespace styles

// Styles for the detail chart (Large.at5). Style keys start with a for areas, l for lines and
// r for label points; all are 14 bytes. Palette numbers are the fallback colours for units
// without true-colour support (20 land, 15 deep water).
struct DetailStyles { Style land, subBlock, depthArea, contourFine, contourAll, labelFine, labelAll; };

inline DetailStyles DetailChartStyles() {
  using namespace styles;
  DetailStyles s;
  s.land = {Key("a=!!!=\xc3\xadl     "), AreaStyle("Land Area", kAllScales, 0, 20, 1)};
  s.subBlock = {Key("SubBlk        "), {}};
  s.subBlock.data.push_back(4);   // the sub-block style: kind 4, no drawing fields
  Zeros(s.subBlock.data, 20);
  s.depthArea = {Key("a7!!!6\xc3\xadl     "), AreaStyle("Depth Area", kAllScales, 0, 15, 1)};
  s.contourFine = {Key("lS#!%\xc3\xac-!=!k\xc3\xa6"), LineStyle("Depth Contours", kFineScales, 0, 19)};
  s.contourAll = {Key("lC#!%\xc3\xac!!=!k\xc3\xa6"), LineStyle("Depth Contours", kAllScales, 0, 21)};
  s.labelFine = {Key("rS#!%\xc3\xac-!=!k\xc3\xa6"), LabelStyle("Depth Contours", kFineScales, 0)};
  s.labelAll = {Key("rC#!%\xc3\xac!!=!k\xc3\xa6"), LabelStyle("Depth Contours", kAllScales, 0)};
  return s;
}

// Styles for the overview chart (Medium.at5): depth areas and land only. Keys are 9 bytes.
struct OverviewStyles { Style depthArea, land; };

inline OverviewStyles OverviewChartStyles() {
  using namespace styles;
  OverviewStyles s;
  s.depthArea = {Key("al!!!6\xc3\xadl"), AreaStyle("Depth Area", kOverviewScales, kAllScales, 15, 0x50)};
  s.land = {Key("ap!!!=\xc3\xadl"), AreaStyle("Land Area", kOverviewScales, kAllScales, 20, 0x50)};
  return s;
}

}  // namespace at5

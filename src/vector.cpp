#include "vector.hpp"
#include "at5.hpp"
#include "inflate.hpp"
#include "styles.hpp"
#include "sources.hpp"

#define NOMINMAX
#include <windows.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <tuple>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace vec {
namespace {

constexpr double kPi = 3.14159265358979323846;
const double kRl = at5::kLowranceR, kRw = at5::kWebR;

// ---- layout of a "Large" detail chart
constexpr int64_t kChild = 6000;              // child cell (metres)
constexpr int64_t kRootCell = kChild * 4;     // 24 km root cell (4x4 child grid)
constexpr int64_t kBlock = kRootCell * 20;    // 480 km block (20x20 root grid)
// Generated style library layout (see BuildLargeLib): 0 Land, 1 SubBlk, 2..2+n-1 depth bands,
// 2+n contour (detail), 3+n contour (all zooms), 4+n / 5+n contour label points.  Medium: 0 Land, 1..n bands.
enum { kSubBlk = 1 };
enum Role { kRoleLand = 0, kRoleBand = 1000, kRoleContourMinor = 2000, kRoleContourMajor = 2001, kRoleContourSafety = 2002 };
int TypeForRole(int role, int nBands, bool medium) {
  if (role == kRoleLand) return 0;
  if (role >= kRoleBand && role < kRoleBand + nBands) return (medium ? 1 : 2) + (role - kRoleBand);
  if (medium) return -1;
  if (role == kRoleContourMinor) return 2 + nBands;
  if (role == kRoleContourMajor) return 3 + nBands;
  if (role == kRoleContourSafety) return 6 + nBands;   // after the two label-point styles (4+n, 5+n)
  return -1;
}

// ------------------------------------------------------------------ byte helpers (AT5 encodings)
void Put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void Put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void PutVar(std::vector<uint8_t>& b, uint32_t v) {
  if (v < 0x80) { b.push_back(uint8_t((v << 1) | 1)); return; }
  if (v < 0x4000) { Put16(b, uint16_t((v << 2) | 2)); return; }
  if (v < 0x200000) { uint32_t w = (v << 3) | 4; b.push_back(uint8_t(w)); b.push_back(uint8_t(w >> 8)); b.push_back(uint8_t(w >> 16)); return; }
  if (v >= 0x20000000) throw std::runtime_error("varint overflow");
  Put32(b, v << 3);
}
void PutStr(std::vector<uint8_t>& b, const std::string& s) {
  if (s.size() < 0xFF) b.push_back(uint8_t(s.size())); else { b.push_back(0xFF); Put16(b, uint16_t(s.size())); }
  b.insert(b.end(), s.begin(), s.end());
}
void PutDelta(std::vector<uint8_t>& b, int64_t d) {  // i8, or 0x80 + i16
  if (d >= -127 && d <= 127) { b.push_back(uint8_t(int8_t(d))); return; }
  if (d < -32768 || d > 32767) throw std::runtime_error("coordinate delta out of range");
  b.push_back(0x80); Put16(b, uint16_t(int16_t(d)));
}
struct Pt { int64_t x, y; };
void PutRing(std::vector<uint8_t>& b, const std::vector<Pt>& r) {
  PutVar(b, uint32_t(r.size()));
  for (size_t i = 0; i < r.size(); ++i) {
    if (!i) { Put32(b, uint32_t(int32_t(r[0].x))); Put32(b, uint32_t(int32_t(r[0].y))); continue; }
    PutDelta(b, r[i].x - r[i - 1].x); PutDelta(b, r[i].y - r[i - 1].y);
  }
}

// ------------------------------------------------------------------ protobuf / MVT
struct Pb {
  const uint8_t* p; const uint8_t* e;
  bool More() const { return p < e; }
  uint64_t Var() { uint64_t v = 0; int s = 0; while (p < e) { uint8_t c = *p++; v |= uint64_t(c & 0x7f) << s; if (!(c & 0x80)) return v; s += 7; } throw std::runtime_error("pbf varint"); }
  void Key(int& f, int& w) { uint64_t k = Var(); f = int(k >> 3); w = int(k & 7); }
  Pb Sub() { uint64_t n = Var(); if (uint64_t(e - p) < n) throw std::runtime_error("pbf length"); Pb s{p, p + n}; p += n; return s; }
  void Skip(int w) { if (w == 0) Var(); else if (w == 1) p += 8; else if (w == 2) { uint64_t n = Var(); p += n; } else if (w == 5) p += 4; else throw std::runtime_error("pbf wiretype"); if (p > e) throw std::runtime_error("pbf skip"); }
};
struct Val { bool num = false; double d = 0; std::string s; };
struct Feat { int type = 0; std::vector<uint32_t> tags, geom; };
struct Layer { std::string name; uint32_t extent = 4096; std::vector<std::string> keys; std::vector<Val> vals; std::vector<Feat> feats; };

std::atomic<long long> gBadVecTiles{0};   // damaged vector tiles skipped in this run

std::vector<Layer> DecodeTileOrThrow(const std::vector<uint8_t>& raw) {
  std::vector<Layer> out;
  Pb t{raw.data(), raw.data() + raw.size()};
  while (t.More()) {
    int f, w; t.Key(f, w);
    if (f != 3 || w != 2) { t.Skip(w); continue; }
    Pb l = t.Sub(); Layer L;
    while (l.More()) {
      int lf, lw; l.Key(lf, lw);
      if (lf == 1 && lw == 2) { Pb s = l.Sub(); L.name.assign((const char*)s.p, size_t(s.e - s.p)); }
      else if (lf == 3 && lw == 2) { Pb s = l.Sub(); L.keys.emplace_back((const char*)s.p, size_t(s.e - s.p)); }
      else if (lf == 5 && lw == 0) { L.extent = uint32_t(l.Var()); if (!L.extent) L.extent = 4096; }
      else if (lf == 4 && lw == 2) {
        Pb v = l.Sub(); Val V;
        while (v.More()) {
          int vf, vw; v.Key(vf, vw);
          if (vf == 1 && vw == 2) { Pb s = v.Sub(); V.s.assign((const char*)s.p, size_t(s.e - s.p)); }
          else if (vf == 2 && vw == 5) { if (v.e - v.p < 4) throw std::runtime_error("pbf float"); float x; memcpy(&x, v.p, 4); v.p += 4; V.num = true; V.d = x; }
          else if (vf == 3 && vw == 1) { if (v.e - v.p < 8) throw std::runtime_error("pbf double"); double x; memcpy(&x, v.p, 8); v.p += 8; V.num = true; V.d = x; }
          else if ((vf == 4 || vf == 5) && vw == 0) { V.num = true; V.d = double(int64_t(v.Var())); }
          else if (vf == 6 && vw == 0) { uint64_t z = v.Var(); V.num = true; V.d = double(int64_t((z >> 1) ^ (~(z & 1) + 1))); }
          else if (vf == 7 && vw == 0) { V.num = true; V.d = double(v.Var()); }
          else v.Skip(vw);
        }
        if (!V.num) { char* end = nullptr; double d = strtod(V.s.c_str(), &end); if (end && end != V.s.c_str() && *end == 0) { V.num = true; V.d = d; } }
        L.vals.push_back(std::move(V));
      } else if (lf == 2 && lw == 2) {
        Pb fe = l.Sub(); Feat F;
        while (fe.More()) {
          int ff, fw; fe.Key(ff, fw);
          if ((ff == 2 || ff == 4) && fw == 2) { Pb s = fe.Sub(); auto& dst = ff == 2 ? F.tags : F.geom; while (s.More()) dst.push_back(uint32_t(s.Var())); }
          else if (ff == 3 && fw == 0) F.type = int(fe.Var());
          else fe.Skip(fw);
        }
        L.feats.push_back(std::move(F));
      } else l.Skip(lw);
    }
    out.push_back(std::move(L));
  }
  return out;
}

// a damaged tile is skipped (counted, reported at the end) instead of stopping the whole chart
std::vector<Layer> DecodeTile(const std::vector<uint8_t>& raw) {
  try { return DecodeTileOrThrow(raw); }
  catch (const std::exception&) { ++gBadVecTiles; return {}; }
}

using Ring = std::vector<std::pair<double, double>>;
std::vector<Ring> DecodeGeom(const std::vector<uint32_t>& g) {
  std::vector<Ring> rings; int64_t x = 0, y = 0;
  auto zz = [](uint32_t v) { return int64_t(int32_t((v >> 1) ^ (0u - (v & 1)))); };  // zigzag -> signed
  for (size_t i = 0; i < g.size();) {
    uint32_t cmd = g[i] & 7, cnt = g[i] >> 3; ++i;
    if (cmd == 7) { if (!rings.empty() && !rings.back().empty()) rings.back().push_back(rings.back().front()); continue; }
    if (cmd != 1 && cmd != 2) break;
    for (uint32_t k = 0; k < cnt; ++k) {
      if (i + 1 >= g.size()) return rings;
      x += zz(g[i]); y += zz(g[i + 1]); i += 2;
      if (cmd == 1 || rings.empty()) rings.emplace_back();
      rings.back().push_back({double(x), double(y)});
    }
  }
  return rings;
}

// ------------------------------------------------------------------ clipping (double coords)
Ring ClipPoly(const Ring& in, double x0, double y0, double x1, double y1) {
  Ring cur = in, nxt;
  for (int edge = 0; edge < 4 && !cur.empty(); ++edge) {
    nxt.clear();
    auto inside = [&](const std::pair<double, double>& p) {
      switch (edge) { case 0: return p.first >= x0; case 1: return p.first <= x1; case 2: return p.second >= y0; default: return p.second <= y1; }
    };
    auto inter = [&](const std::pair<double, double>& a, const std::pair<double, double>& b) {
      double t;
      switch (edge) {
        case 0: t = (x0 - a.first) / (b.first - a.first); return std::make_pair(x0, a.second + t * (b.second - a.second));
        case 1: t = (x1 - a.first) / (b.first - a.first); return std::make_pair(x1, a.second + t * (b.second - a.second));
        case 2: t = (y0 - a.second) / (b.second - a.second); return std::make_pair(a.first + t * (b.first - a.first), y0);
        default: t = (y1 - a.second) / (b.second - a.second); return std::make_pair(a.first + t * (b.first - a.first), y1);
      }
    };
    for (size_t i = 0; i < cur.size(); ++i) {
      const auto& a = cur[i]; const auto& b = cur[(i + 1) % cur.size()];
      bool ia = inside(a), ib = inside(b);
      if (ia) nxt.push_back(a);
      if (ia != ib) nxt.push_back(inter(a, b));
    }
    cur.swap(nxt);
  }
  return cur;
}
std::vector<Ring> ClipLine(const Ring& in, double x0, double y0, double x1, double y1) {
  std::vector<Ring> out; Ring cur;
  for (size_t i = 0; i + 1 < in.size(); ++i) {
    double ax = in[i].first, ay = in[i].second, bx = in[i + 1].first, by = in[i + 1].second;
    double t0 = 0, t1 = 1, dx = bx - ax, dy = by - ay;
    double p[4] = {-dx, dx, -dy, dy}, q[4] = {ax - x0, x1 - ax, ay - y0, y1 - ay};
    bool ok = true;
    for (int k = 0; k < 4 && ok; ++k) {
      if (p[k] == 0) { if (q[k] < 0) ok = false; }
      else { double r = q[k] / p[k]; if (p[k] < 0) t0 = std::max(t0, r); else t1 = std::min(t1, r); if (t0 > t1) ok = false; }
    }
    if (!ok) { if (cur.size() > 1) out.push_back(cur); cur.clear(); continue; }
    std::pair<double, double> A{ax + t0 * dx, ay + t0 * dy}, B{ax + t1 * dx, ay + t1 * dy};
    if (cur.empty()) cur.push_back(A);
    else if (cur.back() != A) { if (cur.size() > 1) out.push_back(cur); cur.clear(); cur.push_back(A); }
    cur.push_back(B);
    if (t1 < 1) { out.push_back(cur); cur.clear(); }
  }
  if (cur.size() > 1) out.push_back(cur);
  return out;
}
std::vector<Pt> Quantize(const Ring& r, bool closed) {
  std::vector<Pt> o;
  for (auto& p : r) {
    Pt q{int64_t(std::llround(p.first)), int64_t(std::llround(p.second))};
    if (o.empty() || o.back().x != q.x || o.back().y != q.y) o.push_back(q);
  }
  if (closed) {
    while (o.size() > 1 && o.back().x == o.front().x && o.back().y == o.front().y) o.pop_back();
    if (o.size() < 3) return {};
    o.push_back(o.front());
  } else if (o.size() < 2) return {};
  return o;
}

// An area ring rounded to whole units. Rounding can make a ring that runs very close to itself (a thin band on a
// steep wall, or a cut along a cell edge) double back on itself or come back to a point it has already passed.
// Plotters fill areas by cutting them into triangles, and that fails on such rings: the missed triangles show as
// land. So back-tracks are dropped and the ring is split at repeated points into simple loops; each loop keeps
// its direction, so exteriors stay exteriors and holes stay holes. Loops are returned closed, like Quantize.
std::vector<std::vector<Pt>> QuantizeArea(const Ring& r) {
  std::vector<Pt> o;
  for (auto& p : r) {
    Pt q{int64_t(std::llround(p.first)), int64_t(std::llround(p.second))};
    if (o.empty() || o.back().x != q.x || o.back().y != q.y) o.push_back(q);
  }
  while (o.size() > 1 && o.back().x == o.front().x && o.back().y == o.front().y) o.pop_back();
  auto same = [](const Pt& a, const Pt& b) { return a.x == b.x && a.y == b.y; };
  // drop spikes: a point where the ring turns straight back (A B A, or B on the way back along A-C)
  auto despike = [&](std::vector<Pt>& v) {
    bool changed = true;
    while (changed && v.size() >= 3) {
      changed = false;
      for (size_t i = 0; i < v.size() && v.size() >= 3;) {
        const size_t n = v.size();
        const Pt& a = v[(i + n - 1) % n]; const Pt& b = v[i]; const Pt& c = v[(i + 1) % n];
        const int64_t cr = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
        const int64_t dt = (b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y);
        if (same(b, c) || (cr == 0 && dt <= 0)) { v.erase(v.begin() + long(i)); changed = true; }
        else ++i;
      }
    }
  };
  auto area2 = [](const std::vector<Pt>& v) {
    long double a = 0;
    for (size_t i = 0; i < v.size(); ++i) { const Pt& p = v[i]; const Pt& q = v[(i + 1) % v.size()]; a += (long double)p.x * q.y - (long double)q.x * p.y; }
    return a;
  };
  despike(o);
  std::vector<std::vector<Pt>> loops;
  if (o.size() < 3) return loops;
  // split at repeated points: walk the ring; coming back to a point on the path closes a loop
  std::vector<Pt> path;
  std::map<std::pair<int64_t, int64_t>, size_t> where;
  auto emit = [&](std::vector<Pt> v) {
    despike(v);
    if (v.size() < 3 || area2(v) == 0) return;
    v.push_back(v.front());
    loops.push_back(std::move(v));
  };
  for (const Pt& p : o) {
    auto it = where.find({p.x, p.y});
    if (it != where.end()) {
      const size_t j = it->second;
      std::vector<Pt> loop(path.begin() + long(j), path.end());
      for (size_t k = j + 1; k < path.size(); ++k) where.erase({path[k].x, path[k].y});
      path.resize(j + 1);
      emit(std::move(loop));
      continue;
    }
    where[{p.x, p.y}] = path.size();
    path.push_back(p);
  }
  emit(std::move(path));
  return loops;
}

// Last safety net for one area's rounded rings: where two of them cross (a thin band rounded onto its neighbour
// on a steep wall, or in the zoomed-out chart's coarse units), the smaller one is dropped. At worst a sliver
// loses its own colour (the neighbouring area covers it); a plotter can always fill what is left. Segments are
// bucketed on a grid, so a big area with many holes is checked quickly.
void DropCrossingRings(std::vector<std::vector<Pt>>& parts) {
  if (parts.size() < 2) return;
  struct Seg { Pt a, b; uint32_t ring; };
  std::vector<Seg> segs;
  int64_t x0 = INT64_MAX, y0 = INT64_MAX, x1 = INT64_MIN, y1 = INT64_MIN;
  for (uint32_t r = 0; r < parts.size(); ++r)
    for (size_t i = 0; i + 1 < parts[r].size(); ++i) {
      const Pt& a = parts[r][i]; const Pt& b = parts[r][i + 1];
      segs.push_back({a, b, r});
      x0 = std::min({x0, a.x, b.x}); x1 = std::max({x1, a.x, b.x}); y0 = std::min({y0, a.y, b.y}); y1 = std::max({y1, a.y, b.y});
    }
  if (segs.empty()) return;
  const double span = double(std::max<int64_t>({x1 - x0, y1 - y0, 1}));
  const int G = int(std::clamp(std::sqrt(double(segs.size()) / 4.0), 1.0, 512.0));
  const double cell = span / G + 1e-9;
  std::vector<std::vector<uint32_t>> grid(size_t(G) * size_t(G));
  for (uint32_t i = 0; i < segs.size(); ++i) {
    const Seg& s = segs[i];
    const int cx0 = int(double(std::min(s.a.x, s.b.x) - x0) / cell), cx1 = int(double(std::max(s.a.x, s.b.x) - x0) / cell);
    const int cy0 = int(double(std::min(s.a.y, s.b.y) - y0) / cell), cy1 = int(double(std::max(s.a.y, s.b.y) - y0) / cell);
    for (int cy = std::max(0, cy0); cy <= std::min(G - 1, cy1); ++cy)
      for (int cx = std::max(0, cx0); cx <= std::min(G - 1, cx1); ++cx) grid[size_t(cy) * G + cx].push_back(i);
  }
  auto orient = [](const Pt& p, const Pt& q, const Pt& r) {
    const int64_t v = (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);   // whole metres in a block: fits
    return (v > 0) - (v < 0);
  };
  std::set<std::pair<uint32_t, uint32_t>> crossing;
  for (auto& bucket : grid)
    for (size_t i = 0; i < bucket.size(); ++i)
      for (size_t j = i + 1; j < bucket.size(); ++j) {
        const Seg& s = segs[bucket[i]]; const Seg& t = segs[bucket[j]];
        if (s.ring == t.ring) continue;
        if (orient(s.a, s.b, t.a) * orient(s.a, s.b, t.b) < 0 && orient(t.a, t.b, s.a) * orient(t.a, t.b, s.b) < 0)
          crossing.insert({std::min(s.ring, t.ring), std::max(s.ring, t.ring)});
      }
  if (crossing.empty()) return;
  auto area = [](const std::vector<Pt>& r) {
    long double a = 0;
    for (size_t i = 0; i + 1 < r.size(); ++i) a += (long double)r[i].x * r[i + 1].y - (long double)r[i + 1].x * r[i].y;
    return std::fabs((double)a);
  };
  std::vector<char> drop(parts.size(), 0);
  for (auto [a, b] : crossing) {
    if (drop[a] || drop[b]) continue;
    drop[area(parts[a]) < area(parts[b]) ? a : b] = 1;
  }
  std::vector<std::vector<Pt>> kept;
  for (size_t i = 0; i < parts.size(); ++i) if (!drop[i]) kept.push_back(std::move(parts[i]));
  parts.swap(kept);
}

// ------------------------------------------------------------------ source
std::vector<std::string> SplitList(const std::string& s) {
  std::vector<std::string> v; std::string cur;
  for (char c : s) { if (c == ',') { if (!cur.empty()) v.push_back(cur); cur.clear(); } else cur += char(tolower((unsigned char)c)); }
  if (!cur.empty()) v.push_back(cur);
  return v;
}
std::string Lower(std::string s) { for (auto& c : s) c = char(tolower((unsigned char)c)); return s; }
bool InList(const std::vector<std::string>& l, const std::string& name) { return std::find(l.begin(), l.end(), Lower(name)) != l.end(); }

struct Source {
  std::string path; int zoom = 0, minZoom = 0, maxZoom = 0;
  std::vector<std::vector<std::pair<uint64_t, int64_t>>> rows;  // per zoom: (key, rowid) sorted
  bool useRowid = false;
  static uint64_t Key(int64_t x, int64_t y) { return uint64_t(x) << 32 | uint64_t(y & 0xFFFFFFFF); }
  bool Has(int z, int64_t x, int64_t y) const {
    if (z < 0 || z >= int(rows.size())) return false;
    const auto& v = rows[size_t(z)];
    auto it = std::lower_bound(v.begin(), v.end(), std::make_pair(Key(x, y), int64_t(INT64_MIN)));
    return it != v.end() && it->first == Key(x, y);
  }
  // reached[z][d]: ancestors (d levels up) of the tiles that exist at zoom z
  std::vector<std::vector<std::unordered_set<uint64_t>>> reached;
  // Where to read tile (z, x, y): d >= 0 = itself (0) or its ancestor d levels up (overzoom, only where the data
  // never reaches zoom z); -1 = an empty tile (zoom z exists around it, it simply has no features); -2 = nothing.
  int Fallback(int z, int64_t x, int64_t y) const {
    for (int d = 0; d <= 6 && z - d >= minZoom; ++d)
      if (Has(z - d, x >> d, y >> d)) {
        if (d > 0 && z < int(reached.size()) && reached[size_t(z)].size() > size_t(d) && reached[size_t(z)][size_t(d)].count(Key(x >> d, y >> d))) return -1;
        return d;
      }
    return -2;
  }
  // a tile of zoom z exists within `rad` tiles of (x, y)
  bool NearData(int z, int64_t x, int64_t y, int rad) const {
    for (int64_t dy = -rad; dy <= rad; ++dy) for (int64_t dx = -rad; dx <= rad; ++dx) if (Has(z, x + dx, y + dy)) return true;
    return false;
  }
  // An empty tile inside the data: tiles of zoom z exist within `rad` tiles in all four directions (a gap in the
  // sea floor data, e.g. flat bottom or a wide reef flat). Empty tiles open to one side (inland) are not.
  bool InsideData(int z, int64_t x, int64_t y, int rad) const {
    auto any = [&](int64_t dx, int64_t dy) { for (int k = 1; k <= rad; ++k) if (Has(z, x + dx * k, y + dy * k)) return true; return false; };
    return any(1, 0) && any(-1, 0) && any(0, 1) && any(0, -1);
  }
  int64_t RowId(int z, int64_t x, int64_t y) const {
    const auto& v = rows[size_t(z)];
    auto it = std::lower_bound(v.begin(), v.end(), std::make_pair(Key(x, y), int64_t(INT64_MIN)));
    return it->second;
  }
};

Source OpenSource(const std::string& path, int zoom) {
  Source s; s.path = path;
  sqlite3* db = src::OpenTilesDb(path);
  sqlite3_stmt* st = nullptr;
  s.useRowid = sqlite3_prepare_v2(db, "SELECT zoom_level, tile_column, tile_row, rowid FROM tiles", -1, &st, nullptr) == SQLITE_OK;
  if (!s.useRowid) { sqlite3_finalize(st); sqlite3_prepare_v2(db, "SELECT zoom_level, tile_column, tile_row FROM tiles", -1, &st, nullptr); }
  s.rows.resize(31);
  s.minZoom = 99; s.maxZoom = -1;
  for (bool first = true; st && src::Step(st, first) == SQLITE_ROW; first = false) {
    int z = sqlite3_column_int(st, 0);
    if (z < 0 || z > 30) continue;
    int64_t x = sqlite3_column_int64(st, 1), y = ((int64_t(1) << z) - 1) - sqlite3_column_int64(st, 2);
    s.rows[size_t(z)].push_back({Source::Key(x, y), s.useRowid ? sqlite3_column_int64(st, 3) : 0});
    s.minZoom = std::min(s.minZoom, z); s.maxZoom = std::max(s.maxZoom, z);
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
  if (s.maxZoom < 0) throw std::runtime_error("no tiles in " + path);
  for (auto& v : s.rows) std::sort(v.begin(), v.end());
  s.reached.resize(s.rows.size());
  for (size_t z = 0; z < s.rows.size(); ++z) {
    if (s.rows[z].empty()) continue;
    s.reached[z].resize(7);
    for (auto& t : s.rows[z])
      for (int d = 1; d <= 6; ++d) s.reached[z][size_t(d)].insert(Source::Key(int64_t(t.first >> 32) >> d, int64_t(t.first & 0xFFFFFFFF) >> d));
  }
  s.zoom = zoom >= 0 ? std::clamp(zoom, s.minZoom, s.maxZoom) : s.maxZoom;
  return s;
}

struct TileReader {
  const Source& s; sqlite3* db = nullptr; sqlite3_stmt* st = nullptr;
  explicit TileReader(const Source& src) : s(src) {
    db = src::OpenTilesDb(s.path);
    if (s.useRowid) sqlite3_prepare_v2(db, "SELECT tile_data FROM tiles WHERE rowid=?1", -1, &st, nullptr);
    else sqlite3_prepare_v2(db, "SELECT tile_data FROM tiles WHERE zoom_level=?1 AND tile_column=?2 AND tile_row=?3", -1, &st, nullptr);
  }
  ~TileReader() { sqlite3_finalize(st); sqlite3_close(db); }
  bool Read(int z, int64_t x, int64_t y, std::vector<uint8_t>& out) {
    if (!s.Has(z, x, y)) return false;
    sqlite3_reset(st);
    if (s.useRowid) sqlite3_bind_int64(st, 1, s.RowId(z, x, y));
    else { sqlite3_bind_int(st, 1, z); sqlite3_bind_int64(st, 2, x); sqlite3_bind_int64(st, 3, ((int64_t(1) << z) - 1) - y); }
    if (src::Step(st) != SQLITE_ROW) return false;
    auto p = (const uint8_t*)sqlite3_column_blob(st, 0);
    int n = sqlite3_column_bytes(st, 0);
    try { out = inflate::Auto(p, size_t(n)); }
    catch (const std::exception&) { out.clear(); ++gBadVecTiles; }   // damaged compression: skipped
    return true;
  }
};

// ------------------------------------------------------------------ external land (coastline files)
// Land polygons in longitude / latitude (WGS84 or GDA94) from an ESRI shapefile - e.g. Geoscience Australia
// GEODATA COAST 100K (polygons coded mainland / island / sea; sea is skipped), GSHHG or OpenStreetMap land
// polygons - or from sat2at5's compact .land file (tools/make_land.py). Projected to Lowrance metres, kept only
// around the chart, and cut into a quadtree of small pieces, so a tile or block finds its land fast.
class LandSource {
 public:
  struct Piece { std::vector<Ring> rings; double bb[4]; };

  // Adds the land of a coastline file. box (Lowrance metres x0,y0,x1,y1, or null): only land there is kept,
  // clipped to it. skip (lon/lat W,S,E,N, or null): polygons lying wholly inside it are left out (where a more
  // detailed file gives the land). bounds (lon/lat, out, or null): the extent of all the file's polygons.
  // Returns the number of polygons added.
  size_t Add(const std::string& path, const double* box, const double* skip, double* bounds = nullptr) {
    std::vector<uint8_t> d = ReadAll(path);
    double llBox[4] = {-1e9, -1e9, 1e9, 1e9};
    if (box) { llBox[0] = Lon(box[0]); llBox[1] = Lat(box[1]); llBox[2] = Lon(box[2]); llBox[3] = Lat(box[3]); }
    double fb[4] = {1e300, 1e300, -1e300, -1e300};
    size_t total = 0, n = 0;
    // a polygon with lon/lat extent bb and its rings (lon/lat pairs, generated on demand)
    auto take = [&](const double* bb, auto&& rings) {
      ++total;
      fb[0] = std::min(fb[0], bb[0]); fb[1] = std::min(fb[1], bb[1]); fb[2] = std::max(fb[2], bb[2]); fb[3] = std::max(fb[3], bb[3]);
      if (bb[0] > llBox[2] || bb[2] < llBox[0] || bb[1] > llBox[3] || bb[3] < llBox[1]) return;
      if (skip && bb[0] >= skip[0] && bb[1] >= skip[1] && bb[2] <= skip[2] && bb[3] <= skip[3]) return;
      std::vector<Ring> rs = rings();
      if (rs.empty()) return;
      const bool inside = bb[0] >= llBox[0] && bb[1] >= llBox[1] && bb[2] <= llBox[2] && bb[3] <= llBox[3];
      if (box && !inside) {   // keep the part around the chart only (large land masses)
        std::vector<Ring> cut;
        for (size_t i = 0; i < rs.size(); ++i) {
          Ring c = ClipPoly(rs[i], box[0], box[1], box[2], box[3]);
          if (c.size() < 3) { if (i == 0) break; continue; }
          if (c.front() != c.back()) c.push_back(c.front());
          cut.push_back(std::move(c));
        }
        rs.swap(cut);
        if (rs.empty()) return;
      }
      Piece pc; pc.rings = std::move(rs); Bound(pc);
      polys_.push_back(std::move(pc));
      ++n;
    };
    auto proj = [](double lon, double lat) {
      lat = std::clamp(lat, -85.0, 85.0);
      return std::pair<double, double>{kRl * lon * kPi / 180, kRl * std::log(std::tan(kPi / 4 + lat * kPi / 360))};
    };
    if (d.size() >= 12 && !memcmp(d.data(), "S2ALAND1", 8)) {
      uint32_t cnt; memcpy(&cnt, &d[8], 4);
      size_t o = 12;
      for (uint32_t k = 0; k < cnt && o + 20 <= d.size(); ++k) {
        int32_t ib[4]; uint32_t len;
        memcpy(ib, &d[o], 16); memcpy(&len, &d[o + 16], 4);
        const size_t c = o + 20; o = c + len;
        if (o > d.size()) throw std::runtime_error("damaged coastline file " + path);
        const double bb[4] = {ib[0] * 1e-7, ib[1] * 1e-7, ib[2] * 1e-7, ib[3] * 1e-7};
        take(bb, [&] {
          size_t q = c;
          auto var = [&]() -> uint64_t {
            uint64_t v = 0; int sh = 0;
            while (q < o) { const uint8_t b = d[q++]; v |= uint64_t(b & 0x7F) << sh; if (!(b & 0x80)) break; sh += 7; }
            return v;
          };
          auto zig = [&]() -> int64_t { const uint64_t v = var(); return int64_t(v >> 1) ^ -int64_t(v & 1); };
          std::vector<Ring> rs;
          const uint64_t nr = var();
          for (uint64_t r = 0; r < nr && q < o; ++r) {
            const uint64_t np = var();
            Ring ring; ring.reserve(size_t(np));
            int64_t x = 0, y = 0;
            for (uint64_t i = 0; i < np; ++i) { x += zig(); y += zig(); ring.push_back(proj(double(x) * 1e-7, double(y) * 1e-7)); }
            if (ring.size() >= 4) rs.push_back(std::move(ring));
          }
          return rs;
        });
      }
    } else {
      if (d.size() < 100 || d[0] != 0 || d[1] != 0 || d[2] != 0x27 || d[3] != 0x0A)
        throw std::runtime_error("not a coastline file (.shp or .land): " + path);
      std::vector<bool> keep = SeaFilter(path.substr(0, path.size() - 4) + ".dbf");
      size_t o = 100, rec = 0;
      auto i32 = [&](size_t p) { int32_t v; memcpy(&v, &d[p], 4); return v; };
      auto f64 = [&](size_t p) { double v; memcpy(&v, &d[p], 8); return v; };
      while (o + 8 <= d.size()) {
        const uint32_t len = (uint32_t(d[o + 4]) << 24 | uint32_t(d[o + 5]) << 16 | uint32_t(d[o + 6]) << 8 | d[o + 7]) * 2u;
        const size_t c = o + 8;
        o = c + len;
        if (o > d.size()) break;
        const size_t idx = rec++;
        const int type = i32(c);
        if (type != 5 && type != 15 && type != 25) continue;     // polygon (plain, Z, M)
        if (idx < keep.size() && !keep[idx]) continue;           // a 'sea' polygon
        const int32_t parts = i32(c + 36), points = i32(c + 40);
        if (parts <= 0 || points <= 0 || c + 44 + size_t(parts) * 4 + size_t(points) * 16 > o) continue;
        const double bb[4] = {f64(c + 4), f64(c + 12), f64(c + 20), f64(c + 28)};
        take(bb, [&] {
          std::vector<Ring> rs;
          for (int32_t k = 0; k < parts; ++k) {
            const int32_t a = i32(c + 44 + size_t(k) * 4), b = k + 1 < parts ? i32(c + 44 + size_t(k + 1) * 4) : points;
            Ring r;
            for (int32_t j = a; j < b; ++j) {
              const size_t p = c + 44 + size_t(parts) * 4 + size_t(j) * 16;
              r.push_back(proj(f64(p), f64(p + 8)));
            }
            if (r.size() >= 4) rs.push_back(std::move(r));     // shapefile: outer rings clockwise, holes anticlockwise
          }
          return rs;
        });
      }
    }
    if (!total) throw std::runtime_error("no land polygons in " + path);
    if (bounds) for (int k = 0; k < 4; ++k) bounds[k] = fb[k];
    return n;
  }

  // builds the quadtree of everything added
  void Finish() {
    nodes_.clear();
    if (polys_.empty()) return;
    double bb[4] = {1e300, 1e300, -1e300, -1e300};
    for (auto& pc : polys_) { bb[0] = std::min(bb[0], pc.bb[0]); bb[1] = std::min(bb[1], pc.bb[1]); bb[2] = std::max(bb[2], pc.bb[2]); bb[3] = std::max(bb[3], pc.bb[3]); }
    nodes_.push_back({{bb[0], bb[1], bb[2], bb[3]}, {}, {-1, -1, -1, -1}});
    Build(0, std::move(polys_), 0);
    polys_.clear();
  }

  size_t Load(const std::string& path) { const size_t n = Add(path, nullptr, nullptr); Finish(); return n; }
  bool Empty() const { return nodes_.empty(); }

  // point (Lowrance metres) on land
  bool Contains(double x, double y) const {
    std::vector<const Piece*> ps;
    Query(x, y, x, y, ps);
    for (auto* pc : ps) {
      bool in = false;
      for (auto& r : pc->rings)
        for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
          if ((r[i].second > y) != (r[j].second > y) &&
              x < (r[j].first - r[i].first) * (y - r[i].second) / (r[j].second - r[i].second) + r[i].first) in = !in;
      if (in) return true;
    }
    return false;
  }

  void Query(double x0, double y0, double x1, double y1, std::vector<const Piece*>& out) const {
    if (nodes_.empty()) return;
    std::vector<int> stack{0};
    while (!stack.empty()) {
      const Node& nd = nodes_[size_t(stack.back())]; stack.pop_back();
      if (nd.bb[0] > x1 || nd.bb[2] < x0 || nd.bb[1] > y1 || nd.bb[3] < y0) continue;
      for (auto& pc : nd.pieces) if (!(pc.bb[0] > x1 || pc.bb[2] < x0 || pc.bb[1] > y1 || pc.bb[3] < y0)) out.push_back(&pc);
      for (int ch : nd.child) if (ch >= 0) stack.push_back(ch);
    }
  }

 private:
  struct Node { double bb[4]; std::vector<Piece> pieces; int child[4]; };
  std::vector<Node> nodes_;
  std::vector<Piece> polys_;   // added, not yet in the quadtree

  static double Lon(double x) { return x / kRl * 180 / kPi; }
  static double Lat(double y) { return (2 * std::atan(std::exp(y / kRl)) - kPi / 2) * 180 / kPi; }

  static void Grow(double* bb, const std::pair<double, double>& p) {
    bb[0] = std::min(bb[0], p.first); bb[1] = std::min(bb[1], p.second); bb[2] = std::max(bb[2], p.first); bb[3] = std::max(bb[3], p.second);
  }
  static void Bound(Piece& pc) {
    pc.bb[0] = pc.bb[1] = 1e300; pc.bb[2] = pc.bb[3] = -1e300;
    for (auto& r : pc.rings) for (auto& p : r) Grow(pc.bb, p);
  }
  // pieces are split into quadrants until small (<= 2000 points) or the cell is ~5 km
  void Build(int ni, std::vector<Piece> pieces, int depth) {
    size_t pts = 0;
    for (auto& pc : pieces) for (auto& r : pc.rings) pts += r.size();
    const double* b = nodes_[size_t(ni)].bb;
    if (pts <= 2000 || depth >= 16 || (b[2] - b[0]) < 5000) { nodes_[size_t(ni)].pieces = std::move(pieces); return; }
    const double mx = (b[0] + b[2]) / 2, my = (b[1] + b[3]) / 2;
    const double q[4][4] = {{b[0], b[1], mx, my}, {mx, b[1], b[2], my}, {b[0], my, mx, b[3]}, {mx, my, b[2], b[3]}};
    std::vector<Piece> sub[4];
    for (auto& pc : pieces)
      for (int k = 0; k < 4; ++k) {
        if (pc.bb[0] > q[k][2] || pc.bb[2] < q[k][0] || pc.bb[1] > q[k][3] || pc.bb[3] < q[k][1]) continue;
        if (pc.bb[0] >= q[k][0] && pc.bb[2] <= q[k][2] && pc.bb[1] >= q[k][1] && pc.bb[3] <= q[k][3]) { sub[k].push_back(pc); continue; }
        Piece cp;
        for (size_t i = 0; i < pc.rings.size(); ++i) {
          Ring c = ClipPoly(pc.rings[i], q[k][0], q[k][1], q[k][2], q[k][3]);
          if (c.size() < 3) { if (i == 0) break; continue; }
          if (c.front() != c.back()) c.push_back(c.front());
          cp.rings.push_back(std::move(c));
        }
        if (cp.rings.empty()) continue;
        Bound(cp);
        sub[k].push_back(std::move(cp));
      }
    pieces.clear(); pieces.shrink_to_fit();
    for (int k = 0; k < 4; ++k) {
      if (sub[k].empty()) continue;
      const int ci = int(nodes_.size());
      nodes_.push_back({{q[k][0], q[k][1], q[k][2], q[k][3]}, {}, {-1, -1, -1, -1}});
      nodes_[size_t(ni)].child[k] = ci;
      Build(ci, std::move(sub[k]), depth + 1);
    }
  }
  static std::vector<uint8_t> ReadAll(const std::string& path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f) throw std::runtime_error("cannot open " + path);
    _fseeki64(f, 0, SEEK_END); const int64_t n = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    std::vector<uint8_t> d(size_t(std::max<int64_t>(n, 0)));
    if (n > 0 && fread(d.data(), 1, d.size(), f) != d.size()) { fclose(f); throw std::runtime_error("read error " + path); }
    fclose(f);
    return d;
  }
  // per record: keep (true) unless a FEAT_CODE / FEATURE / TYPE text field says "sea" (or "water"/"ocean")
  static std::vector<bool> SeaFilter(const std::string& dbfPath) {
    std::vector<bool> keep;
    FILE* f = nullptr;
    if (fopen_s(&f, dbfPath.c_str(), "rb") || !f) return keep;
    fclose(f);
    std::vector<uint8_t> d = ReadAll(dbfPath);
    if (d.size() < 32) return keep;
    const uint32_t n = uint32_t(d[4] | d[5] << 8 | d[6] << 16 | uint32_t(d[7]) << 24);
    const uint16_t hl = uint16_t(d[8] | d[9] << 8), rl = uint16_t(d[10] | d[11] << 8);
    size_t o = 32, off = 1; int fo = -1, fl = 0;
    while (o + 32 <= d.size() && d[o] != 0x0D) {
      std::string name(reinterpret_cast<const char*>(&d[o]), strnlen(reinterpret_cast<const char*>(&d[o]), 11));
      const int len = d[o + 16];
      const std::string ln = Lower(name);
      if ((ln == "feat_code" || ln == "feature" || ln == "type" || ln == "class") && fo < 0) { fo = int(off); fl = len; }
      off += size_t(len); o += 32;
    }
    keep.assign(n, true);
    if (fo < 0) return keep;
    for (uint32_t i = 0; i < n; ++i) {
      const size_t p = size_t(hl) + size_t(i) * rl + size_t(fo);
      if (p + size_t(fl) > d.size()) break;
      std::string v = Lower(std::string(reinterpret_cast<const char*>(&d[p]), size_t(fl)));
      v.erase(v.find_last_not_of(' ') + 1);
      if (v == "sea" || v == "water" || v == "ocean") keep[i] = false;
    }
    return keep;
  }
};
const LandSource* gLand = nullptr;   // set by Convert when a coastline file is given (shared, read-only)

// a coastline file shipped with sat2at5: <exe>\coastlines, <exe>\..\coastlines or <exe>\land; "" if none
std::string BuiltinLand(const char* name) {
  char exe[MAX_PATH * 4] = {};
  GetModuleFileNameA(nullptr, exe, DWORD(sizeof exe));
  const std::filesystem::path dir = std::filesystem::path(exe).parent_path();
  for (const auto& sub : {dir / "coastlines", dir.parent_path() / "coastlines", dir / "land"}) {
    std::error_code ec;
    if (std::filesystem::exists(sub / name, ec)) return (sub / name).string();
  }
  return "";
}

// land pieces in a rectangle, clipped to it (rings closed)
std::vector<std::vector<Ring>> LandIn(double x0, double y0, double x1, double y1) {
  std::vector<std::vector<Ring>> out;
  if (!gLand) return out;
  std::vector<const LandSource::Piece*> ps;
  gLand->Query(x0, y0, x1, y1, ps);
  for (auto* pc : ps) {
    std::vector<Ring> rings;
    for (size_t i = 0; i < pc->rings.size(); ++i) {
      Ring c = ClipPoly(pc->rings[i], x0, y0, x1, y1);
      if (c.size() < 3) { if (i == 0) break; continue; }
      if (c.front() != c.back()) c.push_back(c.front());
      rings.push_back(std::move(c));
    }
    if (!rings.empty()) out.push_back(std::move(rings));
  }
  return out;
}

// ------------------------------------------------------------------ classification
struct Classifier {
  const Options& o;
  std::vector<std::string> land, depth, contour;
  std::vector<std::string> depthKeys, contourKeys;
  std::vector<std::pair<double, uint32_t>> bands;
  explicit Classifier(const Options& opt) : o(opt) {
    land = SplitList(o.landLayers); depth = SplitList(o.depthLayers); contour = SplitList(o.contourLayers);
    depthKeys = o.depthAttr.empty() ? std::vector<std::string>{"drval1", "depth_min", "mindepth", "min_depth", "depth", "drval2", "depth_max", "value"} : SplitList(o.depthAttr);
    contourKeys = o.contourAttr.empty() ? std::vector<std::string>{"valdco", "depth", "contour", "elevation", "ele", "elev", "height", "value"} : SplitList(o.contourAttr);
    bands = FinalBands(o);
  }
  static bool AutoLand(const std::string& layer) {
    std::string l = Lower(layer);
    return l.find("land") != std::string::npos || l.find("lndare") != std::string::npos || l.find("island") != std::string::npos ||
           l == "coast" || l == "coastline_poly";
  }
  bool Attr(const Layer& L, const Feat& F, const std::vector<std::string>& keys, double& v) const {
    for (auto& want : keys)
      for (size_t i = 0; i + 1 < F.tags.size(); i += 2) {
        if (F.tags[i] >= L.keys.size() || F.tags[i + 1] >= L.vals.size()) continue;
        if (Lower(L.keys[F.tags[i]]) != want) continue;
        const Val& val = L.vals[F.tags[i + 1]];
        if (!val.num) continue;
        v = o.elevation ? -val.d : val.d;
        return true;
      }
    return false;
  }
  bool ContourValue(const Layer& L, const Feat& F, double& v) const {
    if (F.type != 2 || (!contour.empty() && !InList(contour, L.name))) return false;
    return Attr(L, F, contourKeys, v);
  }
  // returns a role (kRoleLand, kRoleBand + band index, kRoleContourMinor/Major) or -1; for contours fills name
  int Classify(const Layer& L, const Feat& F, std::string& name) const {
    if (F.type == 3) {
      if ((!land.empty() && InList(land, L.name)) || (land.empty() && AutoLand(L.name))) return kRoleLand;
      if (!depth.empty() && !InList(depth, L.name)) return -1;
      double d;
      if (!Attr(L, F, depthKeys, d)) return -1;
      int band = 0;
      for (size_t i = 0; i < bands.size(); ++i) if (d >= bands[i].first) band = int(i);
      return kRoleBand + band;
    }
    if (F.type == 2) {
      if (!contour.empty() && !InList(contour, L.name)) return -1;
      double d;
      if (!Attr(L, F, contourKeys, d)) return -1;
      char buf[32];
      if (std::fabs(d - std::round(d)) < 1e-6) snprintf(buf, sizeof buf, "%lld m", (long long)std::llround(d));
      else snprintf(buf, sizeof buf, "%.1f m", d);
      name = buf;
      const double every = o.majorContourEvery, q = every > 0 ? d / every : 0.5;
      const bool major = every > 0 && std::fabs(q - std::round(q)) < 1e-6;
      if (o.safetyDepth > 0 && std::fabs(d - o.safetyDepth) < 1e-6) return kRoleContourSafety;
      if (!major && o.fineContourZoom < 0) return -1;     // finer contours switched off
      return major ? kRoleContourMajor : kRoleContourMinor;
    }
    return -1;
  }
};

// ------------------------------------------------------------------ block building
// Two chart layouts:
//  Large  : 480 km blocks, 20x20 root cells of 24 km, each holding a SubBlk node of 4x4 x 6 km cells; 1 m units.
//  Medium : 4800 km blocks, 20x20 flat cells of 240 km; 80 m units.
struct Layout {
  int64_t block, rootCell, child;   // metres (child == 0 -> flat)
  int scale;                        // metres per coordinate unit
  uint8_t b2; uint32_t subs, v80;
  int32_t sd[4];
  std::vector<std::pair<uint16_t, uint8_t>> sdl;
  bool overview;
  bool Nested() const { return child > 0; }
  int64_t Leaf() const { return Nested() ? child : rootCell; }
  int Per() const { return Nested() ? int(rootCell / child) : 1; }
};
const Layout kLargeLayout{480000, 24000, 6000, 1, 3, 23, 23, {22851, 22851, 0, 0}, {{100, 1}, {200, 1}}, false};
const Layout kMediumLayout{4800000, 240000, 0, 80, 2, 20, 20, {457022, 457022, 22851, 22851}, {{100, 1}}, true};

struct CellData { std::map<int, std::vector<uint8_t>> byType; };  // type index -> encoded features
struct BlockBuild {
  int64_t bx, by;                                   // block index
  std::map<int, std::map<int, CellData>> cells;     // root cell -> child cell (0 when flat) -> data
  int64_t ext[4] = {INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN};   // metres
};

void AddFeature(const Layout& Lo, BlockBuild& B, int type, const std::string& name, const std::vector<Ring>& rings, bool poly,
                double tx0, double ty0, double tx1, double ty1) {
  // rings in Lowrance metres (double). Clip to the tile rect and to each leaf cell of this block.
  const double bx0 = double(B.bx * Lo.block), by0 = double(B.by * Lo.block);
  const double leaf = double(Lo.Leaf());
  const int64_t nLeaf = Lo.block / Lo.Leaf();
  double fx0 = 1e300, fy0 = 1e300, fx1 = -1e300, fy1 = -1e300;
  for (auto& r : rings) for (auto& p : r) { fx0 = std::min(fx0, p.first); fx1 = std::max(fx1, p.first); fy0 = std::min(fy0, p.second); fy1 = std::max(fy1, p.second); }
  fx0 = std::max({fx0, tx0, bx0}); fy0 = std::max({fy0, ty0, by0});
  fx1 = std::min({fx1, tx1, bx0 + double(Lo.block)}); fy1 = std::min({fy1, ty1, by0 + double(Lo.block)});
  if (poly ? (fx0 >= fx1 || fy0 >= fy1) : (fx0 > fx1 || fy0 > fy1)) return;  // lines may be axis-aligned
  int64_t cx0 = int64_t(std::floor((fx0 - bx0) / leaf)), cx1 = std::max(cx0, int64_t(std::floor((fx1 - bx0 - 1e-9) / leaf)));
  int64_t cy0 = int64_t(std::floor((fy0 - by0) / leaf)), cy1 = std::max(cy0, int64_t(std::floor((fy1 - by0 - 1e-9) / leaf)));
  cx0 = std::max<int64_t>(cx0, 0); cy0 = std::max<int64_t>(cy0, 0); cx1 = std::min<int64_t>(cx1, nLeaf - 1); cy1 = std::min<int64_t>(cy1, nLeaf - 1);
  const double s = double(Lo.scale);
  for (int64_t cy = cy0; cy <= cy1; ++cy)
    for (int64_t cx = cx0; cx <= cx1; ++cx) {
      double x0 = std::max(bx0 + cx * leaf, tx0), y0 = std::max(by0 + cy * leaf, ty0);
      double x1 = std::min(bx0 + (cx + 1) * leaf, tx1), y1 = std::min(by0 + (cy + 1) * leaf, ty1);
      if (x0 >= x1 || y0 >= y1) continue;
      std::vector<std::vector<Pt>> parts;
      auto toUnits = [&](Ring r) { for (auto& p : r) { p.first /= s; p.second /= s; } return r; };
      if (poly) {
        for (size_t i = 0; i < rings.size(); ++i) {
          auto loops = QuantizeArea(toUnits(ClipPoly(rings[i], x0, y0, x1, y1)));
          if (loops.empty()) { if (i == 0) break; continue; }
          for (auto& q : loops) parts.push_back(std::move(q));
        }
        DropCrossingRings(parts);
      } else {
        for (auto& r : rings) for (auto& seg : ClipLine(r, x0, y0, x1, y1)) { auto q = Quantize(toUnits(seg), false); if (!q.empty()) parts.push_back(std::move(q)); }
      }
      if (parts.empty()) continue;
      const int per = Lo.Per();
      int root = int((cy / per) * 20 + (cx / per)), child = Lo.Nested() ? int((cy % per) * per + (cx % per)) : 0;
      auto& buf = B.cells[root][child].byType[type];
      if (poly) {
        PutVar(buf, uint32_t(parts.size()));
        for (auto& p : parts) PutRing(buf, p);
      } else {
        for (auto& p : parts) { PutStr(buf, name); PutRing(buf, p); }
      }
      for (auto& p : parts)
        for (auto& q : p) {
          B.ext[0] = std::min(B.ext[0], q.x * Lo.scale); B.ext[1] = std::min(B.ext[1], q.y * Lo.scale);
          B.ext[2] = std::max(B.ext[2], q.x * Lo.scale); B.ext[3] = std::max(B.ext[3], q.y * Lo.scale);
        }
    }
}

std::vector<uint8_t> EncodeNode(int64_t ox, int64_t oy, int64_t cell, int dim, const std::vector<std::vector<uint8_t>>& cellBytes,
                                const std::vector<uint32_t>& tableLens) {
  std::vector<uint8_t> h;
  Put32(h, uint32_t(int32_t(ox))); Put32(h, uint32_t(int32_t(oy))); Put32(h, uint32_t(cell)); Put32(h, uint32_t(cell));
  Put16(h, uint16_t(dim)); Put16(h, uint16_t(dim)); PutVar(h, uint32_t(dim * dim));
  uint32_t off = 0;
  for (size_t i = 0; i < cellBytes.size(); ++i) {
    if (cellBytes[i].empty()) { PutVar(h, 0); PutVar(h, 0); continue; }
    PutVar(h, off); PutVar(h, tableLens[i]);
    off += uint32_t(cellBytes[i].size());
  }
  return h;
}

// cell payload: triple table (type, offset, length) in ascending type order + concatenated data
std::vector<uint8_t> EncodeLeaf(const CellData& cd, uint32_t* tableLen) {
  std::vector<uint8_t> tt, data;
  PutVar(tt, uint32_t(cd.byType.size()));
  uint32_t off = 0;
  for (auto& t : cd.byType) {
    PutVar(tt, uint32_t(t.first)); PutVar(tt, off); PutVar(tt, uint32_t(t.second.size()));
    off += uint32_t(t.second.size());
    data.insert(data.end(), t.second.begin(), t.second.end());
  }
  *tableLen = uint32_t(tt.size());
  tt.insert(tt.end(), data.begin(), data.end());
  return tt;
}

std::vector<uint8_t> EncodeBlock(const Layout& Lo, const BlockBuild& B) {
  const int64_t bx0 = B.bx * Lo.block, by0 = B.by * Lo.block;
  std::vector<std::vector<uint8_t>> rootCells(400);
  std::vector<uint32_t> rootTable(400, 0);
  for (auto& rc : B.cells) {
    int root = rc.first;
    if (!Lo.Nested()) { rootCells[size_t(root)] = EncodeLeaf(rc.second.at(0), &rootTable[size_t(root)]); continue; }
    const int per = Lo.Per();
    const int64_t rx0 = bx0 + (root % 20) * Lo.rootCell, ry0 = by0 + (root / 20) * Lo.rootCell;
    std::vector<std::vector<uint8_t>> childCells(size_t(per * per));
    std::vector<uint32_t> childTable(size_t(per * per), 0);
    for (auto& cc : rc.second) childCells[size_t(cc.first)] = EncodeLeaf(cc.second, &childTable[size_t(cc.first)]);
    auto chead = EncodeNode(rx0, ry0, Lo.child, per, childCells, childTable);
    std::vector<uint8_t> cell;
    PutVar(cell, 1); PutVar(cell, kSubBlk); PutVar(cell, 0); PutVar(cell, uint32_t(chead.size()));
    rootTable[size_t(root)] = uint32_t(cell.size());
    cell.insert(cell.end(), chead.begin(), chead.end());
    for (auto& c : childCells) cell.insert(cell.end(), c.begin(), c.end());
    rootCells[size_t(root)] = std::move(cell);
  }
  auto rhead = EncodeNode(bx0, by0, Lo.rootCell, 20, rootCells, rootTable);
  std::vector<uint8_t> b;
  b.push_back(1); b.push_back(0); b.push_back(Lo.b2); b.push_back(0);
  Put32(b, uint32_t(int32_t(bx0))); Put32(b, uint32_t(int32_t(by0))); Put32(b, uint32_t(int32_t(bx0 + Lo.block))); Put32(b, uint32_t(int32_t(by0 + Lo.block)));
  Put16(b, uint16_t(Lo.scale)); PutStr(b, "");
  PutVar(b, 1); Put32(b, 0); Put32(b, Lo.subs);
  PutVar(b, Lo.v80); PutVar(b, uint32_t(rhead.size())); PutVar(b, 0);
  for (int i = 0; i < 4; ++i) Put32(b, uint32_t(Lo.sd[i]));
  PutVar(b, uint32_t(Lo.sdl.size()));
  for (auto& p : Lo.sdl) { Put16(b, p.first); b.push_back(p.second); }
  b.insert(b.end(), rhead.begin(), rhead.end());
  for (auto& c : rootCells) b.insert(b.end(), c.begin(), c.end());
  return b;
}

// ------------------------------------------------------------------ generated style library (true RGBA fills)
// Atlas v12 supports RGBA colours (its *CLRRGB attributes, "RRGGBBAA" parsed as 0xRRGGBBAA). A fill record
// carries flags 0x11 = palette index (fallback for units without RGBA) + RGBA. Entries are built from the
// default styles in styles.hpp.
struct LibGroup { std::vector<uint8_t> name, data; };
LibGroup Group(const at5::Style& s) { return {s.key, s.data}; }
size_t FindBytes(const std::vector<uint8_t>& v, const std::vector<uint8_t>& pat) {
  auto it = std::search(v.begin(), v.end(), pat.begin(), pat.end());
  if (it == v.end()) throw std::runtime_error("style template record not found");
  return size_t(it - v.begin());
}
uint32_t Rgba(uint32_t rgb) { return (rgb << 8) | 0xFF; }
std::vector<uint8_t> AreaEntry(const std::vector<uint8_t>& tpl, uint8_t index, uint32_t rgb) {
  size_t i = FindBytes(tpl, {0, 0, 0, 0, 1, 0, 0, 0});  // fill: u32 0, u32 flags=1, u8 index
  std::vector<uint8_t> e(tpl.begin(), tpl.begin() + long(i));
  Put32(e, 0); Put32(e, 0x11); e.push_back(index); Put32(e, Rgba(rgb));
  e.insert(e.end(), tpl.begin() + long(i + 9), tpl.end());
  return e;
}
std::vector<uint8_t> ContourEntry(const std::vector<uint8_t>& tpl, uint32_t labelIdx, uint32_t rgb, int width = 0) {
  std::vector<uint8_t> t(tpl.begin(), tpl.end() - 1);  // last byte = varint label-point index (l800 list of 1)
  PutVar(t, labelIdx);
  size_t i = FindBytes(t, {0, 0, 0, 0, 0x93, 0, 0, 0});  // pen: flags 0x93 -> 0xB3 (+ colour after b0,w2,b10)
  std::vector<uint8_t> e(t.begin(), t.begin() + long(i));
  Put32(e, 0); Put32(e, 0x93 | 0x20);
  e.insert(e.end(), t.begin() + long(i + 8), t.begin() + long(i + 12));   // b0, w2 (line width), b10
  if (width > 0) { e[e.size() - 3] = uint8_t(width); e[e.size() - 2] = uint8_t(width >> 8); }
  Put32(e, Rgba(rgb));
  e.insert(e.end(), t.begin() + long(i + 12), t.end());
  return e;
}
// Set the zoom from which a style is drawn (entry zmin and style zmin, both i16 = zoom x 256).
std::vector<uint8_t> WithMinZoom(std::vector<uint8_t> d, int16_t from, int to) {
  if (to < 1 || to > 32767 || to == from) return d;
  int found = 0;
  for (size_t i = 0; i + 1 < d.size(); ++i)
    if (int16_t(d[i] | d[i + 1] << 8) == from) { d[i] = uint8_t(to); d[i + 1] = uint8_t(to >> 8); ++found; ++i; }
  if (found != 2) throw std::runtime_error("contour style zoom fields not found");
  return d;
}
std::pair<std::vector<uint8_t>, uint32_t> AssembleLib(const std::vector<LibGroup>& entries) {
  std::vector<uint8_t> hdr, body;
  Put32(hdr, uint32_t(9 + entries[0].name.size())); hdr.push_back(1); Put32(hdr, uint32_t(entries.size()));
  for (auto& e : entries) {
    if (e.name.size() != entries[0].name.size()) throw std::runtime_error("style group names must share a length");
    hdr.push_back(uint8_t(e.name.size())); hdr.insert(hdr.end(), e.name.begin(), e.name.end());
    Put32(hdr, 1); Put32(hdr, uint32_t(body.size())); Put32(hdr, uint32_t(e.data.size()));
    body.insert(body.end(), e.data.begin(), e.data.end());
  }
  uint32_t hl = uint32_t(hdr.size());
  hdr.insert(hdr.end(), body.begin(), body.end());
  return {hdr, hl};
}
uint8_t FallbackIndex(size_t i, size_t n) {  // palette indices ordered shallow -> deep
  static const uint8_t idx[5] = {10, 17, 16, 9, 15};
  size_t k = n > 1 ? size_t(std::lround(double(i) * 4.0 / double(n - 1))) : 0;
  return idx[std::min<size_t>(k, 4)];
}
std::vector<LibGroup> BandEntries(const LibGroup& area, const std::vector<std::pair<double, uint32_t>>& bands) {
  std::vector<LibGroup> out;
  for (size_t i = 0; i < bands.size(); ++i) {
    LibGroup g{area.name, AreaEntry(area.data, FallbackIndex(i, bands.size()), bands[i].second)};
    g.name[5] = uint8_t(i < 10 ? '0' + i : 'A' + (i - 10));  // unique group id per band ('a7!!!N...')
    out.push_back(std::move(g));
  }
  return out;
}
std::pair<std::vector<uint8_t>, uint32_t> BuildLargeLib(const std::vector<std::pair<double, uint32_t>>& bands, uint32_t land, uint32_t contour, uint32_t minorContour, int fineZoom,
                                                        uint32_t safetyColour, bool safety) {
  const auto S = at5::DetailChartStyles();
  const LibGroup land0 = Group(S.land), area = Group(S.depthArea), fine = Group(S.contourFine), all = Group(S.contourAll);
  const LibGroup fineLbl = Group(S.labelFine), allLbl = Group(S.labelAll);
  std::vector<LibGroup> e{{land0.name, AreaEntry(land0.data, 20, land)}, Group(S.subBlock)};
  for (auto& b : BandEntries(area, bands)) e.push_back(b);
  const uint32_t n = uint32_t(bands.size());
  e.push_back({fine.name, WithMinZoom(ContourEntry(fine.data, 4 + n, minorContour), 3808, fineZoom)});
  e.push_back({all.name, ContourEntry(all.data, 5 + n, contour)});
  e.push_back({fineLbl.name, WithMinZoom(fineLbl.data, 3808, fineZoom)}); e.push_back(allLbl);
  // safety contour: the all-zoom contour style, bold (width 3) in its own colour, with its own label points
  if (safety) {
    LibGroup safe{all.name, ContourEntry(all.data, 7 + n, safetyColour, 3)}, safeLbl = allLbl;
    safe.name[1] = 'B'; safeLbl.name[1] = 'B';
    e.push_back(safe); e.push_back(safeLbl);
  }
  return AssembleLib(e);
}
std::pair<std::vector<uint8_t>, uint32_t> BuildMediumLib(const std::vector<std::pair<double, uint32_t>>& bands, uint32_t land) {
  const auto S = at5::OverviewChartStyles();
  const LibGroup land0 = Group(S.land), area = Group(S.depthArea);
  std::vector<LibGroup> e{{land0.name, AreaEntry(land0.data, 20, land)}};
  for (auto& b : BandEntries(area, bands)) e.push_back(b);
  return AssembleLib(e);
}

}  // namespace

// Colour schemes. "s52" = IHO S-52 ECDIS day colours (OpenCPN chartsymbols.xml DAY_BRIGHT):
// DEPIT intertidal green, DEPVS / DEPMS / DEPMD blues, DEPDW deep (near white); LANDA land; DEPCN contour grey.
std::vector<std::pair<double, uint32_t>> DefaultBands() { return ParseBands("s52"); }

// Navionics / C-MAP style automatic shading: drying green, then a blue gradient from dark (shallow) to pale,
// white from the deep-water depth on.
std::vector<std::pair<double, uint32_t>> GradientBands(double deep, const std::vector<double>& levels, int ramp) {
  static const double steps[] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 40, 50, 60, 80, 100, 150, 200, 300, 500};
  std::vector<double> s;
  // shade contour by contour: use the depths that have contour lines in the data, so every colour edge is a line
  for (double v : steps) if (v < deep && std::find(levels.begin(), levels.end(), v) != levels.end()) s.push_back(v);
  if (s.size() < 3) {                                  // unusual levels (e.g. feet): use the data's own levels
    s.clear();
    for (double v : levels) if (v >= 0 && v < deep) s.push_back(v);
    std::sort(s.begin(), s.end());
    while (s.size() > 25) { std::vector<double> t; for (size_t i = 0; i < s.size(); i += 2) t.push_back(s[i]); s.swap(t); }
  }
  if (s.empty()) for (double v : steps) if (v < deep) s.push_back(v);
  if (s.empty() || s.front() > 0) s.insert(s.begin(), 0.0);   // water starts at 0 m even without a 0 m contour
  // ramp from the shallowest shade to the last shade before deep (white)
  //  0 soft blue; 1 Navionics chart palette, measured from Navionics / Garmin Navionics Vision screenshots
  static const uint32_t soft[] = {0x4084D6, 0xDEEEFA};
  static const uint32_t navionics[] = {0x20B4FC, 0x4CC2FC, 0x6ACCFC, 0x8AD2FC, 0xA2DAFC, 0xB6E2F8, 0xC8EAF8};
  const uint32_t* rp = ramp == 1 ? navionics : soft;
  const size_t rn = ramp == 1 ? std::size(navionics) : std::size(soft);
  std::vector<std::pair<double, uint32_t>> b{{-1e9, ramp == 1 ? 0x9CCD00u : 0x96C878u}};   // drying
  for (size_t i = 0; i < s.size(); ++i) {
    // Navionics: equal steps down the palette, contour by contour; soft blue: by depth (more contrast shallow)
    const double t = s.size() <= 1 ? 0 : ramp == 1 ? double(i) / double(s.size() - 1) : std::sqrt(std::max(0.0, s[i]) / s.back());
    const double f = t * double(rn - 1);
    const size_t a = std::min(rn - 2, size_t(f));
    const double u = f - double(a);
    uint32_t c = 0;
    for (int k = 2; k >= 0; --k) {
      const double x = double((rp[a] >> (8 * k)) & 255), y = double((rp[a + 1] >> (8 * k)) & 255);
      c = c << 8 | uint32_t(std::lround(x + (y - x) * u));
    }
    b.push_back({s[i], c});
  }
  b.push_back({deep, 0xFFFFFF});
  return b;
}

// The final colour bands: scheme, then the safety depth (shallow water in the warning colour) and the
// highlighted depth range, each splitting bands where needed.
std::vector<std::pair<double, uint32_t>> FinalBands(const Options& o) {
  auto b = o.gradient ? GradientBands(o.deepDepth > 0 ? o.deepDepth : 20, o.dataLevels, o.gradientRamp) : (o.bands.empty() ? DefaultBands() : o.bands);
  std::sort(b.begin(), b.end());
  auto split = [&](double d) {
    if (d <= b.front().first) return;
    size_t k = 0;
    for (size_t i = 0; i < b.size(); ++i) if (b[i].first <= d) k = i;
    if (b[k].first != d) b.insert(b.begin() + long(k + 1), {d, b[k].second});
  };
  if (o.safetyDepth > 0) {
    split(0); split(o.safetyDepth);
    for (auto& x : b) if (x.first >= 0 && x.first < o.safetyDepth) x.second = o.shallowColour;
  }
  if (o.highlightTo > o.highlightFrom && o.highlightFrom >= -1e8) {
    split(o.highlightFrom); split(o.highlightTo);
    for (auto& x : b) if (x.first >= o.highlightFrom && x.first < o.highlightTo) x.second = o.highlightColour;
  }
  if (b.size() > 30) throw std::runtime_error("too many depth bands (" + std::to_string(b.size()) + "; max 30)");
  return b;
}
std::vector<std::pair<double, uint32_t>> ParseBands(const std::string& s0) {
  std::string s = Lower(s0);
  if (s == "s52" || s == "standard") s = "min:83b295,0:73b6ef,2:98c5f2,5:bad5e1,10:d4eaee";
  else if (s == "navionics-classic") s = "min:96c878,0:5096e1,2:7db9f0,5:afd7f5,10:ffffff";
  else if (s == "s52-dark" || s == "dusk") s = "min:6f977f,0:629acb,2:81a8ce,5:9eb5c0,10:b4c7ca";
  std::vector<std::pair<double, uint32_t>> b;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
    std::string item = s.substr(i, j - i);
    size_t c = item.find(':');
    if (c == std::string::npos) throw std::runtime_error("band '" + item + "' must be DEPTH:RRGGBB (e.g. 2:98C5F2)");
    std::string d = item.substr(0, c), col = item.substr(c + 1);
    if (!col.empty() && col[0] == '#') col = col.substr(1);
    if (col.size() != 6 || col.find_first_not_of("0123456789abcdef") != std::string::npos)
      throw std::runtime_error("colour '" + col + "' must be 6 hex digits RRGGBB");
    b.push_back({d == "min" || d == "-inf" ? -1e9 : std::stod(d), uint32_t(std::stoul(col, nullptr, 16))});
    i = j + 1;
  }
  if (b.empty() || b.size() > 30) throw std::runtime_error("need 1..30 depth bands");
  std::sort(b.begin(), b.end());
  return b;
}
uint32_t ParseRgb(const std::string& s0) {
  std::string s = Lower(s0); if (!s.empty() && s[0] == '#') s = s.substr(1);
  if (s.size() != 6 || s.find_first_not_of("0123456789abcdef") != std::string::npos) throw std::runtime_error("colour must be RRGGBB");
  return uint32_t(std::stoul(s, nullptr, 16));
}

// Raster MBTiles whose pixels are encoded heights (Mapbox Terrain-RGB / Terrarium DEM), not a picture.
std::string RasterDemEncoding(const std::string& path) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return "";
  sqlite3_busy_timeout(db, 60000);
  std::string found;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT name, value FROM metadata", -1, &st, nullptr) == SQLITE_OK)
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char* k = (const char*)sqlite3_column_text(st, 0); const char* v = (const char*)sqlite3_column_text(st, 1);
      if (!k || !v) continue;
      const std::string key = Lower(k), val = Lower(v);
      if (key == "encoding" && (val == "mapbox" || val == "terrarium")) found = val;
      if ((key == "name" || key == "description" || key == "type") &&
          (val.find("terrain-rgb") != std::string::npos || val.find("terrainrgb") != std::string::npos || val.find("terrarium") != std::string::npos))
        found = val.find("terrarium") != std::string::npos ? "terrarium" : "mapbox";
    }
  sqlite3_finalize(st);
  sqlite3_close(db);
  return found;
}

bool IsVectorMbtiles(const std::string& path) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return false;
  sqlite3_busy_timeout(db, 60000);
  bool vecFmt = false;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT value FROM metadata WHERE name='format'", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
    std::string f = Lower((const char*)sqlite3_column_text(st, 0));
    vecFmt = f == "pbf" || f == "mvt";
  }
  sqlite3_finalize(st);
  st = nullptr;
  if (!vecFmt && sqlite3_prepare_v2(db, "SELECT tile_data FROM tiles LIMIT 1", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
    auto p = (const uint8_t*)sqlite3_column_blob(st, 0); int n = sqlite3_column_bytes(st, 0);
    if (p && n > 2) {
      bool img = (p[0] == 0x89 && p[1] == 'P') || (p[0] == 0xFF && p[1] == 0xD8) || (n > 12 && !memcmp(p + 8, "WEBP", 4));
      vecFmt = !img && (p[0] == 0x1f || p[0] == 0x1a);
    }
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
  return vecFmt;
}

namespace {
// ------------------------------------------------------------------ depth areas built from contour lines
// Most free bathymetry tiles carry only depth contours (+ land), no depth-area polygons, so there is nothing
// to colour. Here the areas are rebuilt per tile from the contours themselves, so every colour edge sits on
// its contour line:
//  1. rasterise land and contour lines of a 3x3 tile window (lines 8-connected, so regions can't leak);
//  2. label the water regions between the lines (4-connected);
//  3. a region touching two contour levels lies between them; land counts as the shoreline; a region
//     touching a single level (a hole or a shoal ring) takes the opposite side of that level from its
//     neighbour across the line; regions still unknown use a wider 9x9 window, then default to deep;
//  4. the centre tile's band raster is traced along pixel edges (exteriors clockwise, holes anticlockwise,
//     IMC's non-zero convention), corners cut at edge midpoints so neighbouring bands share one boundary.
struct TileGeo {
  std::vector<std::pair<double, Ring>> lines;   // (depth, polyline) in Lowrance metres
  std::vector<std::vector<Ring>> land;          // polygons (rings) in Lowrance metres
  std::vector<std::array<double, 4>> cover;     // where the data is (x0, y0, x1, y1); nothing is coloured outside
};

// The existing tiles of the first finer zoom (up to 5 levels down) inside tile (z, x, y).
std::vector<std::tuple<int, int64_t, int64_t>> FinerTiles(const Source& src, int z, int64_t x, int64_t y) {
  std::vector<std::tuple<int, int64_t, int64_t>> out;
  for (int dz = 1; dz <= 5 && z + dz <= src.maxZoom && out.empty(); ++dz) {
    const int zf = z + dz;
    if (zf < src.minZoom || src.rows[size_t(zf)].empty()) continue;
    const int64_t n = int64_t(1) << dz;
    for (int64_t cy = y * n; cy < (y + 1) * n; ++cy)
      for (int64_t cx = x * n; cx < (x + 1) * n; ++cx)
        if (src.Has(zf, cx, cy)) out.push_back({zf, cx, cy});
  }
  return out;
}

class ContourBands {
 public:
  ContourBands(const Source& src, TileReader& rd, const Classifier& cls, int z)
      : src_(src), rd_(rd), cls_(cls), z_(z), tileL_(2 * kPi * kRl / double(int64_t(1) << z)) {}

  // Adds the depth-area polygons of tile (tx, ty) to B. Returns the number of features added.
  long long Make(int64_t tx, int64_t ty, const Layout& Lo, BlockBuild& B) {
    const int R = 384;                                    // ~6 m pixels at zoom 14, ~13 m at zoom 13
    Grid& g = fine_;                                      // reused per worker: no per-tile allocation
    Build(tx, ty, 1, R, g, R / 2);
    Solve(g);
    Nearest2(g);
    Vote(g);
    // per-pixel depth of the centre tile: between the two nearest contour levels (distances measured through
    // water, never across another line), or the region's level when only one level bounds it
    std::vector<double> dep(size_t(R) * R, std::nan(""));
    const double shore = Shore(g);
    bool need = false;
    for (int r = 0; r < R; ++r)
      for (int c = 0; c < R; ++c) {
        const size_t i = size_t(r + g.off) * g.N + (c + g.off), o = size_t(r) * R + c;
        if (g.kind[i] == kLand || g.kind[i] == kVoid) continue;
        if (g.kind[i] == kLine) { dep[o] = g.levels[size_t(g.lev[i])]; continue; }
        if (g.lab2[i] >= 0) {
          dep[o] = Interp(g, g.lab1[i], g.d1[i], g.lab2[i], g.d2[i], shore);
        } else {
          dep[o] = g.depth[size_t(g.lab[i])];
          need |= std::isnan(dep[o]);
        }
      }
    // No coastline in the data near here: water shallower than the shallowest contour that runs off towards
    // the unknown shore is left clear (only enclosed patches, e.g. reef tops, are coloured)
    {
      bool land = false;
      for (uint8_t k : g.kind) if (k == kLand) { land = true; break; }
      double minLev = 1e300;
      for (double v : cls_.o.dataLevels) minLev = std::min(minLev, v);
      if (!land && !gLand && minLev < 1e299) {
        // candidates: single-line water that is unresolved, or shallower than every contour and not enclosed here
        std::vector<std::pair<size_t, size_t>> cand;        // (window pixel, centre pixel)
        for (int r = 0; r < R; ++r)
          for (int c = 0; c < R; ++c) {
            const size_t i = size_t(r + g.off) * g.N + (c + g.off), o = size_t(r) * R + c;
            if (g.kind[i] != kWater || g.lab[i] < 0 || g.lab2[i] >= 0) continue;
            const double d = dep[o];
            if (std::isnan(d) || (g.open[size_t(g.lab[i])] && d < minLev)) cand.push_back({i, o});
          }
        if (!cand.empty()) {                              // decide with a ~9-tile wide view (reefs vs shore)
          Grid& c = coarse_;
          Build(tx, ty, 4, 128, c);
          Solve(c);
          for (auto [i, o] : cand) {
            const int col = int(i % size_t(g.N)), row = int(i / size_t(g.N));
            const double X = g.x0 + (col + 0.5) * g.px, Y = g.y1 - (row + 0.5) * g.px;
            const int cc = int((X - c.x0) / c.px), cr = int((c.y1 - Y) / c.px);
            int32_t cl = -1;
            if (cc >= 0 && cr >= 0 && cc < c.N && cr < c.N) cl = c.lab[size_t(cr) * c.N + cc];
            double d = dep[o];
            if (std::isnan(d) && cl >= 0) d = c.depth[size_t(cl)];
            // the only line near it, if any: beyond the shallowest contour (or no line at all) and open = unknown shore
            const int32_t l1 = g.lab1[i];
            const double nearLev = l1 < 0 || l1 == kShoreLab ? -1e300 : g.levels[size_t(l1 >> 15)];
            bool shoreSide = nearLev <= minLev;
            if (l1 < 0) {                                  // no line near: ask the wider view what the water touches
              shoreSide = cl >= 0 && !c.touched[size_t(cl)].empty();   // no line anywhere near: open sea, not shore
              if (cl >= 0) for (int16_t k : c.touched[size_t(cl)]) if (c.levels[size_t(k)] > minLev) shoreSide = false;
            }
            const bool open = cl < 0 || c.open[size_t(cl)];
            if (open && ((std::isnan(d) && shoreSide) || (!std::isnan(d) && d < minLev))) d = std::nan("");
            else if (std::isnan(d)) d = 1e4;                // open water beyond the contours: deep
            dep[o] = d;
          }
        }
      }
      if (!land && !gLand) need = false;               // no coastline anywhere: leave unknown water clear
    }
    if (need) {                                           // wider context at lower resolution
      Grid& c = coarse_;
      Build(tx, ty, 4, 128, c);
      Solve(c);
      std::vector<uint8_t> seeded(g.depth.size(), 0);
      for (int r = 0; r < R; ++r)
        for (int col = 0; col < R; ++col) {
          const size_t i = size_t(r + g.off) * g.N + (col + g.off);
          const int32_t l = g.lab[i];
          if (l < 0 || !std::isnan(g.depth[size_t(l)]) || seeded[size_t(l)]) continue;
          const double X = g.x0 + (col + g.off + 0.5) * g.px, Y = g.y1 - (r + g.off + 0.5) * g.px;
          const int cc = int((X - c.x0) / c.px), cr = int((c.y1 - Y) / c.px);
          if (cc < 0 || cr < 0 || cc >= c.N || cr >= c.N) continue;
          const int32_t cl = c.lab[size_t(cr) * c.N + cc];
          if (cl >= 0 && !std::isnan(c.depth[size_t(cl)])) { g.depth[size_t(l)] = c.depth[size_t(cl)]; seeded[size_t(l)] = 1; }
        }
      Propagate(g);
      for (int r = 0; r < R; ++r)
        for (int col = 0; col < R; ++col) {
          double& d = dep[size_t(r) * R + col];
          if (!std::isnan(d)) continue;
          const size_t i = size_t(r + g.off) * g.N + (col + g.off);
          if (g.kind[i] != kWater) continue;
          d = g.depth[size_t(g.lab[i])];
          if (std::isnan(d)) d = 1e4;                     // open water, no contour anywhere near: deep
        }
    }
    std::vector<int> band(size_t(R) * R, -1);
    for (size_t o = 0; o < dep.size(); ++o) {
      if (std::isnan(dep[o])) continue;
      int b = 0;
      for (size_t k = 0; k < cls_.bands.size(); ++k) if (dep[o] >= cls_.bands[k].first) b = int(k);
      band[o] = b;
    }
    // trace each band
    const double cx0 = -kPi * kRl + double(tx) * tileL_, cy1 = kPi * kRl - double(ty) * tileL_;
    const double px = tileL_ / R;
    std::set<int> present(band.begin(), band.end());
    std::vector<uint8_t> ckind(size_t(R) * R);
    for (int r = 0; r < R; ++r) for (int c = 0; c < R; ++c) ckind[size_t(r) * R + c] = g.kind[size_t(r + g.off) * g.N + (c + g.off)];
    Snapper sn;
    BuildSnapper(tx, ty, R, sn);
    long long n = 0;
    for (int b : present) {
      if (b < 0) continue;
      const int type = TypeForRole(kRoleBand + b, int(cls_.bands.size()), Lo.overview);
      if (type < 0) continue;
      for (auto& pg : Trace(band, ckind, R, b, cx0, cy1, px, sn, trOut1_, trOut2_)) { AddFeature(Lo, B, type, "", pg, true, cx0, cy1 - tileL_, cx0 + tileL_, cy1); ++n; }
    }
    if (cache_.size() > 400 || CachePoints() > 6000000) cache_.clear();   // bound memory per worker (dense 1 m data)
    return n;
  }

 private:
  enum : uint8_t { kWater = 0, kLand = 1, kLine = 2, kVoid = 3 };
  static constexpr int32_t kShoreLab = 0x7FFFFFFE;   // label of the shoreline; others are (lo << 15 | hi) level ranges
  struct Grid {
    int N = 0; double x0 = 0, y1 = 0, px = 0;
    std::vector<uint8_t> kind; std::vector<int16_t> lev; std::vector<int32_t> lab;
    std::vector<double> levels;                         // contour level values (lev indexes this)
    std::vector<double> depth;                          // per region, NaN = unknown
    std::vector<std::tuple<int32_t, int32_t, int16_t>> across;  // regions adjacent across level k
    std::vector<std::vector<int16_t>> touched; std::vector<uint8_t> landAdj, leaky;
    std::vector<uint8_t> open;                 // region reaches no-data or the window edge (not enclosed)
    bool hasLand = false;
    int off = 0;                               // pixel offset of the centre tile
    std::vector<int32_t> lab1, lab2;           // two nearest lines per pixel (Nearest2), as level ranges
    std::vector<uint16_t> d1, d2;              // and their distances (pixels)
    std::vector<int16_t> levLo, levHi;         // shallowest / deepest level drawn through a line pixel (stacked lines at a drop-off)
  };

  // Contour lines (at the band-start depths) and land outlines of the centre tile, in its pixel coordinates
  // (x right, y down), bucketed so a traced edge point can be moved onto the real line next to it.
  struct Snapper {
    static constexpr int kLandKey = -1;
    struct Seg { double ax, ay, bx, by; int key; };
    int R = 0, nb = 0; double cell = 4;
    std::vector<std::vector<Seg>> buckets;
    void Init(int r) { R = r; nb = int(R / cell) + 3; buckets.assign(size_t(nb) * nb, {}); }
    void Add(double ax, double ay, double bx, double by, int key) {
      if (std::max(ax, bx) < -3 || std::max(ay, by) < -3 || std::min(ax, bx) > R + 3 || std::min(ay, by) > R + 3) return;
      const int c0 = std::clamp(int(std::floor(std::min(ax, bx) / cell)) + 1, 0, nb - 1), c1 = std::clamp(int(std::floor(std::max(ax, bx) / cell)) + 1, 0, nb - 1);
      const int r0 = std::clamp(int(std::floor(std::min(ay, by) / cell)) + 1, 0, nb - 1), r1 = std::clamp(int(std::floor(std::max(ay, by) / cell)) + 1, 0, nb - 1);
      if (size_t(c1 - c0 + 1) * size_t(r1 - r0 + 1) > 4096) return;   // degenerate huge segment
      for (int r = r0; r <= r1; ++r) for (int c = c0; c <= c1; ++c) buckets[size_t(r) * nb + c].push_back({ax, ay, bx, by, key});
    }
    template <class F> void Near(double x, double y, F&& f) const {
      const int c = std::clamp(int(std::floor(x / cell)) + 1, 0, nb - 1), r = std::clamp(int(std::floor(y / cell)) + 1, 0, nb - 1);
      for (int rr = std::max(0, r - 1); rr <= std::min(nb - 1, r + 1); ++rr)
        for (int cc = std::max(0, c - 1); cc <= std::min(nb - 1, c + 1); ++cc)
          for (const Seg& s : buckets[size_t(rr) * nb + cc]) f(s);
    }
    // nearest point on a line with this key, within maxd pixels
    bool Project(double& x, double& y, int key, double maxd) const {
      double best = maxd * maxd, bx = 0, by = 0; bool ok = false;
      Near(x, y, [&](const Seg& s) {
        if (s.key != key) return;
        const double dx = s.bx - s.ax, dy = s.by - s.ay, l2 = dx * dx + dy * dy;
        const double t = l2 > 0 ? std::clamp(((x - s.ax) * dx + (y - s.ay) * dy) / l2, 0.0, 1.0) : 0.0;
        const double px = s.ax + t * dx, py = s.ay + t * dy, d2 = (px - x) * (px - x) + (py - y) * (py - y);
        if (d2 < best) { best = d2; bx = px; by = py; ok = true; }
      });
      if (ok) { x = bx; y = by; }
      return ok;
    }
    // where a line with this key crosses the tile edge x = X (vertical) or y = Y, closest to (x, y), within maxd
    bool OnEdge(double& x, double& y, bool vertical, int key, double maxd) const {
      double best = maxd; double nx = x, ny = y; bool ok = false;
      Near(x, y, [&](const Seg& s) {
        if (s.key != key) return;
        const double a = vertical ? s.ax : s.ay, b = vertical ? s.bx : s.by, v = vertical ? x : y;
        if ((a - v) * (b - v) > 0 || a == b) return;
        const double t = (v - a) / (b - a);
        const double o = vertical ? s.ay + t * (s.by - s.ay) : s.ax + t * (s.bx - s.ax);
        const double d = std::fabs(o - (vertical ? y : x));
        if (d < best) { best = d; ok = true; if (vertical) ny = o; else nx = o; }
      });
      if (ok) { x = nx; y = ny; }
      return ok;
    }
  };

  // Snapper for the centre tile: contour lines whose depth is a band start, and land outlines.
  void BuildSnapper(int64_t tx, int64_t ty, int R, Snapper& sn) {
    sn.Init(R);
    const double x0 = -kPi * kRl + double(tx) * tileL_, y1 = kPi * kRl - double(ty) * tileL_, px = tileL_ / R;
    std::set<const TileGeo*> seen;
    for (int64_t y = ty - 1; y <= ty + 1; ++y)
      for (int64_t x = tx - 1; x <= tx + 1; ++x) {
        const TileGeo* t = Get(x, y);
        if (!t || !seen.insert(t).second) continue;
        for (auto& ln : t->lines) {
          int key = -2;
          for (size_t k = 1; k < cls_.bands.size(); ++k) if (cls_.bands[k].first == ln.first) key = int(k);
          if (key < 0) continue;
          for (size_t i = 0; i + 1 < ln.second.size(); ++i)
            sn.Add((ln.second[i].first - x0) / px, (y1 - ln.second[i].second) / px, (ln.second[i + 1].first - x0) / px, (y1 - ln.second[i + 1].second) / px, key);
        }
        for (auto& pg : t->land)
          for (auto& r : pg)
            for (size_t i = 0; i + 1 < r.size(); ++i)
              sn.Add((r[i].first - x0) / px, (y1 - r[i].second) / px, (r[i + 1].first - x0) / px, (y1 - r[i + 1].second) / px, Snapper::kLandKey);
      }
  }

  const TileGeo* Get(int64_t tx, int64_t ty) {
    const int64_t nmax = (int64_t(1) << z_) - 1;
    if (tx < 0 || ty < 0 || tx > nmax || ty > nmax) return nullptr;
    const int d = src_.Fallback(z_, tx, ty);
    if (d == -1) {                                       // an empty tile: no contour crosses it
      // without a coastline it is left clear (could be an island); with one, near the data, it is water with no
      // lines (coloured from the shore and the contours around it) plus whatever land the coastline puts there
      if (!gLand || !src_.NearData(z_, tx, ty, 2)) return nullptr;
      const uint64_t key = uint64_t(62) << 58 | uint64_t(tx) << 29 | uint64_t(ty);
      auto it = cache_.find(key);
      if (it != cache_.end()) return &it->second;
      TileGeo& t = cache_[key];
      const double x0 = -kPi * kRl + double(tx) * tileL_, y1 = kPi * kRl - double(ty) * tileL_;
      t.cover.push_back({x0, y1 - tileL_, x0 + tileL_, y1});
      for (auto& pg : LandIn(x0 - tileL_ * 0.02, y1 - tileL_ * 1.02, x0 + tileL_ * 1.02, y1 + tileL_ * 0.02)) t.land.push_back(std::move(pg));
      return &t;
    }
    if (d < 0) {
      // grid zoom coarser than the data (overview): merge the finer tiles, thinned to half a raster pixel
      auto kids = FinerTiles(src_, z_, tx, ty);
      if (kids.empty()) return nullptr;
      const uint64_t key = uint64_t(63) << 58 | uint64_t(tx) << 29 | uint64_t(ty);
      auto it = cache_.find(key);
      if (it != cache_.end()) return &it->second;
      TileGeo& t = cache_[key];
      const double minD = tileL_ / 384 * 0.5;
      for (auto& k : kids) {
        TileGeo one;
        Load(std::get<0>(k), std::get<1>(k), std::get<2>(k), one);
        for (auto& ln : one.lines) { Thin(ln.second, minD); if (ln.second.size() >= 2) t.lines.push_back(std::move(ln)); }
        for (auto& pg : one.land) { for (auto& r : pg) Thin(r, minD); t.land.push_back(std::move(pg)); }
        t.cover.insert(t.cover.end(), one.cover.begin(), one.cover.end());
      }
      return &t;
    }
    const int zz = z_ - d; const int64_t ax = tx >> d, ay = ty >> d;
    const uint64_t key = uint64_t(zz) << 58 | uint64_t(ax) << 29 | uint64_t(ay);
    auto it = cache_.find(key);
    if (it != cache_.end()) return &it->second;
    TileGeo& t = cache_[key];
    Load(zz, ax, ay, t);
    return &t;
  }

  static void Thin(Ring& r, double minD) {
    if (r.size() < 3) return;
    Ring o; o.push_back(r.front());
    for (size_t i = 1; i + 1 < r.size(); ++i)
      if (std::fabs(r[i].first - o.back().first) + std::fabs(r[i].second - o.back().second) >= minD) o.push_back(r[i]);
    o.push_back(r.back());
    r.swap(o);
  }

  void Load(int zz, int64_t ax, int64_t ay, TileGeo& t) {
    std::vector<uint8_t> raw;
    if (!rd_.Read(zz, ax, ay, raw)) return;
    const double tl = 2 * kPi * kRl / double(int64_t(1) << zz);
    const double aX0 = -kPi * kRl + ax * tl, aY1 = kPi * kRl - ay * tl;
    struct CoverGuard {   // on exit: the tile's area, cut back to its data on sides with no neighbouring tile
      ContourBands& cb; int zz; int64_t ax, ay; double x0, y1, tl; TileGeo& t;
      ~CoverGuard() {
        std::array<double, 4> rc{x0, y1 - tl, x0 + tl, y1}, bb{1e300, 1e300, -1e300, -1e300};
        auto add = [&](const Ring& r) { for (auto& p : r) { bb[0] = std::min(bb[0], p.first); bb[1] = std::min(bb[1], p.second); bb[2] = std::max(bb[2], p.first); bb[3] = std::max(bb[3], p.second); } };
        for (auto& l : t.lines) add(l.second);
        for (auto& pg : t.land) for (auto& r : pg) add(r);
        if (bb[0] <= bb[2]) {
          const Source& s = cb.src_;
          auto beyond = [&](int64_t x, int64_t y) {   // the data really ends there (same rule as Get() for empty tiles)
            return !s.Has(zz, x, y) && !(gLand ? s.NearData(zz, x, y, 2) : s.InsideData(zz, x, y, 20));
          };
          if (beyond(ax - 1, ay)) rc[0] = std::max(rc[0], bb[0]);
          if (beyond(ax + 1, ay)) rc[2] = std::min(rc[2], bb[2]);
          if (beyond(ax, ay + 1)) rc[1] = std::max(rc[1], bb[1]);
          if (beyond(ax, ay - 1)) rc[3] = std::min(rc[3], bb[3]);
        }
        if (rc[0] < rc[2] && rc[1] < rc[3]) t.cover.push_back(rc);
      }
    } guard{*this, zz, ax, ay, aX0, aY1, tl, t};
    for (auto& pg : LandIn(aX0 - tl * 0.02, aY1 - tl * 1.02, aX0 + tl * 1.02, aY1 + tl * 0.02)) t.land.push_back(std::move(pg));
    for (auto& L : DecodeTile(raw)) {
      const double sc = tl / L.extent;
      for (auto& F : L.feats) {
        double v;
        if (F.type == 2 && cls_.ContourValue(L, F, v)) {
          for (auto& r : DecodeGeom(F.geom)) {
            for (auto& p : r) { p.first = aX0 + p.first * sc; p.second = aY1 - p.second * sc; }
            t.lines.push_back({v, std::move(r)});
          }
        } else if (F.type == 3) {
          std::string nm;
          if (cls_.Classify(L, F, nm) != kRoleLand) continue;
          auto rings = DecodeGeom(F.geom);
          for (auto& r : rings) for (auto& p : r) { p.first = aX0 + p.first * sc; p.second = aY1 - p.second * sc; }
          t.land.push_back(std::move(rings));
        }
      }
    }
  }

  // Raster of tile (tx, ty) plus a margin of `margin` pixels (default: rad whole tiles) on every side, built
  // from the tiles within rad of it.
  void Build(int64_t tx, int64_t ty, int rad, int R, Grid& g, int margin = -1) {
    if (margin < 0) margin = rad * R;
    g.off = margin;
    g.N = R + 2 * margin;
    g.px = tileL_ / R;
    g.x0 = -kPi * kRl + double(tx) * tileL_ - margin * g.px;
    g.y1 = kPi * kRl - double(ty) * tileL_ + margin * g.px;
    const size_t NN = size_t(g.N) * g.N;
    g.kind.assign(NN, kVoid); g.lev.assign(NN, -1); g.lab.assign(NN, -1); g.levLo.assign(NN, -1); g.levHi.assign(NN, -1);
    g.levels.clear();
    std::set<const TileGeo*> seen;
    for (int64_t y = ty - rad; y <= ty + rad; ++y)
      for (int64_t x = tx - rad; x <= tx + rad; ++x) {
        const TileGeo* t = Get(x, y);
        if (!t || !seen.insert(t).second) continue;
        for (auto& rc : t->cover) {                       // water wherever there is data; void elsewhere
          const int c0 = std::max(0, int(std::lround((rc[0] - g.x0) / g.px))), c1 = std::min(g.N, int(std::lround((rc[2] - g.x0) / g.px)));
          const int r0 = std::max(0, int(std::lround((g.y1 - rc[3]) / g.px))), r1 = std::min(g.N, int(std::lround((g.y1 - rc[1]) / g.px)));
          for (int r = r0; r < r1; ++r) std::fill(g.kind.begin() + (size_t(r) * g.N + c0), g.kind.begin() + (size_t(r) * g.N + std::max(c0, c1)), kWater);
        }
      }
    for (const TileGeo* t : seen)
      for (auto& pg : t->land) FillPoly(g, pg);
    for (const TileGeo* t : seen)
      for (auto& ln : t->lines) {
        auto it = std::find(g.levels.begin(), g.levels.end(), ln.first);
        int k = int(it - g.levels.begin());
        if (it == g.levels.end()) { if (g.levels.size() >= 30000) continue; g.levels.push_back(ln.first); }
        DrawLine(g, ln.second, int16_t(k));
      }
    // label water regions (4-connected)
    int32_t nReg = 0;
    std::vector<int32_t> stack;
    for (size_t i = 0; i < NN; ++i) {
      if (g.kind[i] != kWater || g.lab[i] >= 0) continue;
      g.lab[i] = nReg; stack.push_back(int32_t(i));
      while (!stack.empty()) {
        const int32_t p = stack.back(); stack.pop_back();
        const int r = p / g.N, c = p % g.N;
        const int32_t nb[4] = {c > 0 ? p - 1 : -1, c + 1 < g.N ? p + 1 : -1, r > 0 ? p - g.N : -1, r + 1 < g.N ? p + g.N : -1};
        for (int32_t q : nb)
          if (q >= 0 && g.kind[size_t(q)] == kWater && g.lab[size_t(q)] < 0) { g.lab[size_t(q)] = nReg; stack.push_back(q); }
      }
      ++nReg;
    }
    g.depth.assign(size_t(nReg), std::nan(""));
    g.touched.assign(size_t(nReg), {}); g.landAdj.assign(size_t(nReg), 0); g.leaky.assign(size_t(nReg), 0); g.open.assign(size_t(nReg), 0);
    g.across.clear();
    auto touch = [&](int32_t reg, int16_t k) {
      auto& t = g.touched[size_t(reg)];
      if (std::find(t.begin(), t.end(), k) == t.end()) t.push_back(k);
    };
    for (int r = 0; r < g.N; ++r)
      for (int c = 0; c < g.N; ++c) {
        const size_t i = size_t(r) * g.N + c;
        if (g.kind[i] == kLine) {
          int32_t regs[8]; int nr = 0;
          for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
              const int rr = r + dy, cc = c + dx;
              if ((!dx && !dy) || rr < 0 || cc < 0 || rr >= g.N || cc >= g.N) continue;
              const int32_t l = g.lab[size_t(rr) * g.N + cc];
              if (l < 0) continue;
              bool dup = false;
              for (int k = 0; k < nr; ++k) dup |= regs[k] == l;
              if (!dup) regs[nr++] = l;
            }
          for (int a = 0; a < nr; ++a) {
            touch(regs[a], g.levLo[i]); touch(regs[a], g.levHi[i]);
            for (int b = a + 1; b < nr; ++b) g.across.push_back({std::min(regs[a], regs[b]), std::max(regs[a], regs[b]), g.lev[i]});
          }
        } else if (g.kind[i] == kWater) {
          const int32_t l = g.lab[i];
          if ((c > 0 && g.kind[i - 1] == kLand) || (c + 1 < g.N && g.kind[i + 1] == kLand) ||
              (r > 0 && g.kind[i - g.N] == kLand) || (r + 1 < g.N && g.kind[i + g.N] == kLand)) g.landAdj[size_t(l)] = 1;
          if (c == 0 || r == 0 || c + 1 == g.N || r + 1 == g.N || g.kind[i - 1] == kVoid || g.kind[i + 1] == kVoid ||
              g.kind[i - g.N] == kVoid || g.kind[i + g.N] == kVoid) g.open[size_t(l)] = 1;
        }
      }
    std::sort(g.across.begin(), g.across.end());
    g.across.erase(std::unique(g.across.begin(), g.across.end()), g.across.end());
  }

  // Land behaves like a contour at the shoreline: just above the lowest (drying) level when the data has
  // 0 m / drying contours, else at 0 m.
  // Land behaves like a contour at the shoreline: just above the lowest drying contour when the data has
  // 0 m / drying contours; otherwise the coastline (mean high water) is taken as --drying-height above chart
  // datum, so a strip of drying (green) foreshore appears along shallow, flat shores and almost none on steep ones.
  double Shore(const Grid& g) const {
    double minLev = 1e300;
    for (double v : g.levels) minLev = std::min(minLev, v);
    return minLev <= 0 ? minLev - 1 : -std::max(0.0, cls_.o.dryingHeight);
  }

  void Solve(Grid& g) {
    double firstPos = -1;
    for (double v : g.levels) if (v > 0 && (firstPos < 0 || v < firstPos)) firstPos = v;
    const double shore = Shore(g);
    std::vector<double> sorted(g.levels); std::sort(sorted.begin(), sorted.end());
    g.leaky.assign(g.depth.size(), 0);
    for (size_t r = 0; r < g.depth.size(); ++r) {
      const auto& t = g.touched[r];
      if (t.empty()) {
        if (g.landAdj[r]) g.depth[r] = 0.5 * (std::max(shore, 0.0) + (firstPos > 0 ? firstPos : 2.0));  // shore water
        continue;
      }
      double lo = 1e300, hi = -1e300;
      for (int16_t k : t) { lo = std::min(lo, g.levels[size_t(k)]); hi = std::max(hi, g.levels[size_t(k)]); }
      // a region between two lines touches exactly two neighbouring levels; more means a line ends inside
      // the window (data edge) and the region leaks around it: leave it to the per-pixel interpolation
      bool leak = t.size() > 2;
      if (t.size() == 2)
        for (double v : sorted) leak |= v > lo && v < hi;
      if (leak) { g.leaky[r] = 1; continue; }
      if (g.landAdj[r]) lo = std::min(lo, shore);
      if (t.size() == 1 && !g.landAdj[r]) continue;       // one level only (hole / shoal): decided by its neighbour
      g.depth[r] = 0.5 * (lo + hi);
    }
    Propagate(g);
  }

  static void Propagate(Grid& g) {
    for (bool changed = true; changed;) {
      changed = false;
      for (auto& [a, b, k] : g.across) {
        const double L = g.levels[size_t(k)];
        double& da = g.depth[size_t(a)]; double& db = g.depth[size_t(b)];
        if (std::isnan(da) == std::isnan(db)) continue;
        const bool aUnknown = std::isnan(da);
        if (g.leaky[size_t(aUnknown ? a : b)]) continue;
        double& known = aUnknown ? db : da; double& unk = aUnknown ? da : db;
        if (known == L) continue;
        unk = known < L ? L + 0.25 : L - 0.25;          // the other side of contour L
        changed = true;
      }
    }
  }

  // Regions still unknown (a pit or bump inside one contour ring, whose surroundings touch several close
  // levels): look across each of their line pixels at the depth of the water on the other side; if that side
  // is shallower than the line, the region is deeper, and vice versa (majority over the whole boundary).
  void Vote(Grid& g) {
    const double shore = Shore(g);
    auto pixDepth = [&](size_t i) -> double {
      if (g.lab2[i] >= 0) return Interp(g, g.lab1[i], g.d1[i], g.lab2[i], g.d2[i], shore);
      return g.lab[i] >= 0 ? g.depth[size_t(g.lab[i])] : std::nan("");
    };
    std::vector<int> vote(g.depth.size(), 0);
    bool any = false;
    for (double d : g.depth) any |= std::isnan(d);
    if (!any) return;
    for (int r = 1; r + 1 < g.N; ++r)
      for (int c = 1; c + 1 < g.N; ++c) {
        const size_t i = size_t(r) * g.N + c;
        if (g.kind[i] != kLine) continue;
        const double L = g.levels[size_t(g.lev[i])];
        int32_t unk[8]; int nu = 0; int side = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const size_t q = size_t(r + dy) * g.N + (c + dx);
            if (g.kind[q] != kWater) continue;
            const int32_t l = g.lab[q];
            if (std::isnan(g.depth[size_t(l)])) { if (nu < 8) unk[nu++] = l; continue; }
            const double d = pixDepth(q);
            if (std::isnan(d) || d == L) continue;
            side += d < L ? 1 : -1;                         // known water here is shallower (+1) / deeper (-1)
          }
        if (!side) continue;
        for (int k = 0; k < nu; ++k) vote[size_t(unk[k])] += side > 0 ? 1 : -1;
      }
    // the level of each unknown region's boundary: the level of the line pixels around it
    std::vector<double> lev(g.depth.size(), std::nan(""));
    for (auto& [a, b, k] : g.across) {
      if (std::isnan(g.depth[size_t(a)])) lev[size_t(a)] = g.levels[size_t(k)];
      if (std::isnan(g.depth[size_t(b)])) lev[size_t(b)] = g.levels[size_t(k)];
    }
    for (size_t r = 0; r < g.depth.size(); ++r) {
      if (!std::isnan(g.depth[r]) || !vote[r]) continue;
      double L = lev[r];
      if (std::isnan(L) && g.touched[r].size() == 1) L = g.levels[size_t(g.touched[r][0])];
      if (std::isnan(L)) continue;
      g.depth[r] = vote[r] > 0 ? L + 0.25 : L - 0.25;
    }
    std::fill(g.leaky.begin(), g.leaky.end(), 0);
    Propagate(g);
  }

  // Depth of a water pixel between its two nearest lines. A line pixel can hold a stack of levels (drop-off):
  // each side uses the level of the stack that faces the other line.
  static double Interp(const Grid& g, int32_t la, int32_t da, int32_t lb, int32_t db, double shore) {
    auto range = [&](int32_t l, double& lo, double& hi) {
      if (l == kShoreLab) { lo = hi = shore; return; }
      lo = g.levels[size_t(l >> 15)]; hi = g.levels[size_t(l & 0x7FFF)];
    };
    double a1, a2, b1, b2;
    range(la, a1, a2); range(lb, b1, b2);
    double a, b;
    if (b2 <= a1) { a = a1; b = b2; } else if (b1 >= a2) { a = a2; b = b1; } else { a = b = std::max(a1, b1); }
    return a + (b - a) * double(da) / double(da + db);
  }

  // For every water pixel, the two nearest distinct levels (contour levels, or the shore) by 4-connected
  // distance through water; a line pixel only passes on its own level, land passes nothing.
  static void Nearest2(Grid& g) {
    const size_t NN = size_t(g.N) * g.N;
    g.lab1.assign(NN, -1); g.lab2.assign(NN, -1); g.d1.assign(NN, 0); g.d2.assign(NN, 0);
    std::vector<std::pair<int32_t, int32_t>> cur, nxt;
    for (size_t i = 0; i < NN; ++i) {
      if (g.kind[i] == kLine) { const int32_t lab = int32_t(g.levLo[i]) << 15 | g.levHi[i]; g.lab1[i] = lab; cur.push_back({int32_t(i), lab}); }
      else if (g.kind[i] == kLand) {
        const int r = int(i / g.N), c = int(i % g.N);
        if ((c > 0 && g.kind[i - 1] != kLand) || (c + 1 < g.N && g.kind[i + 1] != kLand) ||
            (r > 0 && g.kind[i - g.N] != kLand) || (r + 1 < g.N && g.kind[i + g.N] != kLand)) cur.push_back({int32_t(i), kShoreLab});
      }
    }
    for (int32_t d = 1; !cur.empty(); ++d) {
      nxt.clear();
      for (auto [p, lab] : cur) {
        const int r = p / g.N, c = p % g.N;
        const int32_t nb[4] = {c > 0 ? p - 1 : -1, c + 1 < g.N ? p + 1 : -1, r > 0 ? p - g.N : -1, r + 1 < g.N ? p + g.N : -1};
        for (int32_t q : nb) {
          if (q < 0 || g.kind[size_t(q)] != kWater) continue;
          int32_t& l1 = g.lab1[size_t(q)]; int32_t& l2 = g.lab2[size_t(q)];
          if (l1 < 0) { l1 = lab; g.d1[size_t(q)] = d; nxt.push_back({q, lab}); }
          else if (l1 != lab && l2 < 0) { l2 = lab; g.d2[size_t(q)] = d; nxt.push_back({q, lab}); }
        }
      }
      std::swap(cur, nxt);
    }
  }

  static void FillPoly(Grid& g, const std::vector<Ring>& rings) {   // even-odd scanline at pixel centres
    double minY = 1e300, maxY = -1e300;
    for (auto& r : rings) for (auto& p : r) { minY = std::min(minY, p.second); maxY = std::max(maxY, p.second); }
    const int r0 = std::max(0, int(std::floor((g.y1 - maxY) / g.px))), r1 = std::min(g.N - 1, int(std::ceil((g.y1 - minY) / g.px)));
    std::vector<double> xs;
    for (int r = r0; r <= r1; ++r) {
      const double Y = g.y1 - (r + 0.5) * g.px;
      xs.clear();
      for (auto& q : rings)
        for (size_t i = 0; i + 1 < q.size(); ++i) {
          auto a = q[i], b = q[i + 1];
          if ((a.second <= Y) != (b.second <= Y)) xs.push_back(a.first + (Y - a.second) / (b.second - a.second) * (b.first - a.first));
        }
      std::sort(xs.begin(), xs.end());
      for (size_t i = 0; i + 1 < xs.size(); i += 2) {
        const int c0 = std::max(0, int(std::ceil((xs[i] - g.x0) / g.px - 0.5))), c1 = std::min(g.N - 1, int(std::floor((xs[i + 1] - g.x0) / g.px - 0.5)));
        for (int c = c0; c <= c1; ++c) if (g.kind[size_t(r) * g.N + c] != kVoid) g.kind[size_t(r) * g.N + c] = kLand;
      }
    }
  }

  static void DrawLine(Grid& g, const Ring& r, int16_t k) {   // 8-connected DDA, land pixels stay land
    auto put = [&](int64_t c, int64_t rr) {
      if (c < 0 || rr < 0 || c >= g.N || rr >= g.N) return;
      const size_t i = size_t(rr) * g.N + size_t(c);
      if (g.kind[i] == kLand || g.kind[i] == kVoid) return;
      if (g.kind[i] != kLine) { g.levLo[i] = g.levHi[i] = k; }
      else {
        if (g.levels[size_t(k)] < g.levels[size_t(g.levLo[i])]) g.levLo[i] = k;
        if (g.levels[size_t(k)] > g.levels[size_t(g.levHi[i])]) g.levHi[i] = k;
      }
      g.kind[i] = kLine; g.lev[i] = k;
    };
    for (size_t i = 0; i + 1 < r.size(); ++i) {
      const double ax = (r[i].first - g.x0) / g.px, ay = (g.y1 - r[i].second) / g.px;
      const double bx = (r[i + 1].first - g.x0) / g.px, by = (g.y1 - r[i + 1].second) / g.px;
      if (std::max(ax, bx) < -1 || std::max(ay, by) < -1 || std::min(ax, bx) > g.N + 1 || std::min(ay, by) > g.N + 1) continue;
      int64_t x0 = int64_t(std::floor(ax)), y0 = int64_t(std::floor(ay)), x1 = int64_t(std::floor(bx)), y1 = int64_t(std::floor(by));
      const int64_t dx = std::llabs(x1 - x0), dy = -std::llabs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
      int64_t err = dx + dy;
      for (;;) {                                            // Bresenham: 8-connected
        put(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        const int64_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
      }
    }
  }

  // Pixel-edge tracing of band b; returns polygons (exterior + holes) in Lowrance metres.
  static std::vector<std::vector<Ring>> Trace(const std::vector<int>& band, const std::vector<uint8_t>& ckind, int R, int b, double x0, double y1, double px,
                                               const Snapper& sn, std::vector<int32_t>& out1, std::vector<int32_t>& out2) {
    auto in = [&](int r, int c) { return r >= 0 && c >= 0 && r < R && c < R && band[size_t(r) * R + c] == b; };
    const int V = R + 1;
    if (out1.size() != size_t(V) * V) { out1.assign(size_t(V) * V, -1); out2.assign(size_t(V) * V, -1); }   // corner (c, r-down) -> next corner
    auto edge = [&](int c0, int r0, int c1, int r1) {
      const size_t a = size_t(r0) * V + c0; const int32_t t = int32_t(size_t(r1) * V + c1);
      if (out1[a] < 0) out1[a] = t; else out2[a] = t;
    };
    for (int r = 0; r < R; ++r)
      for (int c = 0; c < R; ++c) {
        if (!in(r, c)) continue;                            // band on the right in y-up -> exteriors clockwise
        if (!in(r - 1, c)) edge(c, r, c + 1, r);
        if (!in(r, c + 1)) edge(c + 1, r, c + 1, r + 1);
        if (!in(r + 1, c)) edge(c + 1, r + 1, c, r + 1);
        if (!in(r, c - 1)) edge(c, r + 1, c, r);
      }
    std::vector<Ring> rings; std::vector<double> areas;
    std::vector<int32_t> corner;
    for (size_t s = 0; s < out1.size(); ++s) {
      while (out1[s] >= 0 || out2[s] >= 0) {
        corner.clear();
        int32_t v = int32_t(s);
        for (;;) {
          int32_t& o = out1[size_t(v)] >= 0 ? out1[size_t(v)] : out2[size_t(v)];
          if (o < 0) break;
          corner.push_back(v);
          const int32_t nx = o; o = -1; v = nx;
          if (v == int32_t(s)) break;
        }
        if (corner.size() < 4) continue;
        const size_t n = corner.size();
        // per edge: the real line it lies next to (the contour at the depth between the two bands, or the land
        // outline); -2 = none (tile edge, or no data beyond)
        std::vector<int> ekey(n, -2), eto(n, -2);         // first line to try, and the last (several bands met)
        for (size_t i = 0; i < n; ++i) {
          const int32_t a = corner[i], e = corner[(i + 1) % n];
          const int c0 = a % V, r0 = a / V, dc = e % V - c0, dr = e / V - r0;
          int orow, ocol;                                   // the pixel on the far side of the edge
          if (dc == 1) { orow = r0 - 1; ocol = c0; } else if (dr == 1) { orow = r0; ocol = c0; }
          else if (dc == -1) { orow = r0; ocol = c0 - 1; } else { orow = r0 - 1; ocol = c0 - 1; }
          if (orow < 0 || ocol < 0 || orow >= R || ocol >= R) continue;
          const int ob = band[size_t(orow) * R + ocol];
          if (ob > b) { ekey[i] = b + 1; eto[i] = ob; }       // the line at this band's own edge first
          else if (ob >= 0 && ob < b) { ekey[i] = b; eto[i] = ob + 1; }
          else if (ob < 0 && ckind[size_t(orow) * R + ocol] == kLand) ekey[i] = eto[i] = Snapper::kLandKey;
        }
        // edge midpoints (corner-cut), moved onto that line when it is within 2 pixels; corners on the tile edge
        // stay on it (slid along it onto the line) so neighbouring tiles meet
        struct P { int64_t rx, ry; double x, y; bool snapped; };
        std::vector<P> m;
        for (size_t i = 0; i < n; ++i) {
          const int32_t a = corner[i], e = corner[(i + 1) % n];
          const int ac = a % V, ar = a / V;
          const bool vEdge = ac == 0 || ac == R, hEdge = ar == 0 || ar == R;
          if (vEdge || hEdge) {
            P p{2 * int64_t(ac), 2 * int64_t(ar), double(ac), double(ar), false};
            const size_t j = ekey[i] != -2 ? i : (i + n - 1) % n;
            if (ekey[j] != -2 && vEdge != hEdge)
              for (int k = ekey[j];; k += ekey[j] <= eto[j] ? 1 : -1) {
                if ((p.snapped = sn.OnEdge(p.x, p.y, vEdge, k, 2.0)) || k == eto[j]) break;
              }
            m.push_back(p);
          }
          P p{int64_t(ac) + e % V, int64_t(ar) + e / V, 0, 0, false};
          p.x = double(p.rx) * 0.5; p.y = double(p.ry) * 0.5;
          if (ekey[i] != -2)
            for (int k = ekey[i];; k += ekey[i] <= eto[i] ? 1 : -1) {
              if ((p.snapped = sn.Project(p.x, p.y, k, 2.0)) || k == eto[i]) break;
            }
          m.push_back(p);
        }
        std::vector<const P*> q;
        for (size_t i = 0; i < m.size(); ++i) {
          const P& p = m[(i + m.size() - 1) % m.size()]; const P& c = m[i]; const P& nn = m[(i + 1) % m.size()];
          if (!p.snapped && !c.snapped && !nn.snapped && (c.rx - p.rx) * (nn.ry - c.ry) - (c.ry - p.ry) * (nn.rx - c.rx) == 0) continue;
          if (!q.empty() && q.back()->x == c.x && q.back()->y == c.y) continue;
          q.push_back(&c);
        }
        if (q.size() < 3) continue;
        Ring rg;
        for (auto* p : q) rg.push_back({x0 + p->x * px, y1 - p->y * px});
        rg.push_back(rg.front());
        double a2 = 0;
        for (size_t i = 0; i + 1 < rg.size(); ++i) a2 += rg[i].first * rg[i + 1].second - rg[i + 1].first * rg[i].second;
        if (a2 == 0) continue;
        rings.push_back(std::move(rg)); areas.push_back(a2);
      }
    }
    // group holes (anticlockwise, area > 0) under the smallest clockwise exterior containing them
    std::vector<std::vector<Ring>> polys; std::vector<size_t> extIdx;
    for (size_t i = 0; i < rings.size(); ++i) if (areas[i] < 0) { polys.push_back({rings[i]}); extIdx.push_back(i); }
    std::vector<std::array<double, 4>> bb;
    for (size_t e : extIdx) {
      std::array<double, 4> b4{1e300, 1e300, -1e300, -1e300};
      for (auto& p : rings[e]) { b4[0] = std::min(b4[0], p.first); b4[1] = std::min(b4[1], p.second); b4[2] = std::max(b4[2], p.first); b4[3] = std::max(b4[3], p.second); }
      bb.push_back(b4);
    }
    for (size_t i = 0; i < rings.size(); ++i) {
      if (areas[i] < 0) continue;
      // test point: just inside the hole's first edge on the band side is ambiguous; use the hole's bbox centre
      // of its first vertex instead (vertices lie strictly inside the enclosing exterior)
      const auto P = rings[i][0];
      int best = -1;
      for (size_t k = 0; k < extIdx.size(); ++k) {
        if (P.first < bb[k][0] || P.first > bb[k][2] || P.second < bb[k][1] || P.second > bb[k][3]) continue;
        const Ring& e = rings[extIdx[k]];
        bool c = false;
        for (size_t j = 0; j + 1 < e.size(); ++j) {
          auto A = e[j], Bq = e[j + 1];
          if ((A.second > P.second) != (Bq.second > P.second) && P.first < A.first + (P.second - A.second) / (Bq.second - A.second) * (Bq.first - A.first)) c = !c;
        }
        if (c && (best < 0 || -areas[extIdx[k]] < -areas[extIdx[size_t(best)]])) best = int(k);
      }
      if (best >= 0) polys[size_t(best)].push_back(rings[i]);
    }
    return polys;
  }

  const Source& src_; TileReader& rd_; const Classifier& cls_;
  int z_; double tileL_;
  std::unordered_map<uint64_t, TileGeo> cache_;
  Grid fine_, coarse_;                                   // working rasters, reused tile after tile
  std::vector<int32_t> trOut1_, trOut2_;                 // Trace scratch (left all -1 after each use)
  size_t CachePoints() const {
    size_t n = 0;
    for (auto& [k, t] : cache_) { for (auto& l : t.lines) n += l.second.size(); for (auto& pg : t.land) for (auto& r : pg) n += r.size(); }
    return n;
  }
};

// One conversion pass (Large detail layout or Medium overview layout) -> list of encoded blocks in `store`.
struct PassResult { std::vector<std::pair<std::pair<int64_t, int64_t>, std::pair<at5::BlockRef, std::array<int64_t, 4>>>> blocks; long long feats = 0, tiles = 0; };

// Appends a partial block (one tile row) to the block being assembled; per cell and type the encoded features
// simply concatenate.
void MergeInto(BlockBuild& dst, BlockBuild& part) {
  for (auto& [root, kids] : part.cells)
    for (auto& [child, cd] : kids)
      for (auto& [type, bytes] : cd.byType) {
        auto& d = dst.cells[root][child].byType[type];
        d.insert(d.end(), bytes.begin(), bytes.end());
      }
  dst.ext[0] = std::min(dst.ext[0], part.ext[0]); dst.ext[1] = std::min(dst.ext[1], part.ext[1]);
  dst.ext[2] = std::max(dst.ext[2], part.ext[2]); dst.ext[3] = std::max(dst.ext[3], part.ext[3]);
}

PassResult RunPass(const Source& src, int z, const Layout& Lo, const Classifier& cls, bool synth, int threads, FILE* store, std::mutex& storeMutex) {
  // tiles of zoom z; when the data only exists at finer zooms (overview below the data's zoom), the
  // tiles of z that contain finer data
  std::vector<std::pair<uint64_t, int64_t>> derived;
  const std::vector<std::pair<uint64_t, int64_t>>* tp = &src.rows[size_t(z)];
  if (tp->empty())
    for (int zf = z + 1; zf <= std::min(src.maxZoom, z + 5); ++zf) {
      if (src.rows[size_t(zf)].empty()) continue;
      std::set<uint64_t> keys;
      for (auto& t : src.rows[size_t(zf)]) keys.insert(Source::Key(int64_t(t.first >> 32) >> (zf - z), int64_t(t.first & 0xFFFFFFFF) >> (zf - z)));
      for (uint64_t k : keys) derived.push_back({k, 0});
      tp = &derived;
      break;
    }
  const auto& tiles = *tp;
  const double tileL = 2 * kPi * kRl / double(int64_t(1) << z);  // tile size in Lowrance metres
  std::set<std::pair<int64_t, int64_t>> blockSet;
  for (auto& t : tiles) {
    int64_t x = int64_t(t.first >> 32), y = int64_t(t.first & 0xFFFFFFFF);
    double X0 = -kPi * kRl + x * tileL, Y1 = kPi * kRl - y * tileL, X1 = X0 + tileL, Y0 = Y1 - tileL;
    for (int64_t by = int64_t(std::floor(Y0 / Lo.block)); by <= int64_t(std::floor((Y1 - 1e-6) / Lo.block)); ++by)
      for (int64_t bx = int64_t(std::floor(X0 / Lo.block)); bx <= int64_t(std::floor((X1 - 1e-6) / Lo.block)); ++bx) blockSet.insert({by, bx});
  }
  if (gLand && !blockSet.empty()) {   // with a coastline file: also the blocks of land around the chart (no gaps inland)
    int64_t b0 = INT64_MAX, b1 = INT64_MIN, c0 = INT64_MAX, c1 = INT64_MIN;
    for (auto& [by, bx] : blockSet) { b0 = std::min(b0, by); b1 = std::max(b1, by); c0 = std::min(c0, bx); c1 = std::max(c1, bx); }
    for (int64_t by = b0; by <= b1; ++by)
      for (int64_t bx = c0; bx <= c1; ++bx) {
        if (blockSet.count({by, bx})) continue;
        std::vector<const LandSource::Piece*> ps;
        gLand->Query(double(bx * Lo.block), double(by * Lo.block), double((bx + 1) * Lo.block), double((by + 1) * Lo.block), ps);
        if (!ps.empty()) blockSet.insert({by, bx});
      }
  }
  std::vector<std::pair<int64_t, int64_t>> blocks(blockSet.begin(), blockSet.end());
  // work items: one tile row of one block, in block order, so all cores work on a block together and only a
  // few blocks are in memory at once; the rows of a block are merged in row order (deterministic output)
  struct BlockJob { int64_t tx0, tx1, ty0, ty1; std::vector<BlockBuild> rows; int left = 0; };
  std::vector<BlockJob> jobs(blocks.size());
  std::vector<std::pair<size_t, int64_t>> items;
  const int64_t nmax = (int64_t(1) << z) - 1;
  for (size_t bi = 0; bi < blocks.size(); ++bi) {
    const double bx0 = double(blocks[bi].second * Lo.block), by0 = double(blocks[bi].first * Lo.block);
    BlockJob& J = jobs[bi];
    J.tx0 = std::max<int64_t>(int64_t(std::floor((bx0 + kPi * kRl) / tileL)), 0);
    J.tx1 = std::min(int64_t(std::floor((bx0 + Lo.block + kPi * kRl - 1e-6) / tileL)), nmax);
    J.ty0 = std::max<int64_t>(int64_t(std::floor((kPi * kRl - (by0 + Lo.block)) / tileL)), 0);
    J.ty1 = std::min(int64_t(std::floor((kPi * kRl - by0 - 1e-6) / tileL)), nmax);
    J.left = int(std::max<int64_t>(0, J.ty1 - J.ty0 + 1));
    for (int64_t ty = J.ty0; ty <= J.ty1; ++ty) items.push_back({bi, ty});
  }
  PassResult res;
  std::mutex m;
  std::atomic<size_t> next{0};
  std::atomic<long long> nFeat{0}, nTiles{0};
  std::exception_ptr err;
  auto worker = [&] {
    try {
      TileReader rd(src);
      TileReader rdSynth(src);
      ContourBands cb(src, rdSynth, cls, z);
      std::vector<uint8_t> raw;
      for (;;) {
        const size_t ii = next++;
        if (ii >= items.size()) break;
        const size_t bi = items[ii].first; const int64_t ty = items[ii].second;
        BlockJob& J = jobs[bi];
        BlockBuild B; B.by = blocks[bi].first; B.bx = blocks[bi].second;
        if (gLand) {                                   // coastline file: the land of this tile row of the block
          const double bx0 = double(B.bx * Lo.block), by0 = double(B.by * Lo.block);
          const double ry1 = std::min(kPi * kRl - double(ty) * tileL, by0 + double(Lo.block)), ry0 = std::max(ry1 - tileL, by0);
          const int landType = TypeForRole(kRoleLand, int(cls.bands.size()), Lo.overview);
          if (ry0 < ry1)
            for (auto& pg : LandIn(bx0, ry0, bx0 + double(Lo.block), ry1)) { AddFeature(Lo, B, landType, "", pg, true, bx0, ry0, bx0 + double(Lo.block), ry1); ++nFeat; }
        }
        // features of source tile (zz, ax, ay) clipped to the rect (drops the MVT buffer, restricts ancestor data)
        auto process = [&](int zz, int64_t ax, int64_t ay, double cX0, double cY0, double cX1, double cY1) {
          if (!rd.Read(zz, ax, ay, raw)) return;
          ++nTiles;
          const double tl = 2 * kPi * kRl / double(int64_t(1) << zz);
          const double aX0 = -kPi * kRl + ax * tl, aY1 = kPi * kRl - ay * tl;
          for (auto& L : DecodeTile(raw)) {
            const double sc = tl / L.extent;
            for (auto& F : L.feats) {
              std::string name;
              int role = cls.Classify(L, F, name);
              if (role < 0) continue;
              if (synth && role >= kRoleBand && role < kRoleContourMinor) continue;  // areas rebuilt from contours
              int type = TypeForRole(role, int(cls.bands.size()), Lo.overview);
              if (type < 0) continue;
              auto rings = DecodeGeom(F.geom);
              for (auto& r : rings) for (auto& p : r) { p.first = aX0 + p.first * sc; p.second = aY1 - p.second * sc; }
              if (F.type == 3) {
                // MVT exterior rings have positive area in tile (y-down) space -> negative (clockwise) after the
                // y flip, which is also IMC's exterior orientation; holes keep the opposite orientation.
                std::vector<std::vector<Ring>> polys;
                for (auto& r : rings) {
                  if (r.size() < 4) continue;
                  double a = 0;
                  for (size_t i = 0; i + 1 < r.size(); ++i) a += r[i].first * r[i + 1].second - r[i + 1].first * r[i].second;
                  if (a < 0 || polys.empty()) polys.emplace_back();
                  polys.back().push_back(r);
                }
                for (auto& pg : polys) { AddFeature(Lo, B, type, name, pg, true, cX0, cY0, cX1, cY1); ++nFeat; }
              } else {
                AddFeature(Lo, B, type, name, rings, false, cX0, cY0, cX1, cY1); ++nFeat;
              }
            }
          }
        };
        for (int64_t tx = J.tx0; tx <= J.tx1; ++tx) {
          const double cX0 = -kPi * kRl + tx * tileL, cY1 = kPi * kRl - ty * tileL, cX1 = cX0 + tileL, cY0 = cY1 - tileL;
          // the tile itself, or its nearest ancestor (overzoom fallback, up to 6 levels), or its finer tiles
          const int d = src.Fallback(z, tx, ty);
          if (d == -1) {                                 // empty tile (no feature crosses it)
            if (synth && gLand && src.NearData(z, tx, ty, 2)) nFeat += cb.Make(tx, ty, Lo, B);   // coastline known: colour it
            continue;                                    // otherwise left clear: the plotter's own map shows there
          }
          if (d >= 0) process(z - d, tx >> d, ty >> d, cX0, cY0, cX1, cY1);
          else {
            auto kids = FinerTiles(src, z, tx, ty);
            if (kids.empty()) continue;
            for (auto& [zf, kx, ky] : kids) {
              const double kl = 2 * kPi * kRl / double(int64_t(1) << zf);
              const double kX0 = -kPi * kRl + kx * kl, kY1 = kPi * kRl - ky * kl;
              process(zf, kx, ky, kX0, kY1 - kl, kX0 + kl, kY1);
            }
          }
          if (synth) nFeat += cb.Make(tx, ty, Lo, B);
        }
        BlockBuild full;
        {
          std::lock_guard<std::mutex> g(m);
          if (J.rows.empty()) J.rows.resize(size_t(J.ty1 - J.ty0 + 1));
          J.rows[size_t(ty - J.ty0)] = std::move(B);
          if (--J.left > 0) continue;
          full.by = blocks[bi].first; full.bx = blocks[bi].second;
          for (auto& part : J.rows) MergeInto(full, part);
          std::vector<BlockBuild>().swap(J.rows);
        }
        if (full.cells.empty()) continue;
        auto bytes = EncodeBlock(Lo, full);
        at5::BlockRef ref;
        {
          std::lock_guard<std::mutex> g(storeMutex);
          _fseeki64(store, 0, SEEK_END);
          ref.storeOffset = uint64_t(_ftelli64(store)); ref.size = bytes.size();
          if (fwrite(bytes.data(), 1, bytes.size(), store) != bytes.size()) throw std::runtime_error("temp write failed (disk full?)");
        }
        std::lock_guard<std::mutex> g(m);
        res.blocks.push_back({blocks[bi], {ref, {full.ext[0], full.ext[1], full.ext[2], full.ext[3]}}});
        printf("\r  blocks %zu/%zu, tiles read %lld, features %lld   ", res.blocks.size(), blocks.size(), nTiles.load(), nFeat.load());
      }
    } catch (...) { std::lock_guard<std::mutex> g(m); if (!err) err = std::current_exception(); next = items.size(); }
  };
  std::vector<std::thread> th;
  for (int i = 0; i < threads; ++i) th.emplace_back(worker);
  for (auto& t : th) t.join();
  if (err) std::rethrow_exception(err);
  printf("\n");
  std::sort(res.blocks.begin(), res.blocks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  res.feats = nFeat; res.tiles = nTiles;
  return res;
}

uint64_t WritePass(const PassResult& r, const std::string& outDir, const std::string& name, double splitMb,
                   const std::vector<uint8_t>& lib, uint32_t libHdr, FILE* store, const at5::WriteOptions& wo) {
  std::vector<std::vector<size_t>> files(1);
  uint64_t acc = 0, splitBytes = uint64_t(splitMb * 1e6);
  for (size_t i = 0; i < r.blocks.size(); ++i) {
    uint64_t s = r.blocks[i].second.first.size;
    if (!files.back().empty() && acc + s > splitBytes) { files.emplace_back(); acc = 0; }
    files.back().push_back(i); acc += s;
  }
  uint64_t total = 0;
  for (size_t f = 0; f < files.size(); ++f) {
    std::string nm = name + (files.size() > 1 ? "_" + std::to_string(f) : "") + ".at5";
    std::vector<at5::BlockRef> refs;
    int64_t bb[4] = {INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN};
    for (size_t i : files[f]) {
      refs.push_back(r.blocks[i].second.first);
      const auto& e = r.blocks[i].second.second;
      bb[0] = std::min(bb[0], e[0]); bb[1] = std::min(bb[1], e[1]); bb[2] = std::max(bb[2], e[2]); bb[3] = std::max(bb[3], e[3]);
    }
    int32_t b32[4] = {int32_t(bb[0]), int32_t(bb[1]), int32_t(bb[2]), int32_t(bb[3])};
    uint64_t sz = at5::WriteBlocksFile((fs::path(outDir) / nm).string(), lib, libHdr, refs, store, b32, 1000, wo);
    total += sz;
    printf("  %-30s %12llu bytes, %zu blocks\n", nm.c_str(), (unsigned long long)sz, files[f].size());
  }
  return total;
}
}  // namespace

int Convert(const std::string& input, const std::string& outDir, const Options& o, const std::string& date) {
  auto t0 = std::chrono::steady_clock::now();
  gBadVecTiles = 0;
  Source src = OpenSource(input, o.zoom);
  const int z = src.zoom;
  const auto& tiles = src.rows[size_t(z)];
  printf("Vector MBTiles: zoom %d (%zu tiles; zooms %d..%d)\n", z, tiles.size(), src.minZoom, src.maxZoom);
  // One look at a sample of tiles: contour depths, depth-area depths, and so whether the areas must be built
  // from the contours. Gradient shading steps follow the depths the chart really changes at.
  static LandSource landHolder;
  landHolder = LandSource();
  gLand = nullptr;
  bool landAuto = false;
  if (!o.landFile.empty()) {
    const auto lt = std::chrono::steady_clock::now();
    // land is drawn in every block the chart's blocks span: the chart's extent out to whole overview blocks
    double box[4] = {1e300, 1e300, -1e300, -1e300};
    {
      const double tl = 2 * kPi * kRl / double(int64_t(1) << z), B = double(kMediumLayout.block);
      for (auto& t : tiles) {
        const double X0 = -kPi * kRl + double(int64_t(t.first >> 32)) * tl, Y1 = kPi * kRl - double(int64_t(t.first & 0xFFFFFFFF)) * tl;
        box[0] = std::min(box[0], X0); box[1] = std::min(box[1], Y1 - tl); box[2] = std::max(box[2], X0 + tl); box[3] = std::max(box[3], Y1);
      }
      box[0] = std::floor(box[0] / B) * B - B; box[1] = std::floor(box[1] / B) * B - B;
      box[2] = std::ceil(box[2] / B) * B + B; box[3] = std::ceil(box[3] / B) * B + B;
    }
    const std::string what = Lower(o.landFile);
    size_t n = 0;
    std::string from;
    if (what == "auto" || what == "world" || what == "australia") {
      const std::string world = BuiltinLand("world_coast.land"), aus = BuiltinLand("australia_coast.land");
      if (what == "australia" && aus.empty()) throw std::runtime_error("the Australian coastline (coastlines\\australia_coast.land) is not next to sat2at5.exe");
      if (what != "australia" && world.empty()) throw std::runtime_error("the world coastline (coastlines\\world_coast.land) is not next to sat2at5.exe");
      double ab[4] = {0, 0, 0, 0}, skip[4];
      const double* sk = nullptr;
      if (!aus.empty()) {      // detailed Australian coast; world polygons lying wholly in its extent are left out
        const size_t na = landHolder.Add(aus, box, nullptr, ab);
        skip[0] = ab[0] - 0.02; skip[1] = ab[1] - 0.02; skip[2] = ab[2] + 0.02; skip[3] = ab[3];
        sk = skip;
        n += na;
        if (na) from = "Geoscience Australia";
      }
      if (what != "australia") {
        const size_t nw = landHolder.Add(world, box, sk);
        n += nw;
        if (nw) from += from.empty() ? "GSHHG (world)" : " + GSHHG (world)";
      }
      if (from.empty()) from = "the built-in coastlines (none near this chart)";
      landAuto = what == "auto";
    } else {
      n = landHolder.Add(o.landFile, box, nullptr);
      from = o.landFile;
    }
    landHolder.Finish();
    gLand = &landHolder;
    printf("Coastline: %zu land polygons near the chart from %s (%.1f s)\n", n, from.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - lt).count());
  }
  Options oo = o;
  bool synth = o.areasFromContours == 1;
  {
    Classifier probe(o);
    TileReader rd(src);
    std::set<double> lv, av;
    long long nNeg = 0, nPos = 0, nZero = 0, nLand = 0;
    long long nPt = 0, nPtOnLand = 0;   // a point of each sampled contour line, and how many lie on coastline land
    size_t step = std::max<size_t>(1, tiles.size() / 400);
    for (size_t i = 0; i < tiles.size(); i += step) {
      std::vector<uint8_t> raw;
      if (!rd.Read(z, int64_t(tiles[i].first >> 32), int64_t(tiles[i].first & 0xFFFFFFFF), raw)) continue;
      for (auto& L : DecodeTile(raw))
        for (auto& F : L.feats) {
          double v; std::string nm;
          if (probe.ContourValue(L, F, v)) {
            if (lv.size() < 5000) lv.insert(v);
            (v < 0 ? nNeg : v > 0 ? nPos : nZero)++;
            if (landAuto && gLand) {
              const auto rings = DecodeGeom(F.geom);
              if (!rings.empty() && !rings[0].empty()) {
                const double tl = 2 * kPi * kRl / double(int64_t(1) << z), sc = tl / L.extent;
                const double X = -kPi * kRl + double(int64_t(tiles[i].first >> 32)) * tl + rings[0][0].first * sc;
                const double Y = kPi * kRl - double(int64_t(tiles[i].first & 0xFFFFFFFF)) * tl - rings[0][0].second * sc;
                ++nPt; if (gLand->Contains(X, Y)) ++nPtOnLand;
              }
            }
            continue;
          }
          const int role = probe.Classify(L, F, nm);
          if (role == kRoleLand) ++nLand;
          if (role >= kRoleBand && role < kRoleContourMinor && probe.Attr(L, F, probe.depthKeys, v) && av.size() < 5000) av.insert(v);
        }
    }
    // heights instead of depths (negative under water, e.g. -5 -10 -15): flip them automatically
    if (o.elevationAuto && !o.elevation && av.empty() && nNeg > 0 && nNeg >= 9 * nPos) {
      oo.elevation = true;
      std::set<double> f; for (double v : lv) f.insert(-v); lv.swap(f);
      printf("Contour values are heights (negative under water), so depth = -value (use --no-elevation to turn this off)\n");
    }
    if (o.areasFromContours < 0) synth = av.empty() && !lv.empty();
    // contours of a lake, dam or river lie inside the coastline's land: drawing that land would cover them
    if (landAuto && gLand && nPt >= 10 && nPtOnLand * 10 >= nPt * 8) {
      printf("Coastline: the chart lies inland (lake, dam or river: %lld of %lld sampled contours on land), so no coastline is used\n", nPtOnLand, nPt);
      gLand = nullptr;
    }
    if (lv.empty() && av.empty())
      printf("WARNING: no depth contours or depth areas were found in this file, so the chart has no depths%s.\n"
             "         Run 'sat2at5 <file> --list' to see its layers, then name them with --contour-layers / --depth-layers.\n",
             gLand ? " (only land)" : "");
    oo.dataHasLand = nLand > 0 || gLand;
    if (synth && !nLand && !gLand)
      printf("note: the tiles have no land / coastline polygons, so water shallower than the shallowest contour is only coloured where it is enclosed (reef tops); along the unknown shore it is left clear\n");
    const std::set<double>& edges = synth ? lv : av;
    oo.dataLevels.assign(edges.begin(), edges.end());
    auto noteAt = [&](double d, const char* what) {
      if (edges.empty() || edges.count(d)) return;
      if (synth) printf("note: the data has no %g m contour, so the %s edge at %g m is estimated between the neighbouring lines\n", d, what, d);
      else {
        auto up = edges.upper_bound(d);
        if (up != edges.end()) printf("note: the file's depth areas don't change at %g m, so the %s colour covers whole areas and its edge is at the next area boundary, %g m", d, what, *up);
        else printf("note: the file's depth areas don't change at %g m, so the %s colour covers whole areas", d, what);
        printf("\n");
      }
    };
    if (o.safetyDepth > 0) noteAt(o.safetyDepth, "safety-depth");
    if (o.highlightTo > o.highlightFrom) { noteAt(o.highlightFrom, "highlight"); noteAt(o.highlightTo, "highlight"); }
  }
  Classifier cls(oo);

  if (o.list) {  // layer / attribute inventory from a sample of tiles
    std::map<std::string, std::map<std::string, std::pair<int, std::string>>> inv;
    std::map<std::string, std::map<int, int>> geom;
    TileReader rd(src);
    size_t step = std::max<size_t>(1, tiles.size() / 200);
    for (size_t i = 0; i < tiles.size(); i += step) {
      std::vector<uint8_t> raw;
      int64_t x = int64_t(tiles[i].first >> 32), y = int64_t(tiles[i].first & 0xFFFFFFFF);
      if (!rd.Read(z, x, y, raw)) continue;
      for (auto& L : DecodeTile(raw))
        for (auto& F : L.feats) {
          geom[L.name][F.type]++;
          for (size_t k = 0; k + 1 < F.tags.size(); k += 2)
            if (F.tags[k] < L.keys.size() && F.tags[k + 1] < L.vals.size()) {
              auto& e = inv[L.name][L.keys[F.tags[k]]]; e.first++;
              if (e.second.size() < 40) { const Val& v = L.vals[F.tags[k + 1]]; e.second += (v.num ? std::to_string(v.d).substr(0, 7) : v.s.substr(0, 12)) + " "; }
            }
        }
    }
    for (auto& l : geom) {
      printf("layer '%s':", l.first.c_str());
      for (auto& g : l.second) printf(" %s=%d", g.first == 1 ? "points" : g.first == 2 ? "lines" : g.first == 3 ? "polygons" : "?", g.second);
      printf("\n");
      for (auto& k : inv[l.first]) printf("    %-16s e.g. %s\n", k.first.c_str(), k.second.second.c_str());
    }
    return 0;
  }

  const int threads = o.threads > 0 ? o.threads : int(std::max(1u, std::thread::hardware_concurrency()));
  fs::create_directories(outDir);
  fs::path storePath = fs::path(outDir) / "_sat2at5_vec_store.bin";
  FILE* store = nullptr;
  if (fopen_s(&store, storePath.string().c_str(), "wb+") || !store) throw std::runtime_error("cannot create temp store");
  std::mutex sm;
  at5::WriteOptions wo; wo.date = date; wo.desc = o.desc;
  uint64_t total = 0;
  if (synth) printf("Depth areas: built from the depth contours (the tiles have no depth-area polygons of their own)\n");
  try {
    printf("Detail chart (IMC 'Large' layout, 1 m units) from zoom %d, %d threads; %zu depth colour bands:\n", z, threads, cls.bands.size());
    for (auto& b : cls.bands) {
      if (b.first < -1e8) printf("    shallower than everything -> #%06X\n", unsigned(b.second));
      else printf("    depth >= %g m -> #%06X\n", b.first, unsigned(b.second));
    }
    PassResult large = RunPass(src, z, kLargeLayout, cls, synth, threads, store, sm);
    if (large.blocks.empty()) throw std::runtime_error("no features matched (use --list to see layers/attributes)");
    fflush(store);
    auto lib = BuildLargeLib(cls.bands, o.landColour, o.contourColour, o.minorContourColour == 0xFFFFFFFF ? o.contourColour : o.minorContourColour, o.fineContourZoom, o.safetyColour, o.safetyDepth > 0);
    total += WritePass(large, outDir, o.name, o.splitMb, lib.first, lib.second, store, wo);
    if (o.overview) {
      const int zLow = synth ? std::max(0, src.minZoom - 4) : src.minZoom;   // finer tiles are merged (FinerTiles)
      int zo = o.overviewZoom >= 0 ? std::clamp(o.overviewZoom, zLow, src.maxZoom) : std::clamp(9, zLow, src.maxZoom);
      printf("Overview chart (IMC 'Medium' layout, 80 m units) from zoom %d\n", zo);
      PassResult med = RunPass(src, zo, kMediumLayout, cls, synth, threads, store, sm);
      fflush(store);
      if (!med.blocks.empty())
        { auto mlib = BuildMediumLib(cls.bands, o.landColour);
          total += WritePass(med, outDir, o.overviewName, 1e12, mlib.first, mlib.second, store, wo); }
    }
  } catch (...) { fclose(store); std::error_code ec; fs::remove(storePath, ec); throw; }
  fclose(store);
  fs::remove(storePath);
  if (const long long bad = gBadVecTiles.load())
    printf("WARNING: some tiles of the file are damaged and were skipped (%lld tile reads) - those spots are left blank\n", bad);
  printf("Done: %.1f MB in %.1f s. Copy this folder to the chart card (one folder per chart).\n", total / 1e6,
         std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  return 0;
}

}  // namespace vec

namespace vec {
namespace {
struct VReader {
  FILE* f;
  uint8_t U8() { int c = fgetc(f); if (c == EOF) throw std::runtime_error("unexpected EOF"); return uint8_t(c); }
  void Raw(void* p, size_t n) { if (fread(p, 1, n, f) != n) throw std::runtime_error("unexpected EOF"); }
  uint32_t U32() { uint8_t b[4]; Raw(b, 4); return uint32_t(b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24); }
  uint16_t U16() { uint8_t b[2]; Raw(b, 2); return uint16_t(b[0] | b[1] << 8); }
  uint32_t Var() {
    uint32_t b0 = U8();
    if (b0 & 1) return b0 >> 1;
    if (b0 & 2) return (b0 | U8() << 8) >> 2;
    if (b0 & 4) { uint32_t b1 = U8(), b2 = U8(); return (b0 | b1 << 8 | b2 << 16) >> 3; }
    uint32_t b1 = U8(), b2 = U8(), b3 = U8();
    return (b0 | b1 << 8 | b2 << 16 | b3 << 24) >> 3;
  }
  std::string Str() { uint32_t n = U8(); if (n == 0xFF) n = U16(); std::string s(n, '\0'); if (n) Raw(&s[0], n); return s; }
  std::vector<std::pair<int32_t, int32_t>> Ring() {
    uint32_t n = Var(); std::vector<std::pair<int32_t, int32_t>> pts; pts.reserve(n);
    int32_t x = 0, y = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (!i) { x = int32_t(U32()); y = int32_t(U32()); }
      else {
        int32_t d[2];
        for (int k = 0; k < 2; ++k) { int8_t b = int8_t(U8()); d[k] = b == -128 ? int16_t(U16()) : b; }
        x += d[0]; y += d[1];
      }
      pts.push_back({x, y});
    }
    return pts;
  }
  uint64_t Pos() { return uint64_t(_ftelli64(f)); }
  void Seek(uint64_t p) { _fseeki64(f, int64_t(p), SEEK_SET); }
};

// Feature kinds from the file's own style library: what an instance record looks like for each type index.
enum Kind { kKindUnknown, kKindPoly, kKindLine, kKindChild, kKindPoint };
struct TypeInfo { Kind kind = kKindUnknown; uint32_t rgb = 0; bool hasRgb = false; int paletteIdx = -1; int width = 1; };

struct Chart {
  int scaleUnused = 0;
  std::vector<TypeInfo> types;
  std::vector<uint32_t> blockOffsets;
  uint64_t blocksBase = 0;
};

// Parses header + style library; returns false if the file is not an IMC-style vector chart we can walk.
bool OpenChart(VReader& r, Chart& c) {
  uint8_t h[44]; r.Raw(h, 44);
  uint32_t sec; memcpy(&sec, h + 20, 4);
  r.Seek(44); r.Str(); r.Str(); r.Str();
  std::vector<uint32_t> F; while (r.Pos() + 4 <= 44 + sec) F.push_back(r.U32());
  if (F.size() < 14) return false;
  auto fld = [&](uint32_t o) { return F.at((o - 0x48) / 4); };
  const uint64_t base = 44 + sec;
  r.Seek(base + fld(0x60));
  r.U32(); uint8_t k = r.U8(); uint32_t ng = r.U32();
  if (k != 1 || ng == 0 || ng > 1000) return false;
  std::vector<std::pair<uint32_t, uint32_t>> ents(ng);
  for (auto& e : ents) { r.Str(); r.U32(); e.first = r.U32(); e.second = r.U32(); }
  const uint64_t he = r.Pos();
  for (auto& e : ents) {
    TypeInfo t;
    std::vector<uint8_t> data(e.second);
    r.Seek(he + e.first); r.Raw(data.data(), data.size());
    uint16_t ftype = uint16_t(data[0] | data[1] << 8);
    size_t p = 2;
    for (int s = 0; s < 2; ++s) { size_t n = data[p++]; if (n == 0xFF) { n = data[p] | data[p + 1] << 8; p += 2; } p += n; }
    p += 4 + 4 + 2 + 2 + 2 + 3;  // zmin zmax u16 u16 prio u8 u8 u8
    uint32_t inst = p + 4 <= data.size() ? uint32_t(data[p] | data[p + 1] << 8 | data[p + 2] << 16 | uint32_t(data[p + 3]) << 24) : 0xFFFFFFFF;
    if (ftype == 4) t.kind = kKindChild;
    else if (ftype == 3 && inst == 4) t.kind = kKindPoly;
    else if (ftype == 2 && inst == 0x821) t.kind = kKindLine;
    else if (ftype == 1 && inst == 0) t.kind = kKindPoint;
    // colour: fill record (u32 0, u32 flags 0x11|0x01, u8 index[, u32 rgba]) or pen colour (flags 0xB3)
    for (size_t i = p; i + 9 <= data.size(); ++i) {
      if (memcmp(&data[i], "\0\0\0\0", 4) != 0) continue;
      uint32_t fl = uint32_t(data[i + 4] | data[i + 5] << 8 | data[i + 6] << 16 | uint32_t(data[i + 7]) << 24);
      if (ftype == 3 && (fl == 0x11 || fl == 0x01)) {
        t.paletteIdx = data[i + 8];
        if (fl == 0x11 && i + 13 <= data.size()) { uint32_t v = uint32_t(data[i + 9] | data[i + 10] << 8 | data[i + 11] << 16 | uint32_t(data[i + 12]) << 24); t.rgb = v >> 8; t.hasRgb = true; }
        break;
      }
      if (ftype == 2 && fl == 0xB3 && i + 20 <= data.size()) {
        uint32_t v = uint32_t(data[i + 12] | data[i + 13] << 8 | data[i + 14] << 16 | uint32_t(data[i + 15]) << 24);
        t.rgb = v >> 8; t.hasRgb = true; t.width = std::max(1, int(data[i + 9] | data[i + 10] << 8)); break;
      }
    }
    c.types.push_back(t);
  }
  r.Seek(base + fld(0x70) - 1);
  uint32_t n = r.Var();
  c.blockOffsets.resize(n);
  for (auto& o : c.blockOffsets) { o = r.Var(); r.Var(); }
  c.blocksBase = r.Pos();
  return true;
}

struct Walker {
  VReader& r; const Chart& c;
  uint64_t polys = 0, lines = 0, nodes = 0;
  int scale = 1;
  std::function<void(int, const std::vector<std::vector<std::pair<int32_t, int32_t>>>&, bool)> emit;  // type, rings, isPoly
  void Block() {
    r.U8(); r.U8(); r.U8(); r.U8();
    for (int i = 0; i < 4; ++i) r.U32();
    scale = r.U16(); r.Str();
    uint32_t ns = r.Var(); for (uint32_t i = 0; i < ns; ++i) { r.U32(); r.U32(); }
    r.Var(); r.Var();
    uint32_t m = r.Var(); for (uint32_t i = 0; i < m; ++i) { r.U32(); r.U32(); }
    for (uint32_t i = 0; i < ns; ++i) { for (int j = 0; j < 4; ++j) r.U32(); uint32_t k = r.Var(); for (uint32_t j = 0; j < k; ++j) { r.U16(); r.U8(); } }
    Node();
  }
  void Node() {
    ++nodes;
    for (int i = 0; i < 4; ++i) r.U32();
    uint32_t nx = r.U16(), ny = r.U16(), cnt = r.Var();
    if (cnt != nx * ny) throw std::runtime_error("node grid mismatch");
    std::vector<std::pair<uint32_t, uint32_t>> cells(cnt);
    for (auto& cl : cells) { cl.first = r.Var(); cl.second = r.Var(); }
    const uint64_t ds = r.Pos();
    for (auto& cl : cells) {
      if (!cl.second) continue;
      if (r.Pos() != ds + cl.first) throw std::runtime_error("cell offset mismatch");
      uint32_t k = r.Var();
      std::vector<uint32_t> a(k), len(k);
      for (uint32_t i = 0; i < k; ++i) { a[i] = r.Var(); r.Var(); len[i] = r.Var(); }
      for (uint32_t i = 0; i < k; ++i) {
        if (a[i] >= c.types.size()) throw std::runtime_error("type index out of range");
        Kind kind = c.types[a[i]].kind;
        const uint64_t end = r.Pos() + len[i];
        if (kind == kKindChild) { Node(); continue; }
        while (r.Pos() < end) {
          if (kind == kKindPoly) {
            uint32_t nr = r.Var(); std::vector<std::vector<std::pair<int32_t, int32_t>>> rings;
            for (uint32_t j = 0; j < nr; ++j) rings.push_back(r.Ring());
            ++polys; if (emit) emit(int(a[i]), rings, true);
          } else if (kind == kKindLine) {
            r.Str(); std::vector<std::vector<std::pair<int32_t, int32_t>>> rings{r.Ring()};
            ++lines; if (emit) emit(int(a[i]), rings, false);
          } else throw std::runtime_error("unsupported feature kind for type " + std::to_string(a[i]));
        }
        if (r.Pos() != end) throw std::runtime_error("feature data overruns its triple");
      }
    }
  }
};
}  // namespace

int VerifyBody(const std::string& path) {
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "rb") || !f) return -1;
  VReader r{f};
  Chart c;
  try {
    if (!OpenChart(r, c)) { fclose(f); return -1; }
    bool walkable = !c.types.empty();
    for (auto& t : c.types) if (t.kind == kKindUnknown) walkable = false;
    if (!walkable) { fclose(f); return -1; }  // textures and other libraries are checked elsewhere
    Walker w{r, c};
    for (uint32_t o : c.blockOffsets) { r.Seek(c.blocksBase + o); w.Block(); }
    int bands = 0, rgb = 0;
    for (auto& t : c.types) if (t.kind == kKindPoly) { ++bands; if (t.hasRgb) ++rgb; }
    printf("  vector chart: %zu blocks, %llu nodes, %llu polygons, %llu contour lines; %d area styles (%d with true RGB colour) - structure OK\n",
           c.blockOffsets.size(), (unsigned long long)w.nodes, (unsigned long long)w.polys, (unsigned long long)w.lines, bands, rgb);
    fclose(f);
    return 1;
  } catch (const std::exception& e) {
    printf("  FAIL: vector structure: %s\n", e.what());
    fclose(f);
    return 0;
  }
}

// Render a vector chart (area fills in their RGB colours, contours, land) north-up into a PNG.
// Counts, in a finished chart, the area shapes a plotter's fill can't cut into triangles: rings that meet
// themselves at a point, and rings of one area that cross (or only touch) each other.
int CheckAreas(const std::string& path) {
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "rb") || !f) throw std::runtime_error("cannot open " + path);
  VReader r{f};
  Chart c;
  if (!OpenChart(r, c)) { fclose(f); return -1; }
  using P = std::pair<int32_t, int32_t>;
  auto orient = [](P p, P q, P s) { const int64_t v = int64_t(q.first - p.first) * (s.second - p.second) - int64_t(q.second - p.second) * (s.first - p.first); return (v > 0) - (v < 0); };
  auto onSeg = [](P p, P q, P s) { return std::min(p.first, s.first) <= q.first && q.first <= std::max(p.first, s.first) && std::min(p.second, s.second) <= q.second && q.second <= std::max(p.second, s.second); };
  long long areas = 0, rings = 0, pinch = 0, cross = 0, touch = 0, point = 0;
  Walker w{r, c};
  long long lineFeats = 0, lineVerts = 0, areaVerts = 0;
  w.emit = [&](int, const std::vector<std::vector<P>>& rgs, bool poly) {
    if (!poly) { ++lineFeats; for (auto& rg : rgs) lineVerts += (long long)rg.size(); return; }
    for (auto& rg : rgs) areaVerts += (long long)rg.size();
    ++areas;
    std::vector<std::vector<P>> rs;
    for (auto rg : rgs) { if (rg.size() > 1 && rg.front() == rg.back()) rg.pop_back(); if (rg.size() >= 3) rs.push_back(std::move(rg)); }
    for (size_t i = 0; i < rs.size(); ++i) {
      ++rings;
      std::set<P> seen;
      for (auto& p : rs[i]) if (!seen.insert(p).second) { ++pinch; break; }
      for (size_t j = i + 1; j < rs.size(); ++j) {
        const auto& A = rs[i]; const auto& B = rs[j];
        int kind = 0;   // 2 crossing, 1 sharing a straight edge, 3 touching at a point
        for (size_t u = 0; u < A.size() && kind != 2; ++u)
          for (size_t v = 0; v < B.size() && kind != 2; ++v) {
            const P a = A[u], b = A[(u + 1) % A.size()], s0 = B[v], s1 = B[(v + 1) % B.size()];
            const int o1 = orient(a, b, s0), o2 = orient(a, b, s1), o3 = orient(s0, s1, a), o4 = orient(s0, s1, b);
            if (o1 * o2 < 0 && o3 * o4 < 0) kind = 2;
            else if ((o1 == 0 && onSeg(a, s0, b)) || (o2 == 0 && onSeg(a, s1, b)) || (o3 == 0 && onSeg(s0, a, s1)) || (o4 == 0 && onSeg(s0, b, s1))) {
              // along a shared straight edge (both segments on one line: a cell or tile cut) or at a single point
              const bool shared = o1 == 0 && o2 == 0;
              kind = std::max(kind, shared ? 1 : 3) == 3 ? 3 : std::max(kind, 1);
            }
          }
        if (kind == 2) ++cross; else if (kind == 1) ++touch; else if (kind == 3) ++point;
      }
    }
  };
  for (uint32_t o : c.blockOffsets) { r.Seek(c.blocksBase + o); w.Block(); }
  fclose(f);
  printf("%s: %lld areas, %lld rings; %lld rings meet themselves, %lld ring pairs cross, %lld touch at a point, %lld share a cut edge\n",
         path.c_str(), areas, rings, pinch, cross, point, touch);
  printf("  %lld area points, %lld lines with %lld points\n", areaVerts, lineFeats, lineVerts);
  return 1;
}

int PreviewVector(const std::string& path, const std::string& png, int maxPx, bool drawLines, const double* box) {
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "rb") || !f) throw std::runtime_error("cannot open " + path);
  VReader r{f};
  Chart c;
  if (!OpenChart(r, c)) { fclose(f); return -1; }
  for (auto& t : c.types) if (t.kind == kKindUnknown) { fclose(f); return -1; }
  // pass 1: extent (metres)
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
  {
    Walker w{r, c};
    w.emit = [&](int, const std::vector<std::vector<std::pair<int32_t, int32_t>>>& rings, bool) {
      for (auto& rg : rings) for (auto& p : rg) {
        double X = double(p.first) * w.scale, Y = double(p.second) * w.scale;
        x0 = std::min(x0, X); x1 = std::max(x1, X); y0 = std::min(y0, Y); y1 = std::max(y1, Y);
      }
    };
    for (uint32_t o : c.blockOffsets) { r.Seek(c.blocksBase + o); w.Block(); }
  }
  if (x0 > x1) { fclose(f); throw std::runtime_error("chart has no features"); }
  if (box) {   // only this area (west, south, east, north in degrees)
    auto mx = [](double lon) { return kRl * lon * kPi / 180; };
    auto my = [](double lat) { return kRl * std::log(std::tan(kPi / 4 + lat * kPi / 360)); };
    x0 = mx(box[0]); y0 = my(box[1]); x1 = mx(box[2]); y1 = my(box[3]);
    if (x0 >= x1 || y0 >= y1) { fclose(f); throw std::runtime_error("--bbox must be WEST,SOUTH,EAST,NORTH"); }
  }
  const double span = std::max(x1 - x0, y1 - y0);
  const double ppm = double(maxPx) / std::max(span, 1.0);
  const int W = std::max(1, int((x1 - x0) * ppm) + 1), H = std::max(1, int((y1 - y0) * ppm) + 1);
  std::vector<uint8_t> img(size_t(W) * H * 4, 0);
  for (size_t i = 0; i < img.size(); i += 4) { img[i] = 214; img[i + 1] = 214; img[i + 2] = 214; img[i + 3] = 255; }  // nothing drawn (plotter shows its own map): light grey
  auto colourOf = [&](int type) -> uint32_t {
    const TypeInfo& t = c.types[size_t(type)];
    if (t.hasRgb) return t.rgb;
    return 0x808080;  // palette-only style: real colour is inside the plotter
  };
  std::vector<std::tuple<int, std::vector<std::vector<std::pair<double, double>>>, bool>> lines;
  {
    Walker w{r, c};
    w.emit = [&](int type, const std::vector<std::vector<std::pair<int32_t, int32_t>>>& rings, bool poly) {
      std::vector<std::vector<std::pair<double, double>>> px;
      for (auto& rg : rings) {
        std::vector<std::pair<double, double>> q;
        for (auto& p : rg) q.push_back({(double(p.first) * w.scale - x0) * ppm, (y1 - double(p.second) * w.scale) * ppm});
        px.push_back(std::move(q));
      }
      if (!poly) { if (drawLines) lines.push_back({type, std::move(px), false}); return; }
      // even-odd scanline fill of this polygon (all rings)
      double minY = 1e300, maxY = -1e300;
      for (auto& q : px) for (auto& p : q) { minY = std::min(minY, p.second); maxY = std::max(maxY, p.second); }
      const uint32_t rgb = colourOf(type);
      std::vector<double> xs;
      for (int y = std::max(0, int(minY)); y <= std::min(H - 1, int(maxY)); ++y) {
        const double sy = y + 0.5;
        xs.clear();
        for (auto& q : px)
          for (size_t i = 0; i + 1 < q.size(); ++i) {
            auto a = q[i], b = q[i + 1];
            if ((a.second <= sy) != (b.second <= sy)) xs.push_back(a.first + (sy - a.second) / (b.second - a.second) * (b.first - a.first));
          }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
          for (int x = std::max(0, int(std::ceil(xs[i] - 0.5))); x <= std::min(W - 1, int(std::floor(xs[i + 1] - 0.5))); ++x) {
            uint8_t* d = &img[(size_t(y) * W + x) * 4];
            d[0] = uint8_t(rgb >> 16); d[1] = uint8_t(rgb >> 8); d[2] = uint8_t(rgb);
          }
        }
      }
    };
    for (uint32_t o : c.blockOffsets) { r.Seek(c.blocksBase + o); w.Block(); }
  }
  fclose(f);
  std::stable_sort(lines.begin(), lines.end(), [&](const auto& a, const auto& b) {  // bold lines (safety contour) last
    return c.types[size_t(std::get<0>(a))].width < c.types[size_t(std::get<0>(b))].width;
  });
  for (auto& L : lines) {  // contours on top
    const uint32_t rgb = colourOf(std::get<0>(L));
    const int rad = (c.types[size_t(std::get<0>(L))].width - 1) / 2;
    for (auto& q : std::get<1>(L))
      for (size_t i = 0; i + 1 < q.size(); ++i) {
        double ax = q[i].first, ay = q[i].second, bx = q[i + 1].first, by = q[i + 1].second;
        int steps = int(std::max(std::fabs(bx - ax), std::fabs(by - ay))) + 1;
        for (int s = 0; s <= steps; ++s) {
          const int x0 = int(ax + (bx - ax) * s / steps), y0 = int(ay + (by - ay) * s / steps);
          for (int y = y0 - rad; y <= y0 + rad; ++y)
            for (int x = x0 - rad; x <= x0 + rad; ++x) {
              if (x < 0 || y < 0 || x >= W || y >= H) continue;
              uint8_t* d = &img[(size_t(y) * W + x) * 4];
              d[0] = uint8_t(rgb >> 16); d[1] = uint8_t(rgb >> 8); d[2] = uint8_t(rgb);
            }
        }
      }
  }
  int n = MultiByteToWideChar(CP_UTF8, 0, png.c_str(), -1, nullptr, 0);
  std::wstring w(size_t(n), 0); MultiByteToWideChar(CP_UTF8, 0, png.c_str(), -1, &w[0], n);
  if (!src::SavePng(w, img.data(), W, H)) throw std::runtime_error("PNG save failed");
  printf("preview %s: %dx%d, vector chart, north-up\n", png.c_str(), W, H);
  return 1;
}

}  // namespace vec

#include "depth.hpp"
#include "at5.hpp"
#include "inflate.hpp"

#define NOMINMAX
#include <windows.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace depth {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
const double kRl = at5::kLowranceR;
constexpr double kA = 6378137.0;                  // GRS80 / WGS84 semi-major axis
constexpr double kF = 1.0 / 298.257222101;        // GRS80 flattening (WGS84 differs by 0.1 mm)
const double kE2 = kF * (2 - kF);
const float kMissing = std::numeric_limits<float>::quiet_NaN();
constexpr size_t kMaxCells = 300'000'000;         // bigger grids are read at a reduced resolution

inline bool Valid(float v) { return v == v; }

// ------------------------------------------------------------------ map projections
// Grids come as longitude/latitude, Web Mercator, UTM / MGA zones or Australian Albers. Each converts between
// its map metres (or degrees) and longitude/latitude; GDA94, GDA2020 and WGS84 are treated as one datum
// (they differ by under 2 m).
struct Projection {
  enum Kind { kGeographic, kWebMercator, kTransverseMercator, kAlbers } kind = kGeographic;
  int epsg = 4326;
  double lon0 = 0, k0 = 0.9996, fe = 500000, fn = 0;   // transverse Mercator
  double n = 0, c = 0, rho0 = 0;                       // Albers

  static double M(double phi) {   // meridian distance (Snyder 3-21)
    const double e4 = kE2 * kE2, e6 = e4 * kE2;
    return kA * ((1 - kE2 / 4 - 3 * e4 / 64 - 5 * e6 / 256) * phi - (3 * kE2 / 8 + 3 * e4 / 32 + 45 * e6 / 1024) * std::sin(2 * phi) +
                 (15 * e4 / 256 + 45 * e6 / 1024) * std::sin(4 * phi) - (35 * e6 / 3072) * std::sin(6 * phi));
  }
  static double Q(double phi) {   // Albers q (Snyder 3-12)
    const double e = std::sqrt(kE2), s = std::sin(phi);
    return (1 - kE2) * (s / (1 - kE2 * s * s) - (1 / (2 * e)) * std::log((1 - e * s) / (1 + e * s)));
  }
  static double Mc(double phi) { const double s = std::sin(phi); return std::cos(phi) / std::sqrt(1 - kE2 * s * s); }

  void SetAlbers(double lat0, double lonOrigin, double sp1, double sp2) {
    kind = kAlbers;
    lon0 = lonOrigin * kDeg;
    const double p1 = sp1 * kDeg, p2 = sp2 * kDeg, m1 = Mc(p1), m2 = Mc(p2), q1 = Q(p1), q2 = Q(p2), q0 = Q(lat0 * kDeg);
    n = (m1 * m1 - m2 * m2) / (q2 - q1);
    c = m1 * m1 + n * q1;
    rho0 = kA * std::sqrt(c - n * q0) / n;
  }
  void SetUtm(int zone, bool south) {
    kind = kTransverseMercator;
    lon0 = (zone * 6 - 183) * kDeg;
    k0 = 0.9996; fe = 500000; fn = south ? 10000000 : 0;
  }

  // EPSG codes this reader knows. Returns false for others.
  bool FromEpsg(int code) {
    epsg = code;
    if (code == 4326 || code == 4283 || code == 7844 || code == 4258 || code == 4269 || code == 4202 || code == 4203) { kind = kGeographic; return true; }
    if (code == 3857 || code == 3785 || code == 900913 || code == 102100) { kind = kWebMercator; return true; }
    if (code >= 32601 && code <= 32660) { SetUtm(code - 32600, false); return true; }
    if (code >= 32701 && code <= 32760) { SetUtm(code - 32700, true); return true; }
    if (code >= 28348 && code <= 28358) { SetUtm(code - 28300, true); return true; }   // GDA94 / MGA
    if (code >= 7846 && code <= 7859) { SetUtm(code - 7800, true); return true; }      // GDA2020 / MGA
    if (code == 3577 || code == 9473) { SetAlbers(0, 132, -18, -36); return true; }    // Australian Albers
    return false;
  }

  void ToLonLat(double x, double y, double& lon, double& lat) const {
    switch (kind) {
      case kGeographic: lon = x; lat = y; return;
      case kWebMercator: lon = x / kA / kDeg; lat = (2 * std::atan(std::exp(y / kA)) - kPi / 2) / kDeg; return;
      case kAlbers: {
        const double dy = rho0 - y;
        double rho = std::sqrt(x * x + dy * dy), theta;
        if (n < 0) { rho = -rho; theta = std::atan2(-x, -dy); } else theta = std::atan2(x, dy);
        const double q = (c - rho * rho * n * n / (kA * kA)) / n, e = std::sqrt(kE2);
        double phi = std::asin(std::clamp(q / 2, -1.0, 1.0));
        for (int i = 0; i < 15; ++i) {
          const double s = std::sin(phi), es = 1 - kE2 * s * s;
          const double d = es * es / (2 * std::cos(phi)) * (q / (1 - kE2) - s / es + (1 / (2 * e)) * std::log((1 - e * s) / (1 + e * s)));
          phi += d;
          if (std::fabs(d) < 1e-12) break;
        }
        lon = (lon0 + theta / n) / kDeg; lat = phi / kDeg;
        return;
      }
      case kTransverseMercator: {   // Snyder 8-18 .. 8-25
        const double ep2 = kE2 / (1 - kE2), e1 = (1 - std::sqrt(1 - kE2)) / (1 + std::sqrt(1 - kE2));
        const double mu = ((y - fn) / k0) / (kA * (1 - kE2 / 4 - 3 * kE2 * kE2 / 64 - 5 * kE2 * kE2 * kE2 / 256));
        const double p1 = mu + (3 * e1 / 2 - 27 * e1 * e1 * e1 / 32) * std::sin(2 * mu) + (21 * e1 * e1 / 16 - 55 * std::pow(e1, 4) / 32) * std::sin(4 * mu) +
                          (151 * e1 * e1 * e1 / 96) * std::sin(6 * mu) + (1097 * std::pow(e1, 4) / 512) * std::sin(8 * mu);
        const double s = std::sin(p1), co = std::cos(p1), t = std::tan(p1);
        const double C1 = ep2 * co * co, T1 = t * t, N1 = kA / std::sqrt(1 - kE2 * s * s), R1 = kA * (1 - kE2) / std::pow(1 - kE2 * s * s, 1.5);
        const double D = (x - fe) / (N1 * k0);
        lat = (p1 - (N1 * t / R1) * (D * D / 2 - (5 + 3 * T1 + 10 * C1 - 4 * C1 * C1 - 9 * ep2) * std::pow(D, 4) / 24 +
                                     (61 + 90 * T1 + 298 * C1 + 45 * T1 * T1 - 252 * ep2 - 3 * C1 * C1) * std::pow(D, 6) / 720)) / kDeg;
        lon = (lon0 + (D - (1 + 2 * T1 + C1) * D * D * D / 6 + (5 - 2 * C1 + 28 * T1 - 3 * C1 * C1 + 8 * ep2 + 24 * T1 * T1) * std::pow(D, 5) / 120) / co) / kDeg;
        return;
      }
    }
  }
  void FromLonLat(double lon, double lat, double& x, double& y) const {
    switch (kind) {
      case kGeographic: x = lon; y = lat; return;
      case kWebMercator: x = kA * lon * kDeg; y = kA * std::log(std::tan(kPi / 4 + lat * kDeg / 2)); return;
      case kAlbers: {
        const double rho = kA * std::sqrt(c - n * Q(lat * kDeg)) / n, theta = n * (lon * kDeg - lon0);
        x = rho * std::sin(theta); y = rho0 - rho * std::cos(theta);
        return;
      }
      case kTransverseMercator: {   // Snyder 8-9 .. 8-15
        const double phi = lat * kDeg, ep2 = kE2 / (1 - kE2), s = std::sin(phi), co = std::cos(phi), t = std::tan(phi);
        const double N = kA / std::sqrt(1 - kE2 * s * s), T = t * t, C = ep2 * co * co, A = (lon * kDeg - lon0) * co;
        x = fe + k0 * N * (A + (1 - T + C) * A * A * A / 6 + (5 - 18 * T + T * T + 72 * C - 58 * ep2) * std::pow(A, 5) / 120);
        y = fn + k0 * (M(phi) + N * t * (A * A / 2 + (5 - T + 9 * C + 4 * C * C) * std::pow(A, 4) / 24 +
                                         (61 - 58 * T + T * T + 600 * C - 330 * ep2) * std::pow(A, 6) / 720));
        return;
      }
    }
  }
  // Size on the ground (metres) of one map unit east and north at a latitude.
  void UnitMetres(double lat, double& ex, double& ny) const {
    const double cl = std::cos(lat * kDeg);
    if (kind == kGeographic) { ex = kA * kDeg * cl; ny = kA * kDeg; return; }
    if (kind == kWebMercator) { ex = ny = cl; return; }
    ex = ny = 1;   // UTM / Albers: true metres to well under 1 %
  }
  std::string Name() const {
    switch (kind) {
      case kGeographic: return "longitude/latitude (EPSG:" + std::to_string(epsg) + ")";
      case kWebMercator: return "Web Mercator (EPSG:" + std::to_string(epsg) + ")";
      case kAlbers: return "Australian Albers (EPSG:" + std::to_string(epsg) + ")";
      default: {
        const int zone = int(std::lround((lon0 / kDeg + 183) / 6));
        return "UTM zone " + std::to_string(zone) + (fn > 0 ? "S" : "N") + " (EPSG:" + std::to_string(epsg) + ")";
      }
    }
  }
};

// ------------------------------------------------------------------ TIFF reading
struct Entry { uint16_t type = 0; uint64_t count = 0; std::vector<uint8_t> data; };

class TiffFile {
 public:
  explicit TiffFile(const std::string& path) : path_(path) {
    if (_wfopen_s(&f_, Widen(path).c_str(), L"rb") != 0) f_ = nullptr;
    if (!f_) throw std::runtime_error("cannot open " + path);
    uint8_t h[16] = {};
    if (fread(h, 1, 16, f_) < 8) throw std::runtime_error("not a TIFF file: " + path);
    if (h[0] == 'I' && h[1] == 'I') le_ = true;
    else if (h[0] == 'M' && h[1] == 'M') le_ = false;
    else throw std::runtime_error("not a TIFF file: " + path);
    const uint16_t magic = U16(h + 2);
    if (magic == 42) { big_ = false; first_ = U32(h + 4); }
    else if (magic == 43) { big_ = true; first_ = U64(h + 8); }
    else throw std::runtime_error("not a TIFF file: " + path);
    ReadIfd(first_);
  }
  ~TiffFile() { if (f_) fclose(f_); }
  TiffFile(const TiffFile&) = delete;
  TiffFile& operator=(const TiffFile&) = delete;

  bool Has(int tag) const { return tags_.count(tag) != 0; }
  std::vector<double> Nums(int tag) const {
    std::vector<double> out;
    auto it = tags_.find(tag);
    if (it == tags_.end()) return out;
    const Entry& e = it->second;
    const int sz = TypeSize(e.type);
    for (uint64_t i = 0; i < e.count && (i + 1) * uint64_t(sz) <= e.data.size(); ++i) out.push_back(Num(e.type, e.data.data() + i * uint64_t(sz)));
    return out;
  }
  double Num(int tag, double def) const { auto v = Nums(tag); return v.empty() ? def : v[0]; }
  std::string Text(int tag) const {
    auto it = tags_.find(tag);
    if (it == tags_.end()) return {};
    std::string s(it->second.data.begin(), it->second.data.end());
    while (!s.empty() && (s.back() == 0 || s.back() == ' ')) s.pop_back();
    return s;
  }
  bool LittleEndian() const { return le_; }
  const std::string& Path() const { return path_; }

  static std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(std::max(n, 1)), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(wcslen(w.c_str()));
    return w;
  }

 private:
  static int TypeSize(uint16_t t) {
    switch (t) { case 1: case 2: case 6: case 7: return 1; case 3: case 8: return 2; case 4: case 9: case 11: return 4;
                 case 5: case 10: case 12: case 16: case 17: case 18: return 8; default: return 1; }
  }
  uint16_t U16(const uint8_t* p) const { return le_ ? uint16_t(p[0] | p[1] << 8) : uint16_t(p[0] << 8 | p[1]); }
  uint32_t U32(const uint8_t* p) const {
    return le_ ? uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24
               : uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24;
  }
  uint64_t U64(const uint8_t* p) const {
    return le_ ? uint64_t(U32(p)) | uint64_t(U32(p + 4)) << 32 : uint64_t(U32(p)) << 32 | uint64_t(U32(p + 4));
  }
  double Num(uint16_t t, const uint8_t* p) const {
    switch (t) {
      case 1: case 7: return p[0];
      case 6: return int8_t(p[0]);
      case 3: return U16(p);
      case 8: return int16_t(U16(p));
      case 4: return U32(p);
      case 9: return int32_t(U32(p));
      case 5: { const double d = U32(p + 4); return d ? U32(p) / d : 0; }
      case 10: { const double d = int32_t(U32(p + 4)); return d ? int32_t(U32(p)) / d : 0; }
      case 11: { uint32_t u = U32(p); float f; memcpy(&f, &u, 4); return f; }
      case 12: { uint64_t u = U64(p); double d; memcpy(&d, &u, 8); return d; }
      case 16: case 18: return double(U64(p));
      case 17: return double(int64_t(U64(p)));
      default: return 0;
    }
  }
  void Read(uint64_t off, void* dst, size_t n) {
    if (_fseeki64(f_, int64_t(off), SEEK_SET) != 0 || fread(dst, 1, n, f_) != n) throw std::runtime_error("damaged TIFF (short read): " + path_);
  }
  void ReadIfd(uint64_t off) {
    uint8_t b[8];
    Read(off, b, big_ ? 8 : 2);
    const uint64_t n = big_ ? U64(b) : U16(b);
    if (n == 0 || n > 4096) throw std::runtime_error("damaged TIFF (directory): " + path_);
    const size_t es = big_ ? 20 : 12;
    std::vector<uint8_t> raw(size_t(n) * es);
    Read(off + (big_ ? 8 : 2), raw.data(), raw.size());
    for (uint64_t i = 0; i < n; ++i) {
      const uint8_t* p = raw.data() + i * es;
      Entry e;
      const int tag = U16(p);
      e.type = U16(p + 2);
      e.count = big_ ? U64(p + 4) : U32(p + 4);
      const uint64_t bytes = e.count * uint64_t(TypeSize(e.type));
      if (bytes > (uint64_t(1) << 31)) continue;
      e.data.resize(size_t(bytes));
      const size_t inl = big_ ? 8 : 4;
      if (bytes <= inl) memcpy(e.data.data(), p + (big_ ? 12 : 8), size_t(bytes));
      else Read(big_ ? U64(p + 12) : U32(p + 8), e.data.data(), size_t(bytes));
      tags_[tag] = std::move(e);
    }
  }

  std::string path_;
  FILE* f_ = nullptr;
  bool le_ = true, big_ = false;
  uint64_t first_ = 0;
  std::map<int, Entry> tags_;
};

// TIFF LZW (MSB-first codes, 9-12 bits, code width grows one code early).
std::vector<uint8_t> Lzw(const uint8_t* p, size_t n, size_t expect) {
  std::vector<uint8_t> out;
  out.reserve(expect);
  std::vector<int> prefix(4096, -1), len(4096, 1);
  std::vector<uint8_t> suffix(4096), first(4096);
  for (int i = 0; i < 256; ++i) { suffix[size_t(i)] = uint8_t(i); first[size_t(i)] = uint8_t(i); }
  int width = 9, next = 258, old = -1;
  uint32_t buf = 0; int bits = 0; size_t pos = 0;
  auto emit = [&](int code) {
    const size_t at = out.size(), l = size_t(len[size_t(code)]);
    out.resize(at + l);
    for (size_t k = l; k-- > 0;) { out[at + k] = suffix[size_t(code)]; code = prefix[size_t(code)]; }
  };
  while (out.size() < expect) {
    while (bits < width) { if (pos >= n) return out; buf = buf << 8 | p[pos++]; bits += 8; }
    const int code = int((buf >> (bits - width)) & ((1u << width) - 1));
    bits -= width;
    if (code == 257) break;
    if (code == 256) { width = 9; next = 258; old = -1; continue; }
    if (old < 0) { if (code > 255) break; emit(code); old = code; continue; }
    if (code < next) {
      emit(code);
      if (next < 4096) { prefix[size_t(next)] = old; suffix[size_t(next)] = first[size_t(code)]; first[size_t(next)] = first[size_t(old)]; len[size_t(next)] = len[size_t(old)] + 1; ++next; }
    } else if (code == next && next < 4096) {
      prefix[size_t(next)] = old; suffix[size_t(next)] = first[size_t(old)]; first[size_t(next)] = first[size_t(old)]; len[size_t(next)] = len[size_t(old)] + 1; ++next;
      emit(code);
    } else break;
    old = code;
    if (next >= (1 << width) - 1 && width < 12) ++width;
  }
  return out;
}

std::vector<uint8_t> PackBits(const uint8_t* p, size_t n, size_t expect) {
  std::vector<uint8_t> out;
  out.reserve(expect);
  for (size_t i = 0; i < n && out.size() < expect;) {
    const int c = int8_t(p[i++]);
    if (c >= 0) { const size_t k = std::min(size_t(c) + 1, n - i); out.insert(out.end(), p + i, p + i + k); i += k; }
    else if (c != -128 && i < n) { out.insert(out.end(), size_t(1 - c), p[i]); ++i; }
  }
  return out;
}

// ------------------------------------------------------------------ the grid
}  // namespace

struct Grid {
  std::string path;
  int w = 0, h = 0;                           // cells
  std::vector<std::vector<float>> levels;     // [0] the grid; each next level half the size (mean of valid cells)
  std::vector<int> lw, lh;
  Projection proj;
  double a = 1, b = 0, c0 = 0, d = 0, e = -1, f0 = 0;   // map = (c0 + a*col + b*row, f0 + d*col + e*row), pixel corners
  double ia = 1, ib = 0, id = 0, ie = 1;
  src::LonLatBox box{};
  double centreLat = 0, cellMetres = 1;       // ground size of one cell (level 0)
  float minDepth = 0, maxDepth = 0;           // valid data range (m, positive down)
  std::vector<float> sample;                  // up to 1M depths, sorted (for the automatic range)

  void PixelToMap(double col, double row, double& x, double& y) const { x = c0 + a * col + b * row; y = f0 + d * col + e * row; }
  void MapToPixel(double x, double y, double& col, double& row) const {
    const double dx = x - c0, dy = y - f0;
    col = ia * dx + ib * dy; row = id * dx + ie * dy;
  }
  float At(int L, int x, int y) const { return levels[size_t(L)][size_t(y) * size_t(lw[size_t(L)]) + size_t(x)]; }
  // Bilinear depth at continuous cell-centre coordinates of level L; NaN where mostly missing.
  float Sample(int L, double cx, double cy) const {
    const int W = lw[size_t(L)], H = lh[size_t(L)];
    if (cx < -0.5 || cy < -0.5 || cx > W - 0.5 || cy > H - 0.5) return kMissing;
    const double x0f = std::floor(cx), y0f = std::floor(cy);
    const double fx = cx - x0f, fy = cy - y0f;
    const int x0 = int(x0f), y0 = int(y0f);
    double sum = 0, wsum = 0;
    for (int k = 0; k < 4; ++k) {
      const int x = std::clamp(x0 + (k & 1), 0, W - 1), y = std::clamp(y0 + (k >> 1), 0, H - 1);
      const double wt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
      const float v = At(L, x, y);
      if (Valid(v)) { sum += v * wt; wsum += wt; }
    }
    return wsum >= 0.5 ? float(sum / wsum) : kMissing;
  }
};

namespace {

void BuildLevels(Grid& g) {
  g.lw = {g.w}; g.lh = {g.h};
  while (g.lw.back() > 64 || g.lh.back() > 64) {
    const auto& s = g.levels.back();
    const int W = g.lw.back(), H = g.lh.back(), w2 = (W + 1) / 2, h2 = (H + 1) / 2;
    std::vector<float> o(size_t(w2) * size_t(h2), kMissing);
    for (int y = 0; y < h2; ++y)
      for (int x = 0; x < w2; ++x) {
        double sum = 0; int n = 0;
        for (int k = 0; k < 4; ++k) {
          const int sx = 2 * x + (k & 1), sy = 2 * y + (k >> 1);
          if (sx >= W || sy >= H) continue;
          const float v = s[size_t(sy) * size_t(W) + size_t(sx)];
          if (Valid(v)) { sum += v; ++n; }
        }
        if (n) o[size_t(y) * size_t(w2) + size_t(x)] = float(sum / n);
      }
    g.levels.push_back(std::move(o));
    g.lw.push_back(w2); g.lh.push_back(h2);
  }
}

}  // namespace

bool IsDepthTiff(const std::string& path) {
  try {
    TiffFile t(path);
    const int spp = int(t.Num(277, 1)), bps = int(t.Num(258, 1)), fmt = int(t.Num(339, 1));
    const int photometric = int(t.Num(262, 1));
    if (spp != 1 || photometric == 3) return false;   // colour or palette picture
    return fmt == 3 || bps >= 16;                       // floats, or 16/32-bit numbers (an 8-bit grey picture stays a picture)
  } catch (...) {
    return false;
  }
}

std::shared_ptr<Grid> Load(const std::string& path, const Options& o) {
  TiffFile t(path);
  auto g = std::make_shared<Grid>();
  g->path = path;
  const int W = int(t.Num(256, 0)), H = int(t.Num(257, 0));
  const int spp = int(t.Num(277, 1)), bps = int(t.Num(258, 1)), fmt = int(t.Num(339, 1));
  const int comp = int(t.Num(259, 1)), pred = int(t.Num(317, 1));
  if (W <= 0 || H <= 0) throw std::runtime_error("damaged TIFF (no size): " + path);
  if (spp != 1) throw std::runtime_error("this GeoTIFF has " + std::to_string(spp) + " bands; a depth file has one");
  if (!((fmt == 3 && (bps == 32 || bps == 64)) || ((fmt == 1 || fmt == 2) && (bps == 8 || bps == 16 || bps == 32))))
    throw std::runtime_error("unsupported number format in " + path + " (" + std::to_string(bps) + "-bit, format " + std::to_string(fmt) + ")");
  if (comp != 1 && comp != 5 && comp != 8 && comp != 32946 && comp != 32773)
    throw std::runtime_error("this GeoTIFF uses compression " + std::to_string(comp) +
                             ", which AT5 Chart Maker can't read.\n  Re-save it with GDAL: gdal_translate -co COMPRESS=DEFLATE in.tif out.tif");
  const int bytes = bps / 8;

  // georeferencing
  auto scale = t.Nums(33550), tie = t.Nums(33922), mt = t.Nums(34264);
  if (mt.size() >= 16) { g->a = mt[0]; g->b = mt[1]; g->c0 = mt[3]; g->d = mt[4]; g->e = mt[5]; g->f0 = mt[7]; }
  else if (scale.size() >= 2 && tie.size() >= 6) {
    g->a = scale[0]; g->e = -scale[1]; g->b = g->d = 0;
    g->c0 = tie[3] - tie[0] * g->a; g->f0 = tie[4] - tie[1] * g->e;
  } else throw std::runtime_error("no georeferencing in " + path + " (it needs GeoTIFF position tags)");
  int epsgProj = 0, epsgGeo = 0, model = 0;
  bool pixelIsPoint = false;
  {
    auto k = t.Nums(34735);
    for (size_t i = 4; i + 3 < k.size(); i += 4) {
      const int id = int(k[i]), loc = int(k[i + 1]), val = int(k[i + 3]);
      if (loc != 0) continue;
      if (id == 1024) model = val;
      if (id == 1025 && val == 2) pixelIsPoint = true;
      if (id == 2048) epsgGeo = val;
      if (id == 3072) epsgProj = val;
    }
  }
  if (pixelIsPoint) { g->c0 -= (g->a + g->b) / 2; g->f0 -= (g->d + g->e) / 2; }
  int epsg = o.crs ? o.crs : epsgProj && epsgProj != 32767 ? epsgProj : 0;
  if (!epsg && (model == 2 || (!model && epsgGeo))) epsg = epsgGeo && epsgGeo != 32767 ? epsgGeo : 4326;
  if (!epsg && !model && std::fabs(g->c0) <= 360 && std::fabs(g->f0) <= 90) epsg = 4326;
  if (!epsg || !g->proj.FromEpsg(epsg))
    throw std::runtime_error("this GeoTIFF's map projection" + (epsg ? " (EPSG:" + std::to_string(epsg) + ")" : std::string()) +
                             " isn't supported.\n  Supported: longitude/latitude, Web Mercator, UTM and MGA zones, Australian Albers.\n"
                             "  Re-project it with GDAL: gdalwarp -t_srs EPSG:4326 in.tif out.tif");
  const double det = g->a * g->e - g->b * g->d;
  if (std::fabs(det) < 1e-30) throw std::runtime_error("damaged georeferencing in " + path);
  g->ia = g->e / det; g->ib = -g->b / det; g->id = -g->d / det; g->ie = g->a / det;

  // nodata
  double nodata = std::nan("");
  { const std::string s = t.Text(42113); if (!s.empty()) { try { nodata = std::stod(s); } catch (...) {} } }

  // reduce very large grids
  int k = 1;
  while (size_t(W / k + 1) * size_t(H / k + 1) > kMaxCells) ++k;
  g->w = (W + k - 1) / k; g->h = (H + k - 1) / k;
  if (k > 1) {
    printf("  large survey (%d x %d cells): read every %d%s cell (%d x %d)\n", W, H, k, k == 2 ? "nd" : k == 3 ? "rd" : "th", g->w, g->h);
    g->a *= k; g->b *= k; g->d *= k; g->e *= k;
    const double det2 = g->a * g->e - g->b * g->d;
    g->ia = g->e / det2; g->ib = -g->b / det2; g->id = -g->d / det2; g->ie = g->a / det2;
  }
  std::vector<float> grid(size_t(g->w) * size_t(g->h), kMissing);

  // blocks: tiles or strips
  const bool tiled = t.Has(322);
  const int bw = tiled ? int(t.Num(322, 256)) : W;
  const int bh = tiled ? int(t.Num(323, 256)) : std::min(H, int(t.Num(278, H)));
  auto offs = t.Nums(tiled ? 324 : 273), counts = t.Nums(tiled ? 325 : 279);
  const int across = (W + bw - 1) / bw, down = (H + bh - 1) / bh;
  if (bw <= 0 || bh <= 0 || offs.size() < size_t(across) * size_t(down) || counts.size() < offs.size())
    throw std::runtime_error("damaged TIFF (block table): " + path);
  const bool le = t.LittleEndian();
  // integer grids without a usable no-data value mark empty cells with the type's extreme value
  const double intEmpty = fmt == 3 ? std::nan("") : fmt == 2 ? -std::ldexp(1.0, bps - 1) : std::ldexp(1.0, bps) - 1;
  if (fmt != 3 && nodata == nodata) {   // a no-data value the integer type can't hold is stored clamped (as GDAL does)
    const double lo = fmt == 2 ? -std::ldexp(1.0, bps - 1) : 0, hi = fmt == 2 ? std::ldexp(1.0, bps - 1) - 1 : std::ldexp(1.0, bps) - 1;
    nodata = std::clamp(std::round(nodata), lo, hi);
  }
  std::atomic<int> nextBlock{0}, bad{0};
  auto worker = [&]() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, TiffFile::Widen(path).c_str(), L"rb") != 0) f = nullptr;
    if (!f) { ++bad; return; }
    std::vector<uint8_t> in;
    for (;;) {
      const int bi = nextBlock++;
      if (bi >= across * down) break;
      const int bx = bi % across, by = bi / across;
      const int rows = tiled ? bh : std::min(bh, H - by * bh);
      const size_t expect = size_t(bw) * size_t(rows) * size_t(bytes);
      in.resize(size_t(counts[size_t(bi)]));
      if (in.empty()) continue;   // sparse block: no data
      if (_fseeki64(f, int64_t(offs[size_t(bi)]), SEEK_SET) != 0 || fread(in.data(), 1, in.size(), f) != in.size()) { ++bad; continue; }
      std::vector<uint8_t> raw;
      try {
        if (comp == 1) raw = in;
        else if (comp == 5) raw = Lzw(in.data(), in.size(), expect);
        else if (comp == 32773) raw = PackBits(in.data(), in.size(), expect);
        else raw = inflate::Auto(in.data(), in.size());
      } catch (...) { ++bad; continue; }
      if (raw.size() < expect) { ++bad; continue; }
      std::vector<double> vals(size_t(bw) * size_t(rows));
      for (int r = 0; r < rows; ++r) {
        uint8_t* row = raw.data() + size_t(r) * size_t(bw) * size_t(bytes);
        if (pred == 3) {   // floating-point predictor: byte differences, then byte planes most significant first
          const size_t n = size_t(bw) * size_t(bytes);
          for (size_t i = 1; i < n; ++i) row[i] = uint8_t(row[i] + row[i - 1]);
          std::vector<uint8_t> tmp(row, row + n);
          for (int x = 0; x < bw; ++x)
            for (int b = 0; b < bytes; ++b) row[size_t(x) * size_t(bytes) + size_t(le ? bytes - 1 - b : b)] = tmp[size_t(b) * size_t(bw) + size_t(x)];
        }
        // Predictor 2 on floating-point data (GDAL writes this): the raw 32/64-bit words are differenced as
        // integers, so they are summed back before being read as floats.
        uint64_t acc = 0;
        const uint64_t wordMask = bytes == 8 ? ~0ull : (1ull << (8 * bytes)) - 1;
        for (int x = 0; x < bw; ++x) {
          const uint8_t* p = row + size_t(x) * size_t(bytes);
          uint64_t u = 0;
          for (int b = 0; b < bytes; ++b) u |= uint64_t(p[le ? b : bytes - 1 - b]) << (8 * b);
          if (pred == 2 && fmt == 3) { acc = (x ? acc + u : u) & wordMask; u = acc; }
          double v;
          if (fmt == 3) { if (bytes == 4) { uint32_t u32 = uint32_t(u); float fl; memcpy(&fl, &u32, 4); v = fl; } else { memcpy(&v, &u, 8); } }
          else if (fmt == 2) v = bytes == 1 ? double(int8_t(u)) : bytes == 2 ? double(int16_t(u)) : double(int32_t(u));
          else v = double(u);
          vals[size_t(r) * size_t(bw) + size_t(x)] = v;
        }
        if (pred == 2 && fmt != 3) {   // horizontal differencing (integers)
          const uint64_t mask = bytes == 8 ? ~0ull : (1ull << (8 * bytes)) - 1;
          for (int x = 1; x < bw; ++x) {
            double& cur = vals[size_t(r) * size_t(bw) + size_t(x)];
            const double prev = vals[size_t(r) * size_t(bw) + size_t(x - 1)];
            const uint64_t s = (uint64_t(int64_t(cur)) + uint64_t(int64_t(prev))) & mask;
            cur = fmt == 2 ? (bytes == 1 ? double(int8_t(s)) : bytes == 2 ? double(int16_t(s)) : double(int32_t(s))) : double(s);
          }
        }
      }
      for (int r = 0; r < rows; ++r) {
        const int y = by * bh + r;
        if (y >= H || y % k) continue;
        for (int x0 = 0; x0 < bw; ++x0) {
          const int x = bx * bw + x0;
          if (x >= W || x % k) continue;
          const double v = vals[size_t(r) * size_t(bw) + size_t(x0)];
          if (!std::isfinite(v) || std::fabs(v) > 1e30 || (nodata == nodata && v == nodata) || v == intEmpty) continue;
          grid[size_t(y / k) * size_t(g->w) + size_t(x / k)] = float(v);
        }
      }
    }
    fclose(f);
  };
  {
    const int n = int(std::max(1u, std::min(16u, std::thread::hardware_concurrency())));
    std::vector<std::thread> ts;
    for (int i = 0; i < n; ++i) ts.emplace_back(worker);
    for (auto& th : ts) th.join();
  }
  if (bad > 0) printf("  WARNING: %d block(s) of the file could not be read and are left empty\n", bad.load());

  // heights or depths?
  size_t valid = 0, negative = 0;
  for (float v : grid) if (Valid(v)) { ++valid; if (v < 0) ++negative; }
  if (!valid) throw std::runtime_error("the GeoTIFF has no depth values (all no-data)");
  const bool heights = o.elevation == 1 || (o.elevation < 0 && negative * 2 > valid);
  g->minDepth = 1e30f; g->maxDepth = -1e30f;
  const size_t step = std::max<size_t>(1, valid / 1000000);
  size_t seen = 0;
  for (float& v : grid)
    if (Valid(v)) {
      if (heights) v = -v;
      g->minDepth = std::min(g->minDepth, v); g->maxDepth = std::max(g->maxDepth, v);
      if (seen++ % step == 0) g->sample.push_back(v);
    }
  std::sort(g->sample.begin(), g->sample.end());
  g->levels.push_back(std::move(grid));
  BuildLevels(*g);

  // extent and cell size
  double lo0 = 1e9, la0 = 1e9, lo1 = -1e9, la1 = -1e9;
  for (int i = 0; i <= 8; ++i)
    for (int j = 0; j <= 8; ++j) {
      double x, y, lon, lat;
      g->PixelToMap(g->w * i / 8.0, g->h * j / 8.0, x, y);
      g->proj.ToLonLat(x, y, lon, lat);
      lo0 = std::min(lo0, lon); lo1 = std::max(lo1, lon); la0 = std::min(la0, lat); la1 = std::max(la1, lat);
    }
  g->box = {lo0, std::max(la0, -85.0), lo1, std::min(la1, 85.0)};
  g->centreLat = (la0 + la1) / 2;
  double ex, ny;
  g->proj.UnitMetres(g->centreLat, ex, ny);
  g->cellMetres = std::sqrt(std::hypot(g->a, g->d) * ex * std::hypot(g->b, g->e) * ny);
  printf("  depth grid %d x %d cells, about %.1f m each, %s\n  %s, depths %.1f .. %.1f m\n", g->w, g->h, g->cellMetres,
         g->proj.Name().c_str(), heights ? "values are heights (negative = below the water)" : "values are depths",
         double(g->minDepth), double(g->maxDepth));
  return g;
}

double AutoRangeMax(const Grid& g) {
  if (g.sample.empty()) return 10;
  const float p98 = g.sample[size_t(double(g.sample.size() - 1) * 0.98)];
  const double d = std::max(1.0, double(p98));
  // a round number at or above it: 1, 1.5, 2, 2.5, 3, 4, 5, 6, 8 x 10^n
  const double base = std::pow(10, std::floor(std::log10(d)));
  for (double m : {1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0}) if (m * base >= d) return m * base;
  return 10 * base;
}

namespace {

// ------------------------------------------------------------------ imagery: colour + hillshade
struct Palette { const char* name; std::vector<uint32_t> stops; };
const std::vector<Palette>& Palettes() {
  static const std::vector<Palette> p = {   // shallow -> deep (the same palettes as the SeaBathy apps' website)
    {"spectrum", {0xc8102e, 0xf0582a, 0xf8b62b, 0xf4e04d, 0x5fd38d, 0x19b6c9, 0x1f6fd1, 0x20307a, 0x0e1440}},
    {"lagoon", {0xf2fbf6, 0xa8eedc, 0x4fd1c5, 0x14a3a8, 0x0e7490, 0x0b4f72, 0x0a3352, 0x071d33, 0x050f1f}},
    {"thermal", {0xfff5b8, 0xffd17a, 0xff9f5a, 0xf2645c, 0xd23c78, 0x9c2a8c, 0x5f2186, 0x2e155f, 0x120a2b}},
    {"chart", {0x9fd8f0, 0x7fc0e4, 0x5aa6d4, 0x7db8e0, 0xa3cdea, 0xc4e0f2, 0xdfeefa, 0xeff7fd, 0xfbfdff}},
    {"grey", {0xf2f2f2, 0x0d0d0d}},
  };
  return p;
}
constexpr uint32_t kAboveWater = 0xd9c58f;   // sand

class DepthImagery;

class DepthSampler : public src::Sampler {
 public:
  explicit DepthSampler(const DepthImagery* s) : s_(s) {}
  bool RenderTile(double mpp, int64_t tx, int64_t ty, uint8_t* rgba) override;
 private:
  const DepthImagery* s_;
};

class DepthImagery : public src::Source {
 public:
  DepthImagery(std::shared_ptr<Grid> g, const Options& o) : g_(std::move(g)), o_(o) {
    const Palette* pal = nullptr;
    for (auto& p : Palettes()) if (o.palette == p.name) pal = &p;
    if (!pal) {
      std::string names;
      for (auto& p : Palettes()) names += std::string(names.empty() ? "" : ", ") + p.name;
      throw std::runtime_error("unknown palette '" + o.palette + "' (choose " + names + ")");
    }
    for (int i = 0; i < 256; ++i) {
      const double f = i / 255.0 * double(pal->stops.size() - 1);
      const size_t k = std::min(size_t(f), pal->stops.size() - 2);
      const double w = f - double(k);
      for (int c = 0; c < 3; ++c) {
        const double a = (pal->stops[k] >> (16 - 8 * c)) & 255, b = (pal->stops[k + 1] >> (16 - 8 * c)) & 255;
        lut_[size_t(i)][size_t(c)] = uint8_t(std::lround(a + (b - a) * w));
      }
    }
    lo_ = o.rangeMin;
    hi_ = o.rangeMax > o.rangeMin ? o.rangeMax : std::max(lo_ + 1, AutoRangeMax(*g_));
    printf("  colours: %s, %g .. %g m (deeper shows in the deepest colour), hillshade %d%%\n", pal->name, lo_, hi_, int(std::lround(o.hillshade * 100)));
    // native resolution in Lowrance-Mercator metres
    native_ = g_->cellMetres / std::cos(g_->centreLat * kDeg) * kRl / kA;
  }
  src::LonLatBox Bounds() const override { return g_->box; }
  double NativeMpp() const override { return native_; }
  std::string Describe() const override { return "depth GeoTIFF " + g_->path; }
  std::unique_ptr<src::Sampler> MakeSampler() override { return std::make_unique<DepthSampler>(this); }

  std::shared_ptr<Grid> g_;
  Options o_;
  uint8_t lut_[256][3]{};
  double lo_ = 0, hi_ = 10, native_ = 1;
};

bool DepthSampler::RenderTile(double mpp, int64_t tx, int64_t ty, uint8_t* rgba) {
  const DepthImagery& s = *s_;
  const Grid& g = *s.g_;
  // grid level: about one cell per output pixel
  const int L = std::clamp(int(std::floor(std::log2(std::max(1.0, mpp / s.native_)) + 1e-9)), 0, int(g.levels.size()) - 1);
  const double f = 1.0 / double(1 << L);
  // ground size of one level-L cell, for slopes
  const double latMid = (2 * std::atan(std::exp((ty * 256 + 128) * mpp / kRl)) - kPi / 2) / kDeg;
  double ex, ny;
  g.proj.UnitMetres(latMid, ex, ny);
  const double cellX = std::hypot(g.a, g.d) * ex * double(1 << L), cellY = std::hypot(g.b, g.e) * ny * double(1 << L);
  const double z = s.o_.relief, amount = std::clamp(s.o_.hillshade, 0.0, 1.0);
  // light from the north-west, 45 degrees up
  const double lx = -0.5, ly = 0.5, lz = std::sqrt(0.5);
  std::vector<double> lon(256), lat(256);
  for (int i = 0; i < 256; ++i) lon[size_t(i)] = (tx * 256 + i + 0.5) * mpp / kRl / kDeg;
  for (int j = 0; j < 256; ++j) lat[size_t(j)] = (2 * std::atan(std::exp((ty * 256 + j + 0.5) * mpp / kRl)) - kPi / 2) / kDeg;
  bool got = false;
  for (int j = 0; j < 256; ++j) {
    uint8_t* row = rgba + size_t(j) * 256 * 4;
    for (int i = 0; i < 256; ++i) {
      uint8_t* px = row + size_t(i) * 4;
      double x, y, col, r;
      g.proj.FromLonLat(lon[size_t(i)], lat[size_t(j)], x, y);
      g.MapToPixel(x, y, col, r);
      const double cx = col * f - 0.5, cy = r * f - 0.5;
      const float d = g.Sample(L, cx, cy);
      if (!Valid(d)) { px[3] = 0; continue; }
      uint8_t base[3];
      if (d < 0 && s.lo_ >= 0) {
        if (!s.o_.showAboveWater) { px[3] = 0; continue; }
        base[0] = uint8_t(kAboveWater >> 16); base[1] = uint8_t(kAboveWater >> 8); base[2] = uint8_t(kAboveWater);
      } else {
        const double t = std::clamp((double(d) - s.lo_) / (s.hi_ - s.lo_), 0.0, 1.0);
        const auto& c = s.lut_[size_t(std::lround(t * 255))];
        base[0] = c[0]; base[1] = c[1]; base[2] = c[2];
      }
      double shade = 1;
      const auto& lines = s.o_.pictureContours;
      if (amount > 0 || !lines.empty()) {
        // slope of the seabed height (= -depth): x east, y north (rows run south)
        const float de = g.Sample(L, cx + 1, cy), dw = g.Sample(L, cx - 1, cy), dn = g.Sample(L, cx, cy - 1), ds = g.Sample(L, cx, cy + 1);
        const double hx = (Valid(de) && Valid(dw)) ? -(double(de) - double(dw)) / (2 * cellX) : 0;
        const double hy = (Valid(dn) && Valid(ds)) ? -(double(dn) - double(ds)) / (2 * cellY) : 0;
        if (amount > 0) {
          const double nx = -z * hx, nyy = -z * hy, len = std::sqrt(nx * nx + nyy * nyy + 1);
          const double lit = std::max(0.0, (nx * lx + nyy * ly + lz) / len) / lz;   // 1 on flat ground
          shade = 1 - amount + amount * std::min(lit, 1.6);
        }
        if (!lines.empty() && d >= 0) {
          // Contour lines in the picture: a pixel lies on a line when the nearest contour depth is within about
          // half a pixel's worth of depth change. Lines closer together than a few pixels are left out.
          const double groundPerPixel = mpp * std::cos(lat[size_t(j)] * kDeg) * kA / kRl;
          const double perPixel = std::hypot(hx, hy) * groundPerPixel;        // depth change across one pixel
          auto it = std::lower_bound(lines.begin(), lines.end(), double(d));
          double best = 1e30, gap = 1e30;
          for (auto k = it == lines.begin() ? it : it - 1; k != lines.end() && k <= it; ++k) {
            const double dist = std::fabs(*k - double(d));
            if (dist < best) {
              best = dist;
              const double lo = k == lines.begin() ? 1e30 : *k - *(k - 1), hi = k + 1 == lines.end() ? 1e30 : *(k + 1) - *k;
              gap = std::min(lo, hi);
            }
          }
          if (perPixel > 1e-6 && gap > 4 * perPixel && best < 0.6 * perPixel) shade *= 0.45;
        }
      }
      for (int c = 0; c < 3; ++c) px[c] = uint8_t(std::clamp(std::lround(base[c] * shade), 0L, 255L));
      px[3] = 255;
      got = true;
    }
  }
  return got;
}

// ------------------------------------------------------------------ contours
struct Line { double depth; std::vector<std::pair<double, double>> pts; };   // pts in global tile units (see WriteContours)

// Light 3x3 smoothing (missing cells stay missing), so lines follow the seabed, not the survey's noise.
std::vector<float> Smoothed(const std::vector<float>& s, int W, int H) {
  std::vector<float> o(s.size(), kMissing);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const float c = s[size_t(y) * size_t(W) + size_t(x)];
      if (!Valid(c)) continue;
      double sum = 0, wsum = 0;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          const int xx = x + dx, yy = y + dy;
          if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
          const float v = s[size_t(yy) * size_t(W) + size_t(xx)];
          if (!Valid(v)) continue;
          const double w = (dx ? 1 : 2) * (dy ? 1 : 2);
          sum += v * w; wsum += w;
        }
      o[size_t(y) * size_t(W) + size_t(x)] = float(sum / wsum);
    }
  return o;
}

// Marching squares at one level; segments are joined into lines through the cell edges they share.
// Points are in cell-centre coordinates of the grid.
void Trace(const std::vector<float>& s, int W, int H, double v, std::vector<std::vector<std::pair<double, double>>>& out) {
  auto edgeId = [W](int x, int y, int vertical) { return (uint64_t(uint32_t(y)) * uint64_t(W) + uint64_t(uint32_t(x))) * 2 + uint64_t(vertical); };
  std::unordered_map<uint64_t, std::pair<double, double>> point;
  std::vector<std::pair<uint64_t, uint64_t>> segs;
  auto at = [&](int x, int y) { return s[size_t(y) * size_t(W) + size_t(x)]; };
  auto edgePoint = [&](int x, int y, int vertical) {
    const uint64_t id = edgeId(x, y, vertical);
    if (!point.count(id)) {
      const double a = at(x, y), b = vertical ? at(x, y + 1) : at(x + 1, y);
      const double t = std::clamp((v - a) / (b - a), 0.0, 1.0);
      point[id] = vertical ? std::make_pair(double(x), y + t) : std::make_pair(x + t, double(y));
    }
    return id;
  };
  for (int y = 0; y + 1 < H; ++y)
    for (int x = 0; x + 1 < W; ++x) {
      const float c0 = at(x, y), c1 = at(x + 1, y), c2 = at(x + 1, y + 1), c3 = at(x, y + 1);
      if (!Valid(c0) || !Valid(c1) || !Valid(c2) || !Valid(c3)) continue;
      const int idx = (c0 > v) | (c1 > v) << 1 | (c2 > v) << 2 | (c3 > v) << 3;
      if (idx == 0 || idx == 15) continue;
      // edges: 0 top (x,y)-(x+1,y), 1 right (x+1,y)-(x+1,y+1), 2 bottom (x,y+1)-(x+1,y+1), 3 left (x,y)-(x,y+1)
      auto E = [&](int e) { return e == 0 ? edgePoint(x, y, 0) : e == 1 ? edgePoint(x + 1, y, 1) : e == 2 ? edgePoint(x, y + 1, 0) : edgePoint(x, y, 1); };
      auto seg = [&](int a, int b) { segs.push_back({E(a), E(b)}); };
      const bool centreHigh = (double(c0) + c1 + c2 + c3) / 4 > v;
      switch (idx) {
        case 1: case 14: seg(3, 0); break;
        case 2: case 13: seg(0, 1); break;
        case 3: case 12: seg(3, 1); break;
        case 4: case 11: seg(1, 2); break;
        case 6: case 9: seg(0, 2); break;
        case 7: case 8: seg(3, 2); break;
        case 5: if (centreHigh) { seg(3, 2); seg(0, 1); } else { seg(3, 0); seg(1, 2); } break;
        case 10: if (centreHigh) { seg(3, 0); seg(1, 2); } else { seg(3, 2); seg(0, 1); } break;
      }
    }
  // join
  std::unordered_map<uint64_t, std::vector<size_t>> ends;
  ends.reserve(segs.size() * 2);
  for (size_t i = 0; i < segs.size(); ++i) { ends[segs[i].first].push_back(i); ends[segs[i].second].push_back(i); }
  std::vector<char> used(segs.size(), 0);
  auto follow = [&](size_t start, uint64_t from) {
    std::vector<uint64_t> ids{from};
    size_t cur = start;
    uint64_t at2 = from;
    for (;;) {
      used[cur] = 1;
      const uint64_t nxt = segs[cur].first == at2 ? segs[cur].second : segs[cur].first;
      ids.push_back(nxt);
      at2 = nxt;
      size_t found = SIZE_MAX;
      for (size_t c : ends[nxt]) if (!used[c]) { found = c; break; }
      if (found == SIZE_MAX) break;
      cur = found;
    }
    std::vector<std::pair<double, double>> pts;
    pts.reserve(ids.size());
    for (uint64_t id : ids) pts.push_back(point[id]);
    out.push_back(std::move(pts));
  };
  for (size_t i = 0; i < segs.size(); ++i)   // open lines first, from an end
    if (!used[i]) {
      if (ends[segs[i].first].size() == 1) follow(i, segs[i].first);
      else if (ends[segs[i].second].size() == 1) follow(i, segs[i].second);
    }
  for (size_t i = 0; i < segs.size(); ++i) if (!used[i]) follow(i, segs[i].first);   // closed rings
}

void Simplify(std::vector<std::pair<double, double>>& p, double tol) {   // Douglas-Peucker
  if (p.size() < 3) return;
  std::vector<char> keep(p.size(), 0);
  keep.front() = keep.back() = 1;
  std::vector<std::pair<size_t, size_t>> stack{{0, p.size() - 1}};
  while (!stack.empty()) {
    auto [a, b] = stack.back(); stack.pop_back();
    if (b <= a + 1) continue;
    const double ax = p[a].first, ay = p[a].second, bx = p[b].first, by = p[b].second;
    const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    double best = -1; size_t bi = a;
    for (size_t i = a + 1; i < b; ++i) {
      double d2;
      if (l2 == 0) d2 = (p[i].first - ax) * (p[i].first - ax) + (p[i].second - ay) * (p[i].second - ay);
      else { const double cr = (p[i].first - ax) * dy - (p[i].second - ay) * dx; d2 = cr * cr / l2; }
      if (d2 > best) { best = d2; bi = i; }
    }
    if (best > tol * tol) { keep[bi] = 1; stack.push_back({a, bi}); stack.push_back({bi, b}); }
  }
  std::vector<std::pair<double, double>> o;
  for (size_t i = 0; i < p.size(); ++i) if (keep[i]) o.push_back(p[i]);
  p.swap(o);
}

// Contour lines of one grid level, in global tile units at zoom z (4096 per tile, y down from the north).
std::vector<Line> LinesAt(const Grid& g, int L, int z, const std::vector<double>& levels) {
  const int W = g.lw[size_t(L)], H = g.lh[size_t(L)];
  const auto s = Smoothed(g.levels[size_t(L)], W, H);
  const double world = 4096.0 * double(int64_t(1) << z), f = double(1 << L);
  std::vector<Line> out;
  for (double v : levels) {
    std::vector<std::vector<std::pair<double, double>>> raw;
    Trace(s, W, H, v, raw);
    for (auto& r : raw) {
      Line ln{v, {}};
      ln.pts.reserve(r.size());
      for (auto& p : r) {
        double x, y, lon, lat;
        g.PixelToMap((p.first + 0.5) * f, (p.second + 0.5) * f, x, y);
        g.proj.ToLonLat(x, y, lon, lat);
        lat = std::clamp(lat, -85.0, 85.0);
        const double gx = (lon + 180) / 360 * world;
        const double gy = (1 - std::log(std::tan(kPi / 4 + lat * kDeg / 2)) / kPi) / 2 * world;
        ln.pts.push_back({gx, gy});
      }
      Simplify(ln.pts, 0.75);
      if (ln.pts.size() >= 2) out.push_back(std::move(ln));
    }
  }
  return out;
}

// ---- MVT encoding
void Varint(std::vector<uint8_t>& b, uint64_t v) { while (v >= 0x80) { b.push_back(uint8_t(v | 0x80)); v >>= 7; } b.push_back(uint8_t(v)); }
void Key(std::vector<uint8_t>& b, int field, int wire) { Varint(b, uint64_t(field) << 3 | uint64_t(wire)); }
void Bytes(std::vector<uint8_t>& b, int field, const std::vector<uint8_t>& v) { Key(b, field, 2); Varint(b, v.size()); b.insert(b.end(), v.begin(), v.end()); }
uint32_t Zig(int32_t v) { return uint32_t((v << 1) ^ (v >> 31)); }

struct TileLines { std::vector<std::pair<double, std::vector<std::pair<int32_t, int32_t>>>> lines; };

// Clip a polyline to [x0,x1]x[y0,y1] (Liang-Barsky per segment), appending the pieces.
void ClipTo(const std::vector<std::pair<double, double>>& p, double x0, double y0, double x1, double y1,
            std::vector<std::vector<std::pair<double, double>>>& pieces) {
  std::vector<std::pair<double, double>> cur;
  auto flush = [&]() { if (cur.size() >= 2) pieces.push_back(cur); cur.clear(); };
  for (size_t i = 0; i + 1 < p.size(); ++i) {
    double ax = p[i].first, ay = p[i].second, bx = p[i + 1].first, by = p[i + 1].second;
    double t0 = 0, t1 = 1;
    const double dx = bx - ax, dy = by - ay;
    bool inside = true;
    auto clip = [&](double pp, double q) {
      if (pp == 0) { if (q < 0) inside = false; return; }
      const double r = q / pp;
      if (pp < 0) { if (r > t1) inside = false; else if (r > t0) t0 = r; }
      else { if (r < t0) inside = false; else if (r < t1) t1 = r; }
    };
    clip(-dx, ax - x0); clip(dx, x1 - ax); clip(-dy, ay - y0); clip(dy, y1 - ay);
    if (!inside) { flush(); continue; }
    const std::pair<double, double> s{ax + t0 * dx, ay + t0 * dy}, e{ax + t1 * dx, ay + t1 * dy};
    if (cur.empty() || t0 > 0) { flush(); cur.push_back(s); }
    cur.push_back(e);
    if (t1 < 1) flush();
  }
  flush();
}

void AddToTiles(const std::vector<Line>& lines, std::map<std::pair<int64_t, int64_t>, TileLines>& tiles) {
  constexpr double kBuf = 16;   // a little overlap so lines join across tile edges
  for (const auto& ln : lines) {
    // in pieces of up to 200 points, so each piece spans few tiles
    for (size_t s = 0; s + 1 < ln.pts.size(); s += 199) {
      const size_t e = std::min(ln.pts.size(), s + 200);
      std::vector<std::pair<double, double>> part(ln.pts.begin() + long(s), ln.pts.begin() + long(e));
      double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
      for (auto& q : part) { minx = std::min(minx, q.first); maxx = std::max(maxx, q.first); miny = std::min(miny, q.second); maxy = std::max(maxy, q.second); }
      for (int64_t ty = int64_t(std::floor((miny - kBuf) / 4096)); ty <= int64_t(std::floor((maxy + kBuf) / 4096)); ++ty)
        for (int64_t tx = int64_t(std::floor((minx - kBuf) / 4096)); tx <= int64_t(std::floor((maxx + kBuf) / 4096)); ++tx) {
          std::vector<std::vector<std::pair<double, double>>> pieces;
          ClipTo(part, tx * 4096.0 - kBuf, ty * 4096.0 - kBuf, (tx + 1) * 4096.0 + kBuf, (ty + 1) * 4096.0 + kBuf, pieces);
          for (auto& pc : pieces) {
            std::vector<std::pair<int32_t, int32_t>> q;
            for (auto& pt : pc) {
              const std::pair<int32_t, int32_t> v{int32_t(std::lround(pt.first - tx * 4096.0)), int32_t(std::lround(pt.second - ty * 4096.0))};
              if (q.empty() || q.back() != v) q.push_back(v);
            }
            if (q.size() >= 2) tiles[{tx, ty}].lines.push_back({ln.depth, std::move(q)});
          }
        }
    }
  }
}

std::vector<uint8_t> EncodeTile(const TileLines& t) {
  std::vector<double> vals;
  for (auto& l : t.lines) if (std::find(vals.begin(), vals.end(), l.first) == vals.end()) vals.push_back(l.first);
  std::vector<uint8_t> layer;
  Key(layer, 15, 0); Varint(layer, 2);                                   // version
  Bytes(layer, 1, std::vector<uint8_t>{'c', 'o', 'n', 't', 'o', 'u', 'r', 's'});
  for (auto& l : t.lines) {
    std::vector<uint8_t> feat, tags, geom;
    Varint(tags, 0); Varint(tags, uint64_t(std::find(vals.begin(), vals.end(), l.first) - vals.begin()));
    Bytes(feat, 2, tags);
    Key(feat, 3, 0); Varint(feat, 2);                                    // LINESTRING
    int32_t cx = 0, cy = 0;
    Varint(geom, 1 | 1 << 3);                                            // MoveTo x1
    Varint(geom, Zig(l.second[0].first - cx)); Varint(geom, Zig(l.second[0].second - cy));
    cx = l.second[0].first; cy = l.second[0].second;
    Varint(geom, 2 | uint64_t(l.second.size() - 1) << 3);                // LineTo xN
    for (size_t i = 1; i < l.second.size(); ++i) {
      Varint(geom, Zig(l.second[i].first - cx)); Varint(geom, Zig(l.second[i].second - cy));
      cx = l.second[i].first; cy = l.second[i].second;
    }
    Bytes(feat, 4, geom);
    Bytes(layer, 2, feat);
  }
  Bytes(layer, 3, std::vector<uint8_t>{'d', 'e', 'p', 't', 'h'});
  for (double v : vals) {
    std::vector<uint8_t> val;
    Key(val, 3, 1);                                                      // double_value
    uint64_t u; memcpy(&u, &v, 8);
    for (int i = 0; i < 8; ++i) val.push_back(uint8_t(u >> (8 * i)));
    Bytes(layer, 4, val);
  }
  Key(layer, 5, 0); Varint(layer, 4096);                                 // extent
  std::vector<uint8_t> tile;
  Bytes(tile, 3, layer);
  return tile;
}

}  // namespace

std::unique_ptr<src::Source> MakeImagery(std::shared_ptr<Grid> g, const Options& o) { return std::make_unique<DepthImagery>(std::move(g), o); }

std::vector<double> ContourLevels(const Grid& g, const std::string& spec0, double maxDepth, bool fixed) {
  std::string spec;
  for (char c : spec0) if (c != ' ') spec += c;
  std::vector<double> out;
  if (spec.empty() || spec == "0" || spec == "off" || spec == "none") return out;
  const double lo = std::max(0.0f, g.minDepth), hi = g.maxDepth;
  if (spec.find(',') != std::string::npos) {
    size_t p = 0;
    while (p <= spec.size()) {
      const size_t q = std::min(spec.find(',', p), spec.size());
      const std::string part = spec.substr(p, q - p);
      if (!part.empty()) {
        size_t used = 0; double d = 0;
        try { d = std::stod(part, &used); } catch (...) { used = 0; }
        if (used != part.size() || d < 0) throw std::runtime_error("--contours list needs depths in metres, e.g. 2,5,10,20, got '" + part + "'");
        if (d >= lo && d <= hi) out.push_back(d);
      }
      p = q + 1;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }
  double step;
  if (spec == "auto") {
    // Close together in the shallows, wider apart in deep water (like a paper chart): every 1 m to 10 m for
    // shallow surveys, then 2, 5, 10, 15, 20, 25, 30, 40, 50 ... 5000 m.
    const double r = AutoRangeMax(g);
    std::vector<double> nice = {2, 5, 10, 15, 20, 25, 30, 40, 50, 60, 70, 80, 100, 120, 150, 200, 250, 300, 400, 500,
                                750, 1000, 1500, 2000, 2500, 3000, 4000, 5000, 6000};
    if (r <= 30) { nice.clear(); for (int v = 1; v <= 30; ++v) nice.push_back(v); }
    if (g.minDepth <= 0 && g.maxDepth > 0) out.push_back(0);
    for (double v : nice) if (v >= lo && v <= hi) out.push_back(v);
    return out;
  } else {
    size_t used = 0;
    try { step = std::stod(spec, &used); } catch (...) { used = 0; step = 0; }
    if (used != spec.size() || !(step > 0)) throw std::runtime_error("--contours needs a spacing in metres (e.g. 5), a list (2,5,10,20), auto or off; got '" + spec0 + "'");
  }
  if (g.minDepth <= 0 && g.maxDepth > 0) out.push_back(0);   // the shoreline (chart datum)
  if (fixed) {
    // The same spacing everywhere, down to the bottom of the colour depth range.
    const double top = maxDepth > 0 ? std::min(hi, maxDepth) : hi;
    if (top < hi) printf("  contour lines every %g m down to %g m (the colour depth range); deeper water has none\n", step, top);
    for (double v = std::ceil(std::max(lo, step * 0.5) / step) * step; v <= top; v += step) {
      out.push_back(v);
      if (out.size() > 600)
        throw std::runtime_error("--contours " + spec + " makes over 600 lines; use a bigger spacing or a smaller --depth-range");
    }
    return out;
  }
  // Like a paper chart: the chosen spacing in the shallows, wider in deep water, where close lines only make a
  // dark smear and a slow, huge chart. Never finer than asked for.
  struct Band { double to, spacing; };
  const Band bands[] = {{50, step}, {200, std::max(step, 5.0)}, {500, std::max(step, 10.0)}, {1000, std::max(step, 50.0)},
                        {1e9, std::max(step, 100.0)}};
  std::string summary;
  double from = 0;
  for (const Band& b : bands) {
    if (from >= hi) break;
    const double end = std::min(b.to, hi);
    const size_t before = out.size();
    for (double v = std::ceil(std::max({from, lo, b.spacing * 0.5}) / b.spacing) * b.spacing; v <= end + 1e-9; v += b.spacing)
      if (v > from || from == 0) if (out.empty() || v > out.back()) out.push_back(v);
    if (out.size() > before) {
      char t[96];
      snprintf(t, sizeof t, "%severy %g m to %g m", summary.empty() ? "" : ", ", b.spacing, end);
      summary += t;
    }
    from = b.to;
  }
  printf("  contour lines: %s (%zu depths)\n", summary.c_str(), out.size());
  if (out.size() > 600) throw std::runtime_error("--contours " + spec + " makes over 600 lines; use a bigger spacing");
  return out;
}

size_t WriteContours(const Grid& g, const std::vector<double>& levels, const std::string& path) {
  // detail zoom: about four tile units per cell; overview at zoom 9 from a coarser grid level
  const double cosLat = std::cos(g.centreLat * kDeg);
  int zmax = int(std::ceil(std::log2(2 * kPi * kA * cosLat / (4096 * std::max(0.5, g.cellMetres / 4)))));
  zmax = std::clamp(zmax, 10, 16);
  std::map<int, std::map<std::pair<int64_t, int64_t>, TileLines>> zooms;
  size_t lines = 0;
  {
    auto ls = LinesAt(g, 0, zmax, levels);
    lines = ls.size();
    AddToTiles(ls, zooms[zmax]);
  }
  constexpr int kOverview = 9;
  if (zmax > kOverview) {
    const double unit9 = 2 * kPi * kA * cosLat / (4096.0 * 512);
    int L = 0;
    while (L + 1 < int(g.levels.size()) && g.cellMetres * double(1 << L) < unit9 * 2) ++L;
    AddToTiles(LinesAt(g, L, kOverview, levels), zooms[kOverview]);
  }
  DeleteFileW(TiffFile::Widen(path).c_str());
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
    throw std::runtime_error("cannot create " + path);
  auto exec = [&](const char* sql) { if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) { std::string m = sqlite3_errmsg(db); sqlite3_close(db); throw std::runtime_error("contours file: " + m); } };
  exec("PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;");
  exec("CREATE TABLE metadata (name TEXT, value TEXT); CREATE TABLE tiles (zoom_level INTEGER, tile_column INTEGER, tile_row INTEGER, tile_data BLOB);");
  exec("BEGIN");
  {
    char b[256];
    const int zmin = zooms.begin()->first;
    std::vector<std::pair<std::string, std::string>> meta = {
        {"name", "contours"}, {"format", "pbf"}, {"type", "overlay"},
        {"minzoom", std::to_string(zmin)}, {"maxzoom", std::to_string(zmax)},
        {"json", "{\"vector_layers\":[{\"id\":\"contours\",\"fields\":{\"depth\":\"Number\"}}]}"}};
    snprintf(b, sizeof b, "%.6f,%.6f,%.6f,%.6f", g.box.lonMin, g.box.latMin, g.box.lonMax, g.box.latMax);
    meta.push_back({"bounds", b});
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO metadata VALUES (?1, ?2)", -1, &st, nullptr);
    for (auto& m : meta) {
      sqlite3_reset(st);
      sqlite3_bind_text(st, 1, m.first.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 2, m.second.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "INSERT INTO tiles VALUES (?1, ?2, ?3, ?4)", -1, &st, nullptr);
    for (auto& [z, tiles] : zooms)
      for (auto& [xy, tl] : tiles) {
        const auto data = EncodeTile(tl);
        sqlite3_reset(st);
        sqlite3_bind_int(st, 1, z);
        sqlite3_bind_int64(st, 2, xy.first);
        sqlite3_bind_int64(st, 3, ((int64_t(1) << z) - 1) - xy.second);   // MBTiles rows count from the south
        sqlite3_bind_blob(st, 4, data.data(), int(data.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); sqlite3_close(db); throw std::runtime_error("contours file: write failed"); }
      }
    sqlite3_finalize(st);
  }
  exec("CREATE UNIQUE INDEX tile_index ON tiles (zoom_level, tile_column, tile_row)");
  exec("COMMIT");
  sqlite3_close(db);
  return lines;
}

}  // namespace depth

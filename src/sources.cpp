#include "sources.hpp"
#include "at5.hpp"

#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <propvarutil.h>
#include <winsqlite/winsqlite3.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <list>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace src {
namespace {

constexpr double kPi = 3.14159265358979323846;
std::atomic<long long> gBadTiles{0};
const double kRl = at5::kLowranceR, kRw = at5::kWebR;

std::wstring Widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  w.resize(wcslen(w.c_str()));
  return w;
}
std::string Lower(std::string s) { for (auto& c : s) c = char(tolower((unsigned char)c)); return s; }

struct ComInit {  // one per thread that touches WIC
  ComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
};
thread_local ComInit tlCom;

ComPtr<IWICImagingFactory> Factory() {
  (void)tlCom;
  thread_local ComPtr<IWICImagingFactory> f;
  if (!f && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
    throw std::runtime_error("WIC unavailable");
  return f;
}

bool DecodeRgba(IWICBitmapSource* frame, std::vector<uint8_t>& out, UINT& w, UINT& h) {
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(Factory()->CreateFormatConverter(&conv))) return false;
  if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
  conv->GetSize(&w, &h);
  out.resize(size_t(w) * h * 4);
  return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(out.size()), out.data()));
}

bool DecodeMemory(const void* data, size_t n, std::vector<uint8_t>& out, UINT& w, UINT& h) {
  auto f = Factory();
  ComPtr<IWICStream> st;
  if (FAILED(f->CreateStream(&st)) || FAILED(st->InitializeFromMemory((BYTE*)data, DWORD(n)))) return false;
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(f->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
  ComPtr<IWICBitmapFrameDecode> fr;
  if (FAILED(dec->GetFrame(0, &fr))) return false;
  return DecodeRgba(fr.Get(), out, w, h);
}

// Bilinear sample of an RGBA mosaic with validity (alpha). (x,y) continuous pixel-centre coords.
inline bool Bilinear(const uint8_t* m, int mw, int mh, double x, double y, uint8_t* o) {
  x -= 0.5; y -= 0.5;
  int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  double fx = x - x0, fy = y - y0;
  double acc[3] = {0, 0, 0}, wsum = 0;
  for (int dy = 0; dy < 2; ++dy)
    for (int dx = 0; dx < 2; ++dx) {
      int xi = std::clamp(x0 + dx, 0, mw - 1), yi = std::clamp(y0 + dy, 0, mh - 1);
      const uint8_t* p = m + (size_t(yi) * mw + xi) * 4;
      if (p[3] < 128) continue;
      double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
      acc[0] += w * p[0]; acc[1] += w * p[1]; acc[2] += w * p[2]; wsum += w;
    }
  if (wsum < 1e-6) { o[3] = 0; return false; }
  for (int c = 0; c < 3; ++c) o[c] = uint8_t(std::clamp(acc[c] / wsum + 0.5, 0.0, 255.0));
  o[3] = 255;
  return true;
}

// ============================================================================ MBTiles
class MbTiles;
class MbSampler : public Sampler {
 public:
  MbSampler(MbTiles* s);
  ~MbSampler() override;
  bool RenderTile(double mpp, int64_t tx, int64_t ty, uint8_t* rgba) override;
 private:
  const std::vector<uint8_t>* GetTile(int z, int64_t x, int64_t y);  // y = XYZ row; nullptr if missing
  MbTiles* s_;
  sqlite3* db_ = nullptr;
  sqlite3_stmt* st_ = nullptr;
  struct Entry { uint64_t key; std::vector<uint8_t> px; bool ok; };
  std::list<Entry> lru_;
  std::unordered_map<uint64_t, std::list<Entry>::iterator> map_;
  std::vector<uint8_t> mosaic_;
};

class MbTiles : public Source {
 public:
  explicit MbTiles(const std::string& path, int zoom) : path_(path) {
    {  // an SQLite database at all? (sqlite would only fail later with a vague message)
      FILE* f = nullptr;
      char head[16] = {};
      if (!fopen_s(&f, path.c_str(), "rb") && f) { fread(head, 1, 16, f); fclose(f); }
      if (memcmp(head, "SQLite format 3", 16) != 0) throw std::runtime_error("not an MBTiles file (not an SQLite database): " + path);
    }
    sqlite3* db = OpenTilesDb(path);
    {
      sqlite3_stmt* t = nullptr;
      const bool has = sqlite3_prepare_v2(db, "SELECT 1 FROM tiles LIMIT 1", -1, &t, nullptr) == SQLITE_OK;
      sqlite3_finalize(t);
      if (!has) { sqlite3_close(db); throw std::runtime_error("not an MBTiles file (it has no 'tiles' table): " + path); }
    }
    auto q1 = [&](const char* sql, int& out) {
      sqlite3_stmt* st = nullptr; out = -1;
      if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && Step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        out = sqlite3_column_int(st, 0);
      sqlite3_finalize(st);
    };
    q1("SELECT MAX(zoom_level) FROM tiles", maxZoom_);
    q1("SELECT MIN(zoom_level) FROM tiles", minZoom_);
    if (maxZoom_ < 0) { sqlite3_close(db); throw std::runtime_error("mbtiles has no tiles"); }
    zoom_ = zoom >= 0 ? std::clamp(zoom, minZoom_, maxZoom_) : maxZoom_;
    // tile existence index per zoom (sorted keys), for fast empty checks and parent fallback
    exist_.resize(size_t(maxZoom_) + 1);
    sqlite3_stmt* st = nullptr;
    // rowid lookups are always indexed; 'tiles' may be a VIEW (no rowid) -> fall back to keyed queries
    useRowid_ = sqlite3_prepare_v2(db, "SELECT zoom_level, tile_column, tile_row, rowid FROM tiles", -1, &st, nullptr) == SQLITE_OK &&
                sqlite3_column_count(st) == 4;
    if (!useRowid_) { sqlite3_finalize(st); sqlite3_prepare_v2(db, "SELECT zoom_level, tile_column, tile_row FROM tiles", -1, &st, nullptr); }
    rowids_.resize(size_t(maxZoom_) + 1);
    for (bool first = true; Step(st, first) == SQLITE_ROW; first = false) {
      int z = sqlite3_column_int(st, 0);
      int64_t x = sqlite3_column_int64(st, 1), tr = sqlite3_column_int64(st, 2);
      if (z < 0 || z > maxZoom_ || z > 28) continue;
      int64_t y = ((int64_t(1) << z) - 1) - tr;  // TMS -> XYZ
      rowids_[size_t(z)].push_back({Key(z, x, y), useRowid_ ? sqlite3_column_int64(st, 3) : 0});
    }
    sqlite3_finalize(st);
    for (size_t z = 0; z < rowids_.size(); ++z) {
      std::sort(rowids_[z].begin(), rowids_[z].end());
      exist_[z].reserve(rowids_[z].size());
      for (auto& p : rowids_[z]) exist_[z].push_back(p.first);
      if (!useRowid_) std::vector<std::pair<uint64_t, int64_t>>().swap(rowids_[z]);
    }
    sqlite3_stmt* m;
    if (sqlite3_prepare_v2(db, "SELECT value FROM metadata WHERE name='name'", -1, &m, nullptr) == SQLITE_OK && Step(m) == SQLITE_ROW)
      name_ = (const char*)sqlite3_column_text(m, 0);
    sqlite3_finalize(m);
    sqlite3_close(db);
    // bounds from tiles present at the working zoom
    const auto& v = exist_[size_t(zoom_)];
    if (v.empty()) throw std::runtime_error("no tiles at zoom " + std::to_string(zoom_));
    int64_t x0 = INT64_MAX, x1 = INT64_MIN, y0 = INT64_MAX, y1 = INT64_MIN;
    for (auto k : v) { int64_t x = int64_t((k >> 28) & 0xFFFFFFF), y = int64_t(k & 0xFFFFFFF); x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    double n = double(int64_t(1) << zoom_);
    auto lon = [&](double x) { return x / n * 360.0 - 180.0; };
    auto lat = [&](double y) { return std::atan(std::sinh(kPi * (1 - 2 * y / n))) * 180.0 / kPi; };
    box_ = {lon(double(x0)), lat(double(y1 + 1)), lon(double(x1 + 1)), lat(double(y0))};
    tileCount_ = v.size();
  }
  LonLatBox Bounds() const override { return box_; }
  double NativeMpp() const override { return 2 * kPi * kRl / (256.0 * double(int64_t(1) << zoom_)); }
  std::string Describe() const override {
    return "MBTiles '" + name_ + "' zoom " + std::to_string(zoom_) + " (" + std::to_string(tileCount_) +
           " tiles; zooms " + std::to_string(minZoom_) + ".." + std::to_string(maxZoom_) + ")";
  }
  std::unique_ptr<Sampler> MakeSampler() override { return std::make_unique<MbSampler>(this); }

  static uint64_t Key(int z, int64_t x, int64_t y) { return uint64_t(z) << 56 | uint64_t(x & 0xFFFFFFF) << 28 | uint64_t(y & 0xFFFFFFF); }
  int64_t RowId(int z, int64_t x, int64_t y) const {
    const auto& v = rowids_[size_t(z)];
    auto it = std::lower_bound(v.begin(), v.end(), std::make_pair(Key(z, x, y), int64_t(INT64_MIN)));
    return (it != v.end() && it->first == Key(z, x, y)) ? it->second : -1;
  }
  bool Exists(int z, int64_t x, int64_t y) const {
    if (z < 0 || z >= int(exist_.size())) return false;
    const auto& v = exist_[size_t(z)];
    return std::binary_search(v.begin(), v.end(), Key(z, x, y));
  }
  std::string path_, name_;
  int zoom_ = 0, minZoom_ = 0, maxZoom_ = 0;
  size_t tileCount_ = 0;
  std::vector<std::vector<uint64_t>> exist_;
  std::vector<std::vector<std::pair<uint64_t, int64_t>>> rowids_;
  bool useRowid_ = false;
  LonLatBox box_{};
};

MbSampler::MbSampler(MbTiles* s) : s_(s) {
  db_ = OpenTilesDb(s->path_);
  if (s->useRowid_) sqlite3_prepare_v2(db_, "SELECT tile_data FROM tiles WHERE rowid=?1", -1, &st_, nullptr);
  else sqlite3_prepare_v2(db_, "SELECT tile_data FROM tiles WHERE zoom_level=?1 AND tile_column=?2 AND tile_row=?3", -1, &st_, nullptr);
}
MbSampler::~MbSampler() { sqlite3_finalize(st_); sqlite3_close(db_); }

const std::vector<uint8_t>* MbSampler::GetTile(int z, int64_t x, int64_t y) {
  if (!s_->Exists(z, x, y)) return nullptr;
  uint64_t key = MbTiles::Key(z, x, y);
  auto it = map_.find(key);
  if (it != map_.end()) { lru_.splice(lru_.begin(), lru_, it->second); return it->second->ok ? &it->second->px : nullptr; }
  Entry e{key, {}, false};
  sqlite3_reset(st_);
  if (s_->useRowid_) sqlite3_bind_int64(st_, 1, s_->RowId(z, x, y));
  else {
    sqlite3_bind_int(st_, 1, z);
    sqlite3_bind_int64(st_, 2, x);
    sqlite3_bind_int64(st_, 3, ((int64_t(1) << z) - 1) - y);
  }
  if (Step(st_) == SQLITE_ROW) {
    const void* blob = sqlite3_column_blob(st_, 0);
    int n = sqlite3_column_bytes(st_, 0);
    UINT w = 0, h = 0;
    if (blob && n > 0 && DecodeMemory(blob, size_t(n), e.px, w, h)) {
      if (w == 256 && h == 256) e.ok = true;
      else if (w > 0 && h > 0) {  // non-256 tiles (e.g. 512 retina): nearest resample to 256
        std::vector<uint8_t> r(256 * 256 * 4);
        for (int j = 0; j < 256; ++j)
          for (int i = 0; i < 256; ++i)
            memcpy(&r[(size_t(j) * 256 + i) * 4], &e.px[(size_t(j * int(h) / 256) * w + size_t(i * int(w) / 256)) * 4], 4);
        e.px.swap(r); e.ok = true;
      }
    }
    if (!e.ok) ++gBadTiles;
  }
  lru_.push_front(std::move(e));
  map_[key] = lru_.begin();
  if (lru_.size() > 96) { map_.erase(lru_.back().key); lru_.pop_back(); }
  return lru_.front().ok ? &lru_.front().px : nullptr;
}

bool MbSampler::RenderTile(double mpp, int64_t tx, int64_t ty, uint8_t* rgba) {
  const int z = s_->zoom_;
  const double scale = 256.0 * double(int64_t(1) << z) / (2 * kPi * kRw);  // web metres -> global px
  const double k = kRw / kRl;
  // affine: gpx = ax*i + bx ; gpy = ay*j + by  (i,j output pixel indices, j=0 south)
  const double ax = mpp * k * scale, bx = ((tx * 256 + 0.5) * mpp * k + kPi * kRw) * scale;
  const double ay = -mpp * k * scale, by = (kPi * kRw - (ty * 256 + 0.5) * mpp * k) * scale;
  double gx0 = bx - 2, gx1 = ax * 255 + bx + 2;
  double gy0 = ay * 255 + by - 2, gy1 = by + 2;
  int64_t txa = int64_t(std::floor(gx0 / 256)), txb = int64_t(std::floor(gx1 / 256));
  int64_t tya = int64_t(std::floor(gy0 / 256)), tyb = int64_t(std::floor(gy1 / 256));
  const int64_t nmax = (int64_t(1) << z) - 1;
  txa = std::max<int64_t>(txa, 0); tya = std::max<int64_t>(tya, 0);
  txb = std::min(txb, nmax); tyb = std::min(tyb, nmax);
  if (txa > txb || tya > tyb) return false;
  // quick existence test at this zoom and up to 4 parent levels
  bool any = false;
  for (int d = 0; d <= 4 && !any && z - d >= s_->minZoom_; ++d)
    for (int64_t y = tya >> d; y <= (tyb >> d) && !any; ++y)
      for (int64_t x = txa >> d; x <= (txb >> d) && !any; ++x) any = s_->Exists(z - d, x, y);
  if (!any) return false;
  const int mw = int(txb - txa + 1) * 256, mh = int(tyb - tya + 1) * 256;
  mosaic_.assign(size_t(mw) * mh * 4, 0);
  for (int64_t y = tya; y <= tyb; ++y)
    for (int64_t x = txa; x <= txb; ++x) {
      uint8_t* dst = &mosaic_[(size_t(y - tya) * 256 * mw + size_t(x - txa) * 256) * 4];
      if (auto t = GetTile(z, x, y)) {
        for (int r = 0; r < 256; ++r) memcpy(dst + size_t(r) * mw * 4, t->data() + size_t(r) * 1024, 1024);
        continue;
      }
      for (int d = 1; d <= 4 && z - d >= s_->minZoom_; ++d) {  // parent fallback (upsampled)
        auto t = GetTile(z - d, x >> d, y >> d);
        if (!t) continue;
        const int sub = 256 >> d, ox = int(x & ((1 << d) - 1)) * sub, oy = int(y & ((1 << d) - 1)) * sub;
        for (int r = 0; r < 256; ++r)
          for (int c = 0; c < 256; ++c)
            memcpy(dst + (size_t(r) * mw + c) * 4, t->data() + (size_t(oy + (r >> d)) * 256 + ox + (c >> d)) * 4, 4);
        break;
      }
    }
  bool got = false;
  const double offx = double(txa * 256), offy = double(tya * 256);
  for (int j = 0; j < 256; ++j) {
    const double gy = ay * j + by - offy;
    uint8_t* row = rgba + size_t(j) * 256 * 4;
    for (int i = 0; i < 256; ++i) got |= Bilinear(mosaic_.data(), mw, mh, ax * i + bx - offx, gy, row + i * 4);
  }
  return got;
}

// ============================================================================ georeferenced image
class GeoImage;
class ImgSampler : public Sampler {
 public:
  explicit ImgSampler(GeoImage* g) : g_(g) {}
  bool RenderTile(double mpp, int64_t tx, int64_t ty, uint8_t* rgba) override;
 private:
  GeoImage* g_;
};

class GeoImage : public Source {
 public:
  GeoImage(const std::string& path, int crs) : path_(path) {
    auto f = Factory();
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(Widen(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
      throw std::runtime_error("cannot decode image (WIC): " + path);
    ComPtr<IWICBitmapFrameDecode> fr;
    dec->GetFrame(0, &fr);
    bool geo = ReadGeoTiff(fr.Get());
    if (!geo) geo = ReadWorldFile();
    if (!geo) throw std::runtime_error("no georeferencing found (GeoTIFF tags or world file .tfw/.jgw/.pgw/.wld)");
    if (crs) crs_ = crs;
    if (!crs_) crs_ = (std::fabs(c0_) <= 360 && std::fabs(f0_) <= 90) ? 4326 : 3857;
    UINT w, h;
    if (!DecodeRgba(fr.Get(), px_, w, h)) throw std::runtime_error("image decode failed: " + path);
    w_ = int(w); h_ = int(h);
    // inverse affine (map -> pixel corner coords)
    double det = a_ * e_ - b_ * d_;
    if (std::fabs(det) < 1e-30) throw std::runtime_error("degenerate georeference");
    ia_ = e_ / det; ib_ = -b_ / det; id_ = -d_ / det; ie_ = a_ / det;
    double lo0 = 1e9, la0 = 1e9, lo1 = -1e9, la1 = -1e9;
    for (int cy = 0; cy <= 1; ++cy)
      for (int cx = 0; cx <= 1; ++cx) {
        double X = c0_ + a_ * cx * w_ + b_ * cy * h_, Y = f0_ + d_ * cx * w_ + e_ * cy * h_;
        double lon, lat; MapToLonLat(X, Y, lon, lat);
        lo0 = std::min(lo0, lon); lo1 = std::max(lo1, lon); la0 = std::min(la0, lat); la1 = std::max(la1, lat);
      }
    box_ = {lo0, std::max(la0, -85.0), lo1, std::min(la1, 85.0)};
  }
  LonLatBox Bounds() const override { return box_; }
  double NativeMpp() const override {
    double sx = std::hypot(a_, d_);
    return crs_ == 4326 ? sx * kPi / 180.0 * kRl : sx * kRl / kRw;
  }
  std::string Describe() const override {
    return "image " + path_ + " " + std::to_string(w_) + "x" + std::to_string(h_) + " EPSG:" + std::to_string(crs_);
  }
  std::unique_ptr<Sampler> MakeSampler() override { return std::make_unique<ImgSampler>(this); }

  void MapToLonLat(double X, double Y, double& lon, double& lat) const {
    if (crs_ == 4326) { lon = X; lat = Y; return; }
    lon = X / kRw * 180.0 / kPi; lat = (2 * std::atan(std::exp(Y / kRw)) - kPi / 2) * 180.0 / kPi;
  }

  std::string path_;
  int w_ = 0, h_ = 0, crs_ = 0;
  std::vector<uint8_t> px_;
  double a_ = 1, b_ = 0, c0_ = 0, d_ = 0, e_ = -1, f0_ = 0;  // X = c0 + a*col + b*row ; Y = f0 + d*col + e*row (pixel corners)
  double ia_, ib_, id_, ie_;
  LonLatBox box_{};

 private:
  bool ReadWorldFile() {
    std::string base = path_.substr(0, path_.find_last_of('.'));
    std::string ext = Lower(path_.substr(path_.find_last_of('.') + 1));
    std::vector<std::string> cands = {base + ".wld", path_ + "w", base + "." + ext.substr(0, 1) + ext.substr(ext.size() - 1) + "w",
                                      base + ".tfw", base + ".jgw", base + ".pgw", base + ".bpw", base + ".gfw"};
    for (auto& c : cands) {
      std::ifstream in(c);
      double v[6];
      if (in && (in >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5])) {
        a_ = v[0]; d_ = v[1]; b_ = v[2]; e_ = v[3];
        c0_ = v[4] - a_ / 2 - b_ / 2; f0_ = v[5] - d_ / 2 - e_ / 2;  // centre -> corner
        return true;
      }
    }
    return false;
  }
  bool ReadGeoTiff(IWICBitmapFrameDecode* fr) {
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(fr->GetMetadataQueryReader(&q))) return false;
    auto getd = [&](const wchar_t* name, std::vector<double>& out) {
      PROPVARIANT pv; PropVariantInit(&pv);
      bool ok = false;
      if (SUCCEEDED(q->GetMetadataByName(name, &pv))) {
        if (pv.vt == (VT_VECTOR | VT_R8)) { out.assign(pv.cadbl.pElems, pv.cadbl.pElems + pv.cadbl.cElems); ok = true; }
      }
      PropVariantClear(&pv);
      return ok;
    };
    std::vector<double> scale, tie, keysd;
    if (!getd(L"/ifd/{ushort=33550}", scale) || !getd(L"/ifd/{ushort=33922}", tie) || scale.size() < 2 || tie.size() < 6) return false;
    a_ = scale[0]; e_ = -scale[1]; b_ = d_ = 0;
    c0_ = tie[3] - tie[0] * a_; f0_ = tie[4] - tie[1] * e_;
    // GeoKeyDirectory (34735): find GeographicType(2048)/ProjectedCSType(3072) EPSG codes and raster type(1025)
    PROPVARIANT pv; PropVariantInit(&pv);
    if (SUCCEEDED(q->GetMetadataByName(L"/ifd/{ushort=34735}", &pv)) && pv.vt == (VT_VECTOR | VT_UI2)) {
      const USHORT* k = pv.caui.pElems; ULONG n = pv.caui.cElems;
      int rasterIsPoint = 0;
      for (ULONG i = 4; i + 3 < n; i += 4) {
        USHORT id = k[i], loc = k[i + 1], val = k[i + 3];
        if (loc != 0) continue;
        if (id == 3072 && (val == 3857 || val == 3785)) crs_ = 3857;
        if (id == 2048 && val == 4326 && !crs_) crs_ = 4326;
        if (id == 1024 && val == 2 && !crs_) crs_ = 4326;  // ModelTypeGeographic
        if (id == 1025 && val == 2) rasterIsPoint = 1;
      }
      if (rasterIsPoint) { c0_ -= a_ / 2; f0_ -= e_ / 2; }
    }
    PropVariantClear(&pv);
    return true;
  }
};

bool ImgSampler::RenderTile(double mpp, int64_t tx, int64_t ty, uint8_t* rgba) {
  const GeoImage& g = *g_;
  const double k = kRw / kRl;
  bool got = false;
  std::vector<double> mx(256);
  for (int i = 0; i < 256; ++i) {
    double X = (tx * 256 + i + 0.5) * mpp;
    mx[size_t(i)] = g.crs_ == 4326 ? X / kRl * 180.0 / kPi : X * k;
  }
  for (int j = 0; j < 256; ++j) {
    double Y = (ty * 256 + j + 0.5) * mpp;
    double my = g.crs_ == 4326 ? (2 * std::atan(std::exp(Y / kRl)) - kPi / 2) * 180.0 / kPi : Y * k;
    uint8_t* row = rgba + size_t(j) * 256 * 4;
    for (int i = 0; i < 256; ++i) {
      double dx = mx[size_t(i)] - g.c0_, dy = my - g.f0_;
      double col = g.ia_ * dx + g.ib_ * dy, r = g.id_ * dx + g.ie_ * dy;
      if (col < -0.5 || r < -0.5 || col > g.w_ + 0.5 || r > g.h_ + 0.5) { row[i * 4 + 3] = 0; continue; }
      got |= Bilinear(g.px_.data(), g.w_, g.h_, col, r, row + i * 4);
    }
  }
  return got;
}

}  // namespace

std::unique_ptr<Source> Open(const std::string& path, const OpenOptions& opt) {
  std::string ext = Lower(path.substr(path.find_last_of('.') + 1));
  if (ext == "mbtiles" || ext == "sqlite" || ext == "db") return std::make_unique<MbTiles>(path, opt.zoom);
  return std::make_unique<GeoImage>(path, opt.crs);
}

bool SavePng(const std::wstring& path, const uint8_t* rgba, int w, int h) {
  auto f = Factory();
  ComPtr<IWICStream> st; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> fr;
  if (FAILED(f->CreateStream(&st)) || FAILED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
  if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache))) return false;
  if (FAILED(enc->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr))) return false;
  fr->SetSize(UINT(w), UINT(h));
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;  // PNG encoder's native order; RGBA is silently remapped
  fr->SetPixelFormat(&fmt);
  if (fmt != GUID_WICPixelFormat32bppBGRA) return false;
  std::vector<uint8_t> bgra(rgba, rgba + size_t(w) * h * 4);
  for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
  if (FAILED(fr->WritePixels(UINT(h), UINT(w) * 4, UINT(bgra.size()), bgra.data()))) return false;
  return SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
}

long long BadTiles() { return gBadTiles.load(); }

sqlite3* OpenTilesDb(const std::string& path) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    throw std::runtime_error("cannot open mbtiles: " + path);
  }
  sqlite3_busy_timeout(db, 60000);
  return db;
}

int Step(sqlite3_stmt* st, bool retry) {
  for (int tries = 0;; ++tries) {
    const int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) return rc;
    const int base = rc & 0xFF;
    if (retry && (base == SQLITE_BUSY || base == SQLITE_LOCKED) && tries < 300) { sqlite3_reset(st); Sleep(DWORD(5 + tries)); continue; }
    throw std::runtime_error(std::string("MBTiles read failed (") + sqlite3_errstr(rc) + ")");
  }
}

}  // namespace src

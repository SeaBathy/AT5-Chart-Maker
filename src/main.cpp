// sat2at5 - fast satellite imagery (MBTiles / GeoTIFF / world-file images) -> Simrad/Lowrance AT5 3D-texture converter.
#include "at5.hpp"
#include "depth.hpp"
#include "dxt1.hpp"
#include "sources.hpp"
#include "vector.hpp"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kTopExp = 13;  // 8192 m/px, IMC's coarsest texture level

std::string TodayLong() {
  wchar_t buf[128];
  if (!GetDateFormatEx(L"en-US", 0, nullptr, L"dddd, MMMM d, yyyy", buf, 128, nullptr)) return "Monday, January 1, 2024";
  char out[256];
  WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof out, nullptr, nullptr);
  return out;
}

using Rgba = std::vector<uint8_t>;  // 256*256*4, row 0 = south

struct TileRec { int64_t tx, ty; uint64_t off; };
struct LevelStore {
  std::string path;
  FILE* f = nullptr;
  std::mutex m;
  uint64_t used = 0;
  std::vector<TileRec> tiles;
};

struct Range { int64_t x0, y0, x1, y1; bool Contains(int64_t x, int64_t y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; } };

class Pyramid {
 public:
  Pyramid(src::Source& s, int finest, int coarsest, int threads, const fs::path& tmp)
      : src_(s), finest_(finest), coarsest_(coarsest), threads_(threads) {
    auto b = s.Bounds();
    const double R = at5::kLowranceR;
    X0_ = b.lonMin * kPi / 180 * R; X1_ = b.lonMax * kPi / 180 * R;
    Y0_ = R * std::log(std::tan(kPi / 4 + std::max(b.latMin, -85.0) * kPi / 360));
    Y1_ = R * std::log(std::tan(kPi / 4 + std::min(b.latMax, 85.0) * kPi / 360));
    for (int e = finest_; e <= coarsest_; ++e) {
      double size = 256 * at5::LevelMpp(e);
      ranges_[e] = {int64_t(std::floor(X0_ / size)), int64_t(std::floor(Y0_ / size)),
                    int64_t(std::floor((X1_ - 1e-6) / size)), int64_t(std::floor((Y1_ - 1e-6) / size))};
      auto st = std::make_unique<LevelStore>();
      st->path = (tmp / ("level_" + std::to_string(e) + ".bin")).string();
      if (fopen_s(&st->f, st->path.c_str(), "wb") || !st->f) throw std::runtime_error("cannot create temp " + st->path);
      stores_[e] = std::move(st);
    }
  }
  ~Pyramid() { for (auto& s : stores_) if (s.second->f) fclose(s.second->f); }

  void Run() {
    // work level: coarsest level that still yields enough parallel tasks
    int lw = finest_;
    for (int e = coarsest_; e >= finest_; --e) {
      auto r = ranges_[e];
      if ((r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1) >= int64_t(threads_) * 6) { lw = e; break; }
    }
    auto rf = ranges_[finest_];
    estFinest_ = (rf.x1 - rf.x0 + 1) * (rf.y1 - rf.y0 + 1);
    std::vector<std::pair<int64_t, int64_t>> tasks;
    auto r = ranges_[lw];
    for (int64_t y = r.y0; y <= r.y1; ++y) for (int64_t x = r.x0; x <= r.x1; ++x) tasks.push_back({x, y});
    printf("  work level %s m/px: %zu parallel tasks on %d threads\n", at5::LevelName(lw).c_str(), tasks.size(), threads_);

    std::map<std::pair<int64_t, int64_t>, std::shared_ptr<Rgba>> results;
    std::mutex rm;
    std::atomic<size_t> next{0};
    t0_ = Clock::now();
    std::atomic<bool> stopProgress{false};
    std::thread prog([&] {
      const bool console = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR;
      int tick = 0;
      while (!stopProgress) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (++tick % (console ? 4 : 40)) continue;
        double s = std::chrono::duration<double>(Clock::now() - t0_).count();
        printf(console ? "\r  rendered %lld finest tiles (bbox est. %lld), %lld total, %.0f tiles/s   "
                       : "  rendered %lld finest tiles (bbox est. %lld), %lld total, %.0f tiles/s\n",
               (long long)doneFinest_.load(), (long long)estFinest_, (long long)doneAll_.load(),
               doneAll_.load() / std::max(s, 1e-3));
        fflush(stdout);
      }
    });
    std::exception_ptr err;
    std::mutex em;
    auto worker = [&] {
      try {
        auto sampler = src_.MakeSampler();
        for (;;) {
          size_t i = next++;
          if (i >= tasks.size()) break;
          auto t = Build(*sampler, lw, tasks[i].first, tasks[i].second);
          if (t) { std::lock_guard<std::mutex> g(rm); results[tasks[i]] = std::move(t); }
        }
      } catch (...) { std::lock_guard<std::mutex> g(em); if (!err) err = std::current_exception(); next = tasks.size(); }
    };
    std::vector<std::thread> th;
    for (int i = 0; i < threads_; ++i) th.emplace_back(worker);
    for (auto& t : th) t.join();
    if (err) { stopProgress = true; prog.join(); std::rethrow_exception(err); }
    // levels above the work level, built from the retained work-level tiles
    for (int e = lw + 1; e <= coarsest_; ++e) {
      std::map<std::pair<int64_t, int64_t>, std::shared_ptr<Rgba>> parents;
      std::map<std::pair<int64_t, int64_t>, std::array<std::shared_ptr<Rgba>, 4>> groups;
      for (auto& kv : results) {
        int64_t x = kv.first.first, y = kv.first.second;
        int64_t px = x >> 1, py = y >> 1;  // floor division (arithmetic shift)
        groups[{px, py}][size_t((y - py * 2) * 2 + (x - px * 2))] = kv.second;
      }
      std::vector<std::pair<std::pair<int64_t, int64_t>, std::array<std::shared_ptr<Rgba>, 4>>> list(groups.begin(), groups.end());
      std::atomic<size_t> ni{0};
      std::mutex pm;
      auto w2 = [&] {
        for (;;) {
          size_t i = ni++;
          if (i >= list.size()) break;
          auto out = std::make_shared<Rgba>(size_t(256) * 256 * 4, 0);
          for (int q = 0; q < 4; ++q) if (list[i].second[size_t(q)]) Down(*list[i].second[size_t(q)], *out, q & 1, q >> 1);
          Emit(e, list[i].first.first, list[i].first.second, *out);
          std::lock_guard<std::mutex> g(pm); parents[list[i].first] = out;
        }
      };
      th.clear();
      for (int i = 0; i < threads_; ++i) th.emplace_back(w2);
      for (auto& t : th) t.join();
      results.swap(parents);
    }
    stopProgress = true;
    prog.join();
    double s = std::chrono::duration<double>(Clock::now() - t0_).count();
    printf("\r  rendered %lld finest tiles, %lld tiles in all levels in %.1f s (%.0f tiles/s)          \n",
           (long long)doneFinest_.load(), (long long)doneAll_.load(), s, doneAll_.load() / std::max(s, 1e-3));
    for (auto& st : stores_) { fclose(st.second->f); st.second->f = nullptr; }
  }

  std::map<int, std::unique_ptr<LevelStore>>& Stores() { return stores_; }

 private:
  std::shared_ptr<Rgba> Build(src::Sampler& sm, int e, int64_t tx, int64_t ty) {
    if (!ranges_[e].Contains(tx, ty)) return nullptr;
    auto out = std::make_shared<Rgba>(size_t(256) * 256 * 4, 0);
    if (e == finest_) {
      if (!sm.RenderTile(at5::LevelMpp(e), tx, ty, out->data())) return nullptr;
      ++doneFinest_;
    } else {
      bool any = false;
      for (int q = 0; q < 4; ++q) {
        auto c = Build(sm, e - 1, tx * 2 + (q & 1), ty * 2 + (q >> 1));
        if (c) { Down(*c, *out, q & 1, q >> 1); any = true; }
      }
      if (!any) return nullptr;
    }
    Emit(e, tx, ty, *out);
    return out;
  }

  // 2x2 box filter of a child tile into quadrant (qx,qy) of the parent (row 0 = south on both)
  static void Down(const Rgba& c, Rgba& p, int qx, int qy) {
    for (int j = 0; j < 128; ++j) {
      const uint8_t* r0 = c.data() + size_t(2 * j) * 1024;
      const uint8_t* r1 = r0 + 1024;
      uint8_t* o = p.data() + (size_t(qy * 128 + j) * 256 + qx * 128) * 4;
      for (int i = 0; i < 128; ++i, o += 4) {
        const uint8_t* q[4] = {r0 + i * 8, r0 + i * 8 + 4, r1 + i * 8, r1 + i * 8 + 4};
        int n = 0, s[3] = {0, 0, 0};
        for (auto* a : q) if (a[3]) { ++n; s[0] += a[0]; s[1] += a[1]; s[2] += a[2]; }
        if (!n) { o[3] = 0; continue; }
        o[0] = uint8_t((s[0] + n / 2) / n); o[1] = uint8_t((s[1] + n / 2) / n); o[2] = uint8_t((s[2] + n / 2) / n); o[3] = 255;
      }
    }
  }

  void Emit(int e, int64_t tx, int64_t ty, const Rgba& rgba) {
    thread_local std::vector<uint8_t> dxt(at5::kTileDxtBytes);
    dxt1::EncodeTile256(rgba.data(), dxt.data());
    auto& st = *stores_[e];
    std::lock_guard<std::mutex> g(st.m);
    if (fwrite(dxt.data(), 1, dxt.size(), st.f) != dxt.size()) throw std::runtime_error("temp write failed (disk full?)");
    st.tiles.push_back({tx, ty, st.used});
    st.used += dxt.size();
    ++doneAll_;
  }

  src::Source& src_;
  int finest_, coarsest_, threads_;
  double X0_, X1_, Y0_, Y1_;
  std::map<int, Range> ranges_;
  std::map<int, std::unique_ptr<LevelStore>> stores_;
  std::atomic<long long> doneFinest_{0}, doneAll_{0};
  long long estFinest_ = 0;
  Clock::time_point t0_;
};

struct ConvertOpts {
  std::string input, outDir, desc = "M=SatMap", usage = "UnrestrictedUse";
  double finestMpp = 0, coarsestMpp = 8192;
  int zoom = -1, crs = 0, threads = 0;
  bool asImage = false;   // convert encoded-DEM raster MBTiles as plain imagery anyway
  bool ignoreSpace = false;   // skip the free-disk-space check
  double splitMb = 450;
  vec::Options v;
  bool descSet = false;
  std::string project;   // IMC "Project Based Naming": prefix in the output file names (empty = IMC default names)
  // depth GeoTIFFs (bathymetry surveys)
  depth::Options d;
  std::string contours = "off";   // contour lines for the vector chart: off, auto, a spacing or a list
  double lineMin = -1, lineMax = -1;   // --contour-range: lines only between these depths (m); -1 = all
  bool imagery = true;            // the coloured, shaded picture (imagery layer)
  bool pictureContours = true;    // with --contours, also draw the lines into the picture
  bool fixedContours = false;     // the same contour spacing at every depth (down to the colour range)
};

std::string gNewOutDir;   // output folder this run created (removed again if the run fails and leaves it empty)

int ExpOf(double mpp) { return int(std::lround(std::log2(mpp))); }

std::string Lower(std::string s) { for (auto& c : s) c = char(tolower((unsigned char)c)); return s; }

// Atlas description: the tag a plotter uses to tell charts on one card apart (IMC's "Atlas Description",
// e.g. M=SatMapsQLD). Default M=<input file name>, so every chart gets its own; a plain name gets the M= prefix,
// a given tag (with '=') or an explicit empty one is kept as is.
// IMC "Project Based Naming" puts a project (work folder) name in every output file name:
// <project>_Large.at5, <project>_Medium.at5, _3DTexture_<project>_<level>.at5 (+ _0, _1.. when split).
// Kept to letters, digits and '-' so the level and part numbers after it stay unambiguous.
std::string ProjectName(const std::string& s) {
  std::string t;
  for (char c : s) if (isalnum((unsigned char)c) || c == '-') t += c;
  if (t.size() > 24) t.resize(24);
  if (t.empty() || t.find_first_not_of("0123456789-") == std::string::npos)
    throw std::runtime_error("--project needs a name with letters in it, got '" + s + "'");
  return t;
}

std::string AtlasName(const std::string& desc, bool set, const std::string& input) {
  if (set && (desc.empty() || desc.find('=') != std::string::npos)) return desc;
  std::string t;
  for (char c : set ? desc : fs::path(input).stem().string())
    if (isalnum((unsigned char)c) || c == '-' || c == '_') t += c;
  if (t.size() > 24) t.resize(24);
  return "M=" + (t.empty() ? std::string("SatMap") : t);
}

std::vector<std::string> WriteLevel(int e, bool top, LevelStore& st, const fs::path& outDir, const at5::WriteOptions& wo,
                                    uint64_t splitBytes, const std::string& project) {
  std::map<std::pair<int64_t, int64_t>, at5::BlockPlan> blocks;  // key (gy,gx)
  for (auto& t : st.tiles) {
    int64_t gx = t.tx >= 0 ? t.tx / at5::kGrid : -((-t.tx + at5::kGrid - 1) / at5::kGrid);
    int64_t gy = t.ty >= 0 ? t.ty / at5::kGrid : -((-t.ty + at5::kGrid - 1) / at5::kGrid);
    auto& b = blocks[{gy, gx}];
    b.gx = gx; b.gy = gy;
    b.cells.push_back({int((t.ty - gy * at5::kGrid) * at5::kGrid + (t.tx - gx * at5::kGrid)), t.off});
  }
  std::vector<std::vector<at5::BlockPlan>> files(1);
  uint64_t acc = 0;
  for (auto& kv : blocks) {
    uint64_t s = at5::BlockEncodedSize(e, kv.second);
    if (!files.back().empty() && acc + s > splitBytes) { files.emplace_back(); acc = 0; }
    files.back().push_back(kv.second);
    acc += s;
  }
  std::vector<std::string> names(files.size());
  std::vector<std::thread> th;
  std::exception_ptr err; std::mutex em;
  for (size_t i = 0; i < files.size(); ++i) {
    std::string nm = "_3DTexture_" + (project.empty() ? "" : project + "_") + at5::LevelName(e) +
                     (files.size() > 1 ? "_" + std::to_string(i) : "") + ".at5";
    names[i] = (outDir / nm).string();
    th.emplace_back([&, i] {
      try {
        FILE* f = nullptr;
        if (fopen_s(&f, st.path.c_str(), "rb") || !f) throw std::runtime_error("cannot reopen tile store");
        at5::WriteTextureFile(names[i], e, top, files[i], f, wo);
        fclose(f);
      } catch (...) { std::lock_guard<std::mutex> g(em); if (!err) err = std::current_exception(); }
    });
  }
  for (auto& t : th) t.join();
  if (err) std::rethrow_exception(err);
  return names;
}

int Convert(const ConvertOpts& o, std::unique_ptr<src::Source> given = nullptr) {
  auto tStart = Clock::now();
  src::OpenOptions oo; oo.zoom = o.zoom; oo.crs = o.crs;
  auto source = given ? std::move(given) : src::Open(o.input, oo);
  auto b = source->Bounds();
  printf("Source: %s\n  bounds lon %.6f..%.6f lat %.6f..%.6f\n  native %.3f m/px (Lowrance Mercator)\n", source->Describe().c_str(),
         b.lonMin, b.lonMax, b.latMin, b.latMax, source->NativeMpp());
  int finest = o.finestMpp > 0 ? ExpOf(o.finestMpp) : int(std::floor(std::log2(source->NativeMpp()) + 1e-9));
  finest = std::clamp(finest, -1, kTopExp);
  int coarsest = std::clamp(ExpOf(o.coarsestMpp), finest, kTopExp);
  int threads = o.threads > 0 ? o.threads : int(std::max(1u, std::thread::hardware_concurrency()));
  printf("Levels: %s .. %s m/px (%d levels)\n", at5::LevelName(finest).c_str(), at5::LevelName(coarsest).c_str(), coarsest - finest + 1);
  {
    // Disk space: the finest level's tiles over the map's area (32 KB each), plus about a third for the coarser
    // levels; the work files need about as much again while converting. Checked now, not 10 minutes in.
    const double R = at5::kLowranceR;
    const double w = (b.lonMax - b.lonMin) * kPi / 180 * R;
    const double h = R * (std::log(std::tan(kPi / 4 + b.latMax * kPi / 360)) - std::log(std::tan(kPi / 4 + b.latMin * kPi / 360)));
    const double tile = 256 * std::ldexp(1.0, finest);
    const double need = (std::ceil(w / tile) + 1) * (std::ceil(h / tile) + 1) * 32768.0 * 4 / 3;
    std::error_code ec;
    fs::path probe = fs::absolute(o.outDir, ec);
    while (!probe.empty() && !fs::exists(probe, ec) && probe.has_parent_path() && probe.parent_path() != probe) probe = probe.parent_path();
    const auto sp = fs::space(probe, ec);
    if (!ec) {
      const double freeGb = double(sp.available) / 1e9, needGb = need / 1e9;
      printf("  chart size: up to %.1f GB (plus about as much again while converting); %.1f GB free on that drive\n", needGb, freeGb);
      if (need > double(sp.available) && !o.ignoreSpace)
        throw std::runtime_error("not enough disk space: this chart needs up to " + std::to_string(int(std::ceil(needGb))) + " GB and the drive has " +
                                 std::to_string(int(freeGb)) + " GB free.\n  Choose a coarser Detail (each step coarser is about 4x smaller), a smaller area,"
                                 " or a Save to folder on a drive with more space. (--ignore-space converts anyway.)");
      if (need * 2 > double(sp.available))
        printf("  WARNING: the drive may fill up while converting (the work files need about as much space again)\n");
    }
  }
  fs::create_directories(o.outDir);
  fs::path tmp = fs::path(o.outDir) / "_sat2at5_tmp";
  fs::create_directories(tmp);
  struct TmpGone { fs::path p; ~TmpGone() { std::error_code ec; fs::remove_all(p, ec); } } tmpGone{tmp};   // also on errors
  std::vector<std::string> written;
  {
    Pyramid p(*source, finest, coarsest, threads, tmp);
    p.Run();
    at5::WriteOptions wo; wo.date = TodayLong(); wo.desc = o.desc; wo.usage = o.usage;
    auto tw = Clock::now();
    printf("Writing chart files...\n");
    for (auto& kv : p.Stores()) {
      if (kv.second->tiles.empty()) continue;
      auto names = WriteLevel(kv.first, kv.first == kTopExp, *kv.second, o.outDir, wo, uint64_t(o.splitMb * 1e6), o.project);
      written.insert(written.end(), names.begin(), names.end());
    }
    if (written.empty())
      throw std::runtime_error("no map picture could be read from the file - its tiles are damaged, or in an image format "
                               "Windows cannot open (for WebP install Microsoft's free 'WebP Image Extensions')");
    printf("Wrote %zu file(s) in %.1f s\n", written.size(), std::chrono::duration<double>(Clock::now() - tw).count());
    if (const long long bad = src::BadTiles())
      printf("WARNING: %lld source tile(s) could not be read (damaged or an unsupported image format) and are left blank\n", bad);
  }
  std::error_code ec;
  fs::remove_all(tmp, ec);
  uint64_t total = 0;
  for (auto& n : written) { auto sz = fs::file_size(n); total += sz; printf("  %-40s %12llu bytes\n", fs::path(n).filename().string().c_str(), (unsigned long long)sz); }
  // texture files of this chart from an earlier run left in the folder would mix into it on the card
  // (other charts' files - another project name, or vector files - may share the folder)
  std::vector<std::string> stale;
  const std::string mine = "_3dtexture_" + Lower(o.project.empty() ? "" : o.project + "_");
  for (auto& e : fs::directory_iterator(o.outDir)) {
    const std::string n = Lower(e.path().filename().string());
    if (e.path().extension() != ".at5" || n.rfind(mine, 0) != 0 || n.size() <= mine.size()) continue;
    const char c = n[mine.size()];
    if ((isdigit((unsigned char)c) || (c == '1' && n.size() > mine.size() + 1 && n[mine.size() + 1] == 'd')) &&
        std::none_of(written.begin(), written.end(), [&](const std::string& w) { return Lower(fs::path(w).filename().string()) == n; }))
      stale.push_back(e.path().filename().string());
  }
  if (!stale.empty()) {
    printf("WARNING: %zu older .at5 file(s) in this folder are not part of this chart - delete them before copying:\n", stale.size());
    for (auto& s : stale) printf("  %s\n", s.c_str());
  }
  printf("Done: %.1f MB in %.1f s total. Copy the .at5 files to the top of the chart card.\n", total / 1e6,
         std::chrono::duration<double>(Clock::now() - tStart).count());
  return 0;
}

// A depth GeoTIFF: the coloured, shaded picture (imagery layer) and, when asked, a depth chart from contour
// lines (vector layer), both in the one output folder.
int ConvertDepth(ConvertOpts o) {
  printf("Depth GeoTIFF: %s\n", o.input.c_str());
  o.d.crs = o.crs;
  auto grid = depth::Load(o.input, o.d);
  auto levels = depth::ContourLevels(*grid, o.contours, o.d.rangeMax > o.d.rangeMin ? o.d.rangeMax : depth::AutoRangeMax(*grid), o.fixedContours);
  if (o.lineMax > o.lineMin && !levels.empty()) {   // --contour-range: lines (and colour steps) only between two depths
    std::vector<double> kept;
    for (double v : levels) if (v >= o.lineMin - 1e-9 && v <= o.lineMax + 1e-9) kept.push_back(v);
    printf("  contour lines only from %g to %g m (--contour-range): %zu of %zu depths\n", o.lineMin, o.lineMax, kept.size(), levels.size());
    if (kept.empty()) throw std::runtime_error("--contour-range leaves no contour lines: no contour depths lie between those two depths");
    levels.swap(kept);
  }
  if (!o.imagery && levels.empty()) throw std::runtime_error("--contours-only needs contour levels (e.g. --contours 5)");
  if (o.pictureContours) o.d.pictureContours = levels;   // the lines also go into the picture, so Photo overlay shows them
  if (o.imagery) {
    auto img = depth::MakeImagery(grid, o.d);
    // One level finer than the survey by default: colours and hillshade are worked out from the depth grid at
    // every pixel, so zooming in on the plotter stays smooth instead of showing stretched blocks.
    if (o.finestMpp <= 0) o.finestMpp = std::max(0.5, std::exp2(std::floor(std::log2(img->NativeMpp()) + 1e-9) - 1));
    const int rc = Convert(o, std::move(img));
    if (rc) return rc;
  }
  if (levels.empty()) {
    printf("No contour lines (add --contours 5, or --contours auto, for a depth chart with contours too)\n");
    return 0;
  }
  std::string list;
  for (size_t i = 0; i < levels.size() && i < 12; ++i) { char b[32]; snprintf(b, sizeof b, "%s%g", i ? ", " : "", levels[i]); list += b; }
  if (levels.size() > 12) list += ", ...";
  printf("\nDepth chart: contour lines at %zu depths (%s m)\n", levels.size(), list.c_str());
  fs::create_directories(o.outDir);
  const std::string tmp = (fs::path(o.outDir) / "_sat2at5_contours.mbtiles").string();
  struct Gone { std::string p; ~Gone() { std::error_code ec; fs::remove(p, ec); } } gone{tmp};
  const double dryTo = o.v.dryingHeight;   // --drying-height: for a survey, water shallower than this dries (green)
  if (dryTo > 0) printf("  water shallower than %g m is shown as drying (green); --drying-height 0 turns this off\n", dryTo);
  const bool keep = o.d.clipToRange && o.lineMax > o.lineMin;   // --only-ranges: no water outside the line depths
  if (keep) printf("  only water from %g to %g m gets colours in the chart (--only-ranges)\n", o.lineMin, o.lineMax);
  const size_t n = depth::WriteContours(*grid, levels, tmp, dryTo, keep ? o.lineMin : 0, keep ? o.lineMax : -1);
  printf("  %zu contour lines\n", n);
  if (n == 0) { printf("No contour lines fall inside this survey's depths - no depth chart made.\n"); return 0; }
  vec::Options v = o.v;
  v.elevation = false; v.elevationAuto = false;   // the contour file holds depths
  // The water between the contour lines is coloured (automatic, as for any contour chart): plotters draw chart
  // areas without depth colours as land. --no-areas-from-contours makes a lines-only chart.
  if (!o.project.empty()) { v.name = o.project + "_" + v.name; v.overviewName = o.project + "_" + v.overviewName; }
  v.zoom = -1; v.threads = o.threads; v.splitMb = o.splitMb; v.desc = o.desc;
  return vec::Convert(tmp, o.outDir, v, TodayLong());
}

int CmdVerify(const std::vector<std::string>& paths, bool verbose) {
  int bad = 0, n = 0;
  for (auto& p : paths) {
    std::vector<std::string> files;
    if (fs::is_directory(p)) { for (auto& e : fs::directory_iterator(p)) if (e.path().extension() == ".at5") files.push_back(e.path().string()); }
    else files.push_back(p);
    std::sort(files.begin(), files.end());
    for (auto& f : files) { ++n; bool ok = at5::Verify(f, verbose); if (vec::VerifyBody(f) == 0) ok = false; if (!ok) ++bad; }
  }
  printf("%d file(s) checked, %d invalid\n", n, bad);
  return bad ? 1 : 0;
}

int CmdRebuild(const std::string& in, const std::string& out) {
  auto ex = at5::Extract(in);
  fs::path tmp = fs::path(out).parent_path() / "_rebuild_store.bin";
  FILE* st = nullptr;
  fopen_s(&st, tmp.string().c_str(), "wb+");
  if (!st) throw std::runtime_error("cannot create store");
  std::vector<at5::BlockPlan> plans;
  uint64_t off = 0;
  for (auto& b : ex.blocks) {
    at5::BlockPlan p; p.gx = b.first.first; p.gy = b.first.second;
    for (auto& t : b.second) { fwrite(t.second.data(), 1, t.second.size(), st); p.cells.push_back({t.first, off}); off += t.second.size(); }
    plans.push_back(std::move(p));
  }
  fflush(st);
  at5::WriteOptions wo; wo.date = ex.date; wo.desc = ex.desc; wo.b1 = ex.b1;
  at5::WriteTextureFile(out, ex.levelExp, ex.top, plans, st, wo);
  fclose(st);
  fs::remove(tmp);
  printf("rebuilt %s -> %s (level %s, %zu blocks)\n", in.c_str(), out.c_str(), at5::LevelName(ex.levelExp).c_str(), ex.blocks.size());
  return 0;
}

void DecodeDxt1Tile(const uint8_t* d, uint8_t* rgba /*256x256, row0 south*/) {
  for (int by = 0; by < 64; ++by)
    for (int bx = 0; bx < 64; ++bx, d += 8) {
      uint16_t c0 = uint16_t(d[0] | d[1] << 8), c1 = uint16_t(d[2] | d[3] << 8);
      uint32_t bits = uint32_t(d[4] | d[5] << 8 | d[6] << 16 | uint32_t(d[7]) << 24);
      int p[4][3]; dxt1::From565(c0, p[0]); dxt1::From565(c1, p[1]);
      for (int c = 0; c < 3; ++c) {
        if (c0 > c1) { p[2][c] = (2 * p[0][c] + p[1][c]) / 3; p[3][c] = (p[0][c] + 2 * p[1][c]) / 3; }
        else { p[2][c] = (p[0][c] + p[1][c]) / 2; p[3][c] = 0; }
      }
      for (int i = 0; i < 16; ++i) {
        int k = (bits >> (2 * i)) & 3;
        uint8_t* o = rgba + (size_t(by * 4 + i / 4) * 256 + bx * 4 + i % 4) * 4;
        o[0] = uint8_t(p[k][0]); o[1] = uint8_t(p[k][1]); o[2] = uint8_t(p[k][2]);
        o[3] = (c0 <= c1 && k == 3) ? 0 : 255;   // three-colour mode's index 3: see-through (no data)
      }
    }
}

// _3DTexture_[<project>_]<level>[_<part>].at5 -> level name ("16", "1D2") and part (-1 = not split)
bool TextureFileName(const std::string& n, std::string& level, int& part) {
  std::string l = n;
  for (auto& c : l) c = char(tolower((unsigned char)c));
  if (l.rfind("_3dtexture_", 0) != 0 || l.size() < 16 || l.compare(l.size() - 4, 4, ".at5") != 0) return false;
  std::vector<std::string> tok;
  std::string rest = n.substr(11, n.size() - 15);
  for (size_t a = 0, b; a <= rest.size(); a = b + 1) { b = rest.find('_', a); if (b == std::string::npos) b = rest.size(); tok.push_back(rest.substr(a, b - a)); }
  auto num = [](const std::string& t) { return !t.empty() && t.find_first_not_of("0123456789") == std::string::npos; };
  auto lev = [&](const std::string& t) { return num(t) || (t.size() > 2 && (t[0] == '1') && (t[1] == 'D' || t[1] == 'd') && num(t.substr(2))); };
  part = -1;
  if (tok.size() >= 2 && num(tok.back()) && lev(tok[tok.size() - 2])) { part = std::stoi(tok.back()); level = tok[tok.size() - 2]; return true; }
  if (lev(tok.back())) { level = tok.back(); return true; }
  return false;
}

// A folder gives a whole-chart overview: [<project>_]Large*.at5 (vector), else the finest texture level kept in
// one file (a level split into _0,_1.. parts has each part cover only a strip of the chart).
std::string PreviewSource(const std::string& in) {
  if (!fs::is_directory(in)) return in;
  std::string vec, med, tex;
  long long texLevel = -1;
  for (auto& e : fs::directory_iterator(in)) {
    if (e.path().extension() != ".at5") continue;
    const std::string n = e.path().filename().string();
    std::string l = n;
    for (auto& c : l) c = char(tolower((unsigned char)c));
    std::string level; int part = -1;
    if (TextureFileName(n, level, part)) {
      const long long lv = std::isdigit((unsigned char)level[0]) && level.find_first_not_of("0123456789") == std::string::npos ? std::stoll(level) : 0;
      if (part < 0 && lv >= 16 && (texLevel < 0 || lv < texLevel)) { texLevel = lv; tex = e.path().string(); }
    }
    else if (l.rfind("large", 0) == 0 || l.find("_large") != std::string::npos) { if (vec.empty() || e.path().string() < vec) vec = e.path().string(); }
    else if (l.rfind("medium", 0) == 0 || l.find("_medium") != std::string::npos) med = e.path().string();
  }
  if (!vec.empty()) return vec;
  if (!med.empty()) return med;
  if (tex.empty()) throw std::runtime_error("no .at5 chart files in " + in);
  return tex;
}

int CmdPreview(const std::string& in, const std::string& out, int maxPx) {
  auto ex = at5::Extract(in);
  {
    const std::string n = fs::path(in).filename().string();
    std::string level; int part = -1;
    if (TextureFileName(n, level, part) && part >= 0)
      printf("note: %s is one part of a level split into several files, so it shows only part of the chart.\n"
             "      Give the output folder instead for a picture of the whole chart.\n", n.c_str());
  }
  int64_t x0 = INT64_MAX, x1 = INT64_MIN, y0 = INT64_MAX, y1 = INT64_MIN;
  for (auto& b : ex.blocks)
    for (auto& t : b.second) {
      int64_t tx = b.first.first * 20 + t.first % 20, ty = b.first.second * 20 + t.first / 20;
      x0 = std::min(x0, tx); x1 = std::max(x1, tx); y0 = std::min(y0, ty); y1 = std::max(y1, ty);
    }
  int64_t nx = x1 - x0 + 1, ny = y1 - y0 + 1;
  int tp = 256;
  while (tp > 1 && (nx * tp > maxPx || ny * tp > maxPx)) tp /= 2;
  int W = int(nx * tp), H = int(ny * tp);
  std::vector<uint8_t> img(size_t(W) * H * 4, 0), tile(256 * 256 * 4);
  const int f = 256 / tp;
  for (auto& b : ex.blocks)
    for (auto& t : b.second) {
      int64_t tx = b.first.first * 20 + t.first % 20, ty = b.first.second * 20 + t.first / 20;
      DecodeDxt1Tile(t.second.data(), tile.data());
      for (int j = 0; j < tp; ++j)
        for (int i = 0; i < tp; ++i) {
          const uint8_t* s = &tile[(size_t(j * f) * 256 + i * f) * 4];
          int row = int((y1 - ty) * tp) + (tp - 1 - j);  // north-up
          uint8_t* d = &img[(size_t(row) * W + size_t((tx - x0) * tp + i)) * 4];
          memcpy(d, s, 4);
        }
    }
  int n = MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, nullptr, 0);
  std::wstring w(size_t(n), 0); MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, &w[0], n);
  if (!src::SavePng(w.c_str(), img.data(), W, H)) throw std::runtime_error("PNG save failed");
  printf("preview %s: %dx%d (%d px per tile), level %s, north-up\n", out.c_str(), W, H, tp, at5::LevelName(ex.levelExp).c_str());
  return 0;
}

void Usage() {
  printf(
      "sat2at5 - satellite imagery to Simrad/Lowrance AT5 (3D texture layers)\n\n"
      "  sat2at5 <input.mbtiles|image.tif|.jpg|.png> [-o outdir] [options]\n"
      "      --finest M     finest level in m/px (0.5,1,2,4...; default: auto from source resolution)\n"
      "      --coarsest M   coarsest level in m/px (default 8192)\n"
      "      --zoom Z       MBTiles zoom to read (default: max zoom)\n"
      "      --crs 4326|3857  CRS for world-file images without GeoTIFF keys (default: guess)\n"
      "      --desc NAME    chart (atlas) name the plotter tells charts apart by; stored as M=NAME\n"
      "                     (default: M=<input file name>; give each chart on a card its own)\n"
      "      --threads N    worker threads (default: all cores)\n"
      "      --split-mb N   max size per output file before splitting into _0,_1.. (default 450)\n"
      "      --project NAME put NAME in every file name, as IMC's 'Project Based Naming' does:\n"
      "                     NAME_Large.at5, NAME_Medium.at5, _3DTexture_NAME_16.at5 (lets charts share a folder)\n"
      "\n  Vector MBTiles (pbf/MVT) -> depth areas, land and depth contours (IMC 'Large' + 'Medium' layout):\n"
      "  sat2at5 <vector.mbtiles> [-o outdir] [--list] [--name Large] [--colours s52|navionics|D:RRGGBB,...]\n"
      "      --list            show layers/attributes found in the tiles, then stop\n"
      "      --colours S       depth band colours (true RGB, Atlas v12 RGBA fills). Presets:\n"
      "                          s52       (default) IHO S-52 chart colours: drying green, blues, near-white deep\n"
      "                          navionics Navionics chart look: yellow land, lime drying, strong -> pale blue one step\n"
      "                                    per contour (5 m or 1 m data), white from --deep-depth (default 20)\n"
      "                          gradient  soft blue gradient, one step per contour, white from --deep-depth\n"
      "                          navionics-classic  the earlier fixed 5-band Navionics-like scheme\n"
      "                        or custom: min:83B295,0:73B6EF,2:98C5F2,5:BAD5E1,10:D4EAEE  (start depth m : RRGGBB)\n"
      "      --safety-depth D  water 0..D m in the shallow colour (--shallow-colour, default F2A0A0) and the D m\n"
      "                        contour drawn bold (--safety-colour, default 1E1E1E), like a plotter's safety depth\n"
      "      --highlight A-B[:RRGGBB]  depth range shading, e.g. 10-20 (fishing range, default colour FFB347)\n"
      "      --land-colour / --contour-colour / --fine-contour-colour RRGGBB   (default S-52 C9B97A / 7D898C / same;\n"
      "                        navionics: FFEF73 / 404040 / A8A8A8)\n"
      "      --land-layers L   polygon layers drawn as Land (default: names containing land/lndare/island)\n"
      "      --depth-layers L / --depth-attr K     depth polygons (default: any layer with drval1/depth/...)\n"
      "      --contour-layers L / --contour-attr K depth contour lines (default: valdco/depth/elevation/...)\n"
      "      --elevation / --no-elevation   values are heights, depth = -value (default: detected)\n"
      "      --land auto|world|australia|FILE   coastline: land is drawn and the shore is known. auto = the\n"
      "                        built-in world coastline (GSHHG, with Geoscience Australia detail around Australia),\n"
      "                        left out by itself for inland (lake, dam, river) charts; world = the same, always;\n"
      "                        australia = Geoscience Australia only; FILE = your own .shp (lon/lat) or .land\n"
      "      --drying-height H coastline height above chart datum when there is no 0 m contour (default 1.5 m):\n"
      "                        gives the green drying strip along the shore; 0 = none\n"
      "      --areas-from-contours / --no-areas-from-contours   colour the water between the contour lines\n"
      "                        (default: automatic when the tiles have contours but no depth-area polygons)\n"
      "      --major-contours N   contours at multiples of N m show at every zoom (default 5)\n"
      "      --fine-contours close|medium|always|never   when the other (e.g. 1 m) contours appear (default close)\n\n"
      "  Depth GeoTIFF (one band of depths or heights, e.g. a BathyPicker or survey file) -> a coloured, shaded\n"
      "  depth picture (imagery layer) and, with --contours, a depth chart with contour lines (vector layer):\n"
      "  sat2at5 <survey.tif> [-o outdir] [--depth-range 0-200] [--palette spectrum] [--hillshade 60] [--contours 5]\n"
      "      --depth-range A-B  spread the colours over A..B m (default 0 .. about the deepest 2 %%); deeper water\n"
      "                        shows in the deepest colour. A small range shows the shallows in more detail\n"
      "      --palette P       spectrum (default), lagoon, thermal, chart or grey\n"
      "      --shade X         shading strength, as in the SeaBathy apps: 0 (flat colours), 0.5, 1 (default), 2 .. 50\n"
      "                        (stronger shows smaller bumps, darker)\n"
      "      --hillshade N     older form: shading amount 0 .. 100 at the --relief strength\n"
      "      --relief N        slope exaggeration for the shading (default 5, which is --shade 1)\n"
      "      --finest M        finest detail in m per pixel (default: one level finer than the survey, for smooth zooming)\n"
      "      --hide-above-water  leave ground above chart datum (islands, drying reef) clear instead of sand\n"
      "      --contours S      also make the depth chart: S = a spacing in m (5), a list (2,5,10,20,50) or auto;\n"
      "                        uses the vector options above (--colours, --land, --safety-depth ...). Default off\n"
      "                        A spacing applies in the shallows and widens in deep water (5 m past 50 m, 10 m past\n"
      "                        200 m, 50 m past 500 m, 100 m past 1000 m), never finer than asked for\n"
      "      --fixed-contours  the same spacing at every depth instead, down to the bottom of --depth-range\n"
      "      --contour-range A-B  contour lines only from A to B m (e.g. 0-50); the chart's colour steps follow\n"
      "      --only-ranges     leave out water outside the ranges: the picture shows only --depth-range,\n"
      "                        the chart colours only --contour-range (the rest is see-through)\n"
      "      --contours-only   make just the depth chart, not the picture\n"
      "      --no-picture-contours  with --contours, keep the lines out of the picture (they're drawn into it by default,\n"
      "                        so the plotter's Photo overlay shows them too)\n"
      "      --elevation / --no-elevation   values are heights / depths (default: detected)\n"
      "      --as-image        treat the file as an ordinary picture instead\n\n"
      "  sat2at5 verify <file.at5|folder>... [-v]   check headers, checksums, keys and tile structure\n"
      "  sat2at5 preview <file.at5|folder> <out.png> [maxpx] [--no-lines]   render north-up (--no-lines: vector colours only;\n"
      "      a folder shows the whole chart)\n"
      "  sat2at5 rebuild <in.at5> <out.at5>   re-encode an existing texture file (writer self-test)\n");
}

}  // namespace

int main(int argc, char** argv) {
  SetConsoleOutputCP(CP_UTF8);
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::vector<std::string> a(argv + 1, argv + argc);
  if (a.empty() || a[0] == "-h" || a[0] == "--help") { Usage(); return a.empty() ? 1 : 0; }
  try {
    if (a[0] == "info" && a.size() == 2) {   // what kind of input this is (used by the window program)
      std::error_code ec;
      const std::string ext = Lower(fs::path(a[1]).extension().string());
      if (!fs::exists(a[1], ec)) printf("missing\n");
      else if (fs::is_directory(a[1], ec)) printf("folder\n");
      else if (ext == ".mbtiles" || ext == ".sqlite" || ext == ".db") {
        char head[16] = {};
        FILE* f = nullptr;
        if (!fopen_s(&f, a[1].c_str(), "rb") && f) { fread(head, 1, 16, f); fclose(f); }
        if (memcmp(head, "SQLite format 3", 16) != 0) printf("invalid\n");
        else if (vec::IsVectorMbtiles(a[1])) printf("depth\n");
        else if (!vec::RasterDemEncoding(a[1]).empty()) printf("elevation\n");
        else printf("picture\n");
      } else if ((ext == ".tif" || ext == ".tiff") && depth::IsDepthTiff(a[1])) printf("bathymetry\n");
      else if (ext == ".tif" || ext == ".tiff" || ext == ".jpg" || ext == ".jpeg" || ext == ".png") printf("picture\n");
      else printf("unknown\n");
      return 0;
    }
    if (a[0] == "verify") {
      bool v = std::find(a.begin(), a.end(), "-v") != a.end();
      std::vector<std::string> p;
      for (size_t i = 1; i < a.size(); ++i) if (a[i] != "-v") p.push_back(a[i]);
      return CmdVerify(p, v);
    }
    if (a[0] == "rebuild" && a.size() == 3) return CmdRebuild(a[1], a[2]);
    if (a[0] == "check-areas" && a.size() >= 2) {   // developer check: area shapes in finished charts
      for (size_t i = 1; i < a.size(); ++i) vec::CheckAreas(a[i]);
      return 0;
    }
    if (a[0] == "preview" && a.size() >= 3) {
      bool lines = true;
      for (size_t i = 3; i < a.size(); ++i) if (a[i] == "--no-lines") { lines = false; a.erase(a.begin() + long(i)); break; }
      double box[4]; bool hasBox = false;
      for (size_t i = 3; i + 1 < a.size(); ++i)
        if (a[i] == "--bbox") {   // WEST,SOUTH,EAST,NORTH in degrees
          if (sscanf_s(a[i + 1].c_str(), "%lf,%lf,%lf,%lf", &box[0], &box[1], &box[2], &box[3]) != 4) throw std::runtime_error("--bbox needs WEST,SOUTH,EAST,NORTH, e.g. 145.3,-17.5,146.6,-16.2");
          hasBox = true; a.erase(a.begin() + long(i), a.begin() + long(i + 2)); break;
        }
      int px = a.size() > 3 ? std::stoi(a[3]) : 4096;
      a[1] = PreviewSource(a[1]);
      if (fs::is_directory(a[2])) a[2] = (fs::path(a[2]) / "_preview.png").string();
      if (vec::PreviewVector(a[1], a[2], std::min(px, 8192), lines, hasBox ? box : nullptr) == 1) return 0;
      return CmdPreview(a[1], a[2], px);
    }
    ConvertOpts o;
    bool navPreset = false, landSet = false, contourSet = false, minorSet = false;
    for (size_t i = 0; i < a.size(); ++i) {
      auto val = [&]() -> std::string { if (i + 1 >= a.size()) throw std::runtime_error("missing value for " + a[i]); return a[++i]; };
      // a number for this option (whole text must be a number), with a readable error otherwise
      auto numOf = [&](const std::string& opt, const std::string& v) -> double {
        size_t used = 0; double d = 0;
        try { d = std::stod(v, &used); } catch (...) { used = 0; }
        if (used == 0 || used != v.size() || !std::isfinite(d)) throw std::runtime_error(opt + " needs a number, got '" + v + "'");
        return d;
      };
      auto num = [&]() { const std::string opt = a[i]; return numOf(opt, val()); };
      auto positive = [&]() { const std::string opt = a[i]; const double d = numOf(opt, val()); if (d <= 0) throw std::runtime_error(opt + " must be greater than 0"); return d; };
      if (a[i] == "-o" || a[i] == "--out") o.outDir = val();
      else if (a[i] == "--finest") o.finestMpp = positive();
      else if (a[i] == "--coarsest") o.coarsestMpp = positive();
      else if (a[i] == "--zoom") o.zoom = int(num());
      else if (a[i] == "--crs") o.crs = int(num());
      else if (a[i] == "--desc") { o.desc = val(); o.descSet = true; }
      else if (a[i] == "--threads") o.threads = int(num());
      else if (a[i] == "--split-mb") o.splitMb = positive();
      else if (a[i] == "--usage") o.usage = val();
      else if (a[i] == "--land-layers") o.v.landLayers = val();
      else if (a[i] == "--depth-layers") o.v.depthLayers = val();
      else if (a[i] == "--contour-layers") o.v.contourLayers = val();
      else if (a[i] == "--depth-attr") o.v.depthAttr = val();
      else if (a[i] == "--contour-attr") o.v.contourAttr = val();
      else if (a[i] == "--elevation") { o.v.elevation = true; o.v.elevationAuto = false; o.d.elevation = 1; }
      else if (a[i] == "--no-elevation") { o.v.elevation = false; o.v.elevationAuto = false; o.d.elevation = 0; }
      else if (a[i] == "--depth-range") {   // A-B metres
        const std::string opt = a[i], v = val();
        const size_t dash = v.find('-', 1);
        if (dash == std::string::npos) throw std::runtime_error("--depth-range needs FROM-TO in metres, e.g. 0-200");
        o.d.rangeMin = numOf(opt, v.substr(0, dash)); o.d.rangeMax = numOf(opt, v.substr(dash + 1));
        if (o.d.rangeMin > o.d.rangeMax) std::swap(o.d.rangeMin, o.d.rangeMax);
        if (o.d.rangeMax - o.d.rangeMin < 0.1) throw std::runtime_error("--depth-range needs two different depths, e.g. 0-200");
      }
      else if (a[i] == "--palette") o.d.palette = Lower(val());
      else if (a[i] == "--hillshade") {
        const std::string opt = a[i]; const double h = numOf(opt, val());
        if (h < 0 || h > 100) throw std::runtime_error("--hillshade must be 0 to 100");
        o.d.hillshade = h / 100;
      }
      else if (a[i] == "--relief") o.d.relief = positive();
      else if (a[i] == "--shade") {
        const double x = num();
        if (x < 0 || x > 50) throw std::runtime_error("--shade must be 0 to 50, e.g. 1 or 2.5");
        o.d.relief = 5 * std::max(x, 0.01);
        o.d.hillshade = x > 0 ? 1 : 0;
      }
      else if (a[i] == "--hide-above-water") o.d.showAboveWater = false;
      else if (a[i] == "--contours") o.contours = Lower(val());
      else if (a[i] == "--contours-only") { o.imagery = false; if (o.contours == "off") o.contours = "auto"; }
      else if (a[i] == "--no-picture-contours") o.pictureContours = false;
      else if (a[i] == "--fixed-contours") o.fixedContours = true;
      else if (a[i] == "--only-ranges") o.d.clipToRange = true;   // leave out water outside --depth-range / --contour-range
      else if (a[i] == "--contour-range") {   // A-B metres: contour lines only between these depths
        const std::string opt = a[i], v = val();
        const size_t dash = v.find('-', 1);
        if (dash == std::string::npos) throw std::runtime_error("--contour-range needs FROM-TO in metres, e.g. 0-50");
        o.lineMin = numOf(opt, v.substr(0, dash)); o.lineMax = numOf(opt, v.substr(dash + 1));
        if (o.lineMin > o.lineMax) std::swap(o.lineMin, o.lineMax);
        if (o.lineMax - o.lineMin < 0.1) throw std::runtime_error("--contour-range needs two different depths, e.g. 0-50");
      }
      else if (a[i] == "--bands" || a[i] == "--colours" || a[i] == "--colors") {
        const std::string v = val();
        navPreset = v == "navionics";
        if (v == "gradient" || v == "navionics") { o.v.gradient = true; o.v.gradientRamp = navPreset ? 1 : 0; o.v.bands.clear(); }
        else { o.v.gradient = false; o.v.bands = vec::ParseBands(v); }
      }
      else if (a[i] == "--deep-depth") o.v.deepDepth = positive();
      else if (a[i] == "--safety-depth") o.v.safetyDepth = positive();
      else if (a[i] == "--shallow-colour" || a[i] == "--shallow-color") o.v.shallowColour = vec::ParseRgb(val());
      else if (a[i] == "--safety-colour" || a[i] == "--safety-color") o.v.safetyColour = vec::ParseRgb(val());
      else if (a[i] == "--highlight") {   // FROM-TO[:RRGGBB]
        std::string v = val(); size_t c = v.find(':');
        if (c != std::string::npos) { o.v.highlightColour = vec::ParseRgb(v.substr(c + 1)); v = v.substr(0, c); }
        size_t d = v.find('-', 1);
        if (d == std::string::npos) throw std::runtime_error("--highlight needs FROM-TO, e.g. 10-20 or 10-20:FFB347");
        o.v.highlightFrom = numOf("--highlight", v.substr(0, d)); o.v.highlightTo = numOf("--highlight", v.substr(d + 1));
        if (o.v.highlightFrom > o.v.highlightTo) std::swap(o.v.highlightFrom, o.v.highlightTo);
        if (o.v.highlightFrom == o.v.highlightTo) throw std::runtime_error("--highlight needs two different depths, e.g. 10-20");
      }
      else if (a[i] == "--land-colour" || a[i] == "--land-color") { o.v.landColour = vec::ParseRgb(val()); landSet = true; }
      else if (a[i] == "--contour-colour" || a[i] == "--contour-color") { o.v.contourColour = vec::ParseRgb(val()); contourSet = true; }
      else if (a[i] == "--fine-contour-colour" || a[i] == "--fine-contour-color") { o.v.minorContourColour = vec::ParseRgb(val()); minorSet = true; }
      else if (a[i] == "--name") o.v.name = val();
      else if (a[i] == "--list") o.v.list = true;
      else if (a[i] == "--no-overview") o.v.overview = false;
      else if (a[i] == "--areas-from-contours") o.v.areasFromContours = 1;
      else if (a[i] == "--no-areas-from-contours") o.v.areasFromContours = 0;
      else if (a[i] == "--as-image") o.asImage = true;
      else if (a[i] == "--ignore-space") o.ignoreSpace = true;
      else if (a[i] == "--land") o.v.landFile = val();
      else if (a[i] == "--drying-height") { const std::string opt = a[i]; o.v.dryingHeight = numOf(opt, val()); if (o.v.dryingHeight < 0) throw std::runtime_error(opt + " must be 0 or more"); }
      else if (a[i] == "--major-contours") o.v.majorContourEvery = positive();
      else if (a[i] == "--fine-contours") {
        const std::string v = val();
        o.v.fineContourZoom = v == "close" ? 3808 : v == "medium" ? 9330 : v == "always" ? 11425 /* styles::kAllScales */ : v == "never" ? -1 : 0;
        if (o.v.fineContourZoom == 0) {
          try { o.v.fineContourZoom = int(std::lround(numOf("--fine-contours", v) * 256)); } catch (...) { o.v.fineContourZoom = 0; }
        }
        if (o.v.fineContourZoom == 0 || o.v.fineContourZoom < -1 || o.v.fineContourZoom > 32767)
          throw std::runtime_error("--fine-contours must be close, medium, always, never, or an IMC zoom value between 0.01 and 127");
      }
      else if (a[i] == "--overview-zoom") o.v.overviewZoom = int(num());
      else if (a[i] == "--overview-name") o.v.overviewName = val();
      else if (a[i] == "--project") o.project = ProjectName(val());
      else if (!a[i].empty() && a[i][0] == '-') throw std::runtime_error("unknown option " + a[i]);
      else o.input = a[i];
    }
    if (navPreset) {   // Navionics chart look: yellow land, dark-grey main contours, light-grey fine contours
      if (!landSet) o.v.landColour = 0xFFEF73;
      if (!contourSet) o.v.contourColour = 0x404040;
      if (!minorSet) o.v.minorContourColour = 0xA8A8A8;
    }
    if (o.input.empty()) { Usage(); return 1; }
    {
      std::error_code ec;
      if (!fs::exists(o.input, ec)) throw std::runtime_error("file not found: " + o.input);
      if (fs::is_directory(o.input, ec))
        throw std::runtime_error(o.input + " is a folder - choose a map file (.mbtiles, .tif, or .jpg/.png with a world file)");
    }
    if (o.outDir.empty()) o.outDir = (fs::path(o.input).parent_path() / (fs::path(o.input).stem().string() + "_AT5")).string();
    { std::error_code ec; if (!fs::exists(o.outDir, ec)) gNewOutDir = o.outDir; }
    o.desc = AtlasName(o.desc, o.descSet, o.input);
    printf("Chart (atlas) name: %s\n", o.desc.empty() ? "(none)" : o.desc.c_str());
    if (!o.project.empty()) printf("File names: project '%s' (IMC Project Based Naming)\n", o.project.c_str());
    if (vec::IsVectorMbtiles(o.input)) {
      if (!o.project.empty()) { o.v.name = o.project + "_" + o.v.name; o.v.overviewName = o.project + "_" + o.v.overviewName; }
      o.v.zoom = o.zoom; o.v.threads = o.threads; o.v.splitMb = o.splitMb;
      o.v.desc = o.desc;
      const auto runStart = fs::file_time_type::clock::now() - std::chrono::seconds(2);
      const int rc = vec::Convert(o.input, o.outDir, o.v, TodayLong());
      // parts of this chart from an earlier run (e.g. Large_3.at5 after a run that needed fewer parts)
      std::vector<std::string> stale;
      for (const std::string& base : {o.v.name, o.v.overviewName}) {
        const std::string b = Lower(base);
        for (auto& e : fs::directory_iterator(o.outDir)) {
          const std::string n = Lower(e.path().filename().string());
          if (e.path().extension() != ".at5" || e.last_write_time() >= runStart) continue;
          if (n == b + ".at5" || (n.rfind(b + "_", 0) == 0 && n.find_first_not_of("0123456789", b.size() + 1) == n.size() - 4))
            stale.push_back(e.path().filename().string());
        }
      }
      if (!stale.empty()) {
        printf("WARNING: %zu older .at5 file(s) in this folder are not part of this chart - delete them before copying:\n", stale.size());
        for (auto& s : stale) printf("  %s\n", s.c_str());
      }
      return rc;
    }
    if (!o.asImage && fs::path(o.input).extension() == ".mbtiles") {
      const std::string enc = vec::RasterDemEncoding(o.input);
      if (!enc.empty())
        throw std::runtime_error("this MBTiles holds elevation/depth values encoded as colours (" + enc +
                                 " DEM), not a picture.\n  Converting it as satellite imagery would show meaningless colours. Use vector depth\n"
                                 "  MBTiles (contours / depth areas) or a shaded bathymetry image instead, or add --as-image\n"
                                 "  to convert the raw tiles anyway.");
    }
    {
      const std::string ext = Lower(fs::path(o.input).extension().string());
      if (!o.asImage && (ext == ".tif" || ext == ".tiff") && depth::IsDepthTiff(o.input)) return ConvertDepth(o);
    }
    return Convert(o);
  } catch (const std::exception& e) {
    fprintf(stderr, "\nerror: %s\n", e.what());
    std::error_code ec;   // a failed run leaves no empty output folder it made itself
    if (!gNewOutDir.empty() && fs::is_directory(gNewOutDir, ec) && fs::is_empty(gNewOutDir, ec)) fs::remove(gNewOutDir, ec);
    return 2;
  }
}

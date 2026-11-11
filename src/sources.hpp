// Imagery sources: MBTiles (EPSG:3857 tiles) and georeferenced images (GeoTIFF / world file).
#pragma once
#include <cstdint>
#include <memory>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace src {

struct LonLatBox { double lonMin, latMin, lonMax, latMax; };  // degrees

// Per-thread renderer. Renders one 256x256 RGBA output tile in Lowrance Mercator at
// mpp metres/pixel, tile (tx,ty); row 0 = south edge. Returns false if the tile has no data.
class Sampler {
 public:
  virtual ~Sampler() = default;
  virtual bool RenderTile(double mpp, int64_t tx, int64_t ty, uint8_t* rgba) = 0;
};

class Source {
 public:
  virtual ~Source() = default;
  virtual LonLatBox Bounds() const = 0;
  virtual double NativeMpp() const = 0;        // native resolution in Lowrance-Mercator metres/pixel
  virtual std::string Describe() const = 0;
  virtual std::unique_ptr<Sampler> MakeSampler() = 0;   // call from the thread that will use it
};

struct OpenOptions { int zoom = -1; int crs = 0; /* 0 auto, 4326, 3857 */ };
std::unique_ptr<Source> Open(const std::string& path, const OpenOptions& opt);

// WIC helpers shared with the preview command
bool SavePng(const std::wstring& path, const uint8_t* rgba, int w, int h);

// MBTiles (SQLite) access shared with the vector converter. Many threads read one file at once; a file in WAL
// journal mode then reports 'busy' now and then, which must be waited out, never taken as a missing tile.
sqlite3* OpenTilesDb(const std::string& path);                // read-only, with a busy timeout; throws
int Step(sqlite3_stmt* st, bool retry = true);                 // SQLITE_ROW or SQLITE_DONE; throws on errors

long long BadTiles();   // MBTiles image tiles that could not be decoded so far

}  // namespace src

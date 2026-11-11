// AT5 (Navico/Lowrance Atlas v12) 3D-texture container: writer, integrity keys, verifier.
// Writes files compatible with those made by Navico Insight Map Creator 0.7.4.0;
// the layout is described in docs/TECHNICAL.md.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <array>

namespace at5 {

constexpr double kLowranceR = 6356752.3142;   // Lowrance Mercator sphere radius (WGS84 polar)
constexpr double kWebR = 6378137.0;           // EPSG:3857 sphere radius
constexpr int kTilePx = 256;                  // texture tile edge (pixels)
constexpr int kTileDxtBytes = kTilePx * kTilePx / 2;  // DXT1: 32768
constexpr int kGrid = 20;                     // cells per block edge

// Level = metres-per-pixel as power of two exponent: mpp = 2^exp (exp=-1 -> "1D2" = 0.5 m)
std::string LevelName(int exp);
inline double LevelMpp(int exp) { return exp >= 0 ? double(1u << exp) : 1.0 / double(1u << -exp); }

uint16_t Crc16(const uint8_t* p, size_t n, uint16_t crc = 0);
std::array<uint32_t, 4> DeriveKeys(FILE* f, const std::string& usage);  // f positioned anywhere, header must be complete except keys/checksum

struct TileRef { int64_t tx, ty; uint64_t dataOffset; };  // dataOffset into a tile store file
struct BlockPlan { int64_t gx, gy; std::vector<std::pair<int, uint64_t>> cells; };  // local index -> store offset

struct WriteOptions {
  std::string date;          // "Tuesday, March 31, 2026"
  std::string desc;          // atlas description / tag, e.g. "M=SatMap"
  std::string usage = "UnrestrictedUse";
  uint8_t b1 = 5;
};

// Write one .at5 file with the given blocks; tile bytes are read from `store` at the recorded offsets.
// Returns file size. Throws std::runtime_error on failure.
uint64_t WriteTextureFile(const std::string& path, int levelExp, bool topLevel,
                          const std::vector<BlockPlan>& blocks, FILE* store, const WriteOptions& opt);

// Size of one block's encoded bytes (for splitting decisions)
uint64_t BlockEncodedSize(int levelExp, const BlockPlan& b);

// Generic writer: fixed Atlas sections + caller's type library + pre-encoded blocks (read from `store`).
struct BlockRef { uint64_t storeOffset, size; };
uint64_t WriteBlocksFile(const std::string& path, const std::vector<uint8_t>& typeLib, uint32_t typeLibHeaderLen,
                         const std::vector<BlockRef>& blocks, FILE* store, const int32_t bbox[4], uint32_t fieldB8,
                         const WriteOptions& opt);

// Full verification of an .at5 file (header, checksum, CRC, keys, texture body structure).
bool Verify(const std::string& path, bool verbose);

// Extract tiles (gx,gy,local index -> 32 KB DXT1) from a texture .at5 (for rebuild/preview)
struct ExtractedFile {
  int levelExp = 0; std::string name, date, desc; uint8_t b1 = 5; bool top = false;
  std::vector<std::pair<std::pair<int64_t, int64_t>, std::map<int, std::vector<uint8_t>>>> blocks;
};
ExtractedFile Extract(const std::string& path);

}  // namespace at5

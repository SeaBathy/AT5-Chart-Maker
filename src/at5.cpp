#include "at5.hpp"
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace at5 {

// ---------------------------------------------------------------- small helpers
static void Put8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
static void Put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
static void Put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
static void PutF64(std::vector<uint8_t>& b, double d) { uint64_t u; memcpy(&u, &d, 8); for (int i = 0; i < 8; ++i) b.push_back(uint8_t(u >> (8 * i))); }
static void PutBytes(std::vector<uint8_t>& b, const void* p, size_t n) { auto c = (const uint8_t*)p; b.insert(b.end(), c, c + n); }

// AT5 variable-length integer: tag in the low bits of the first byte.
static void PutVar(std::vector<uint8_t>& b, uint32_t v) {
  if (v < 0x80) { Put8(b, uint8_t((v << 1) | 1)); return; }
  if (v < 0x4000) { Put16(b, uint16_t((v << 2) | 2)); return; }
  if (v < 0x200000) { uint32_t w = (v << 3) | 4; Put8(b, uint8_t(w)); Put8(b, uint8_t(w >> 8)); Put8(b, uint8_t(w >> 16)); return; }
  Put32(b, v << 3);
}
static size_t VarLen(uint32_t v) { return v < 0x80 ? 1 : v < 0x4000 ? 2 : v < 0x200000 ? 3 : 4; }
static void PutStr(std::vector<uint8_t>& b, const std::string& s) {
  if (s.size() < 0xFF) Put8(b, uint8_t(s.size())); else { Put8(b, 0xFF); Put16(b, uint16_t(s.size())); }
  PutBytes(b, s.data(), s.size());
}

std::string LevelName(int exp) {
  if (exp >= 0) return std::to_string(1u << exp);
  return "1D" + std::to_string(1u << -exp);
}
static int LevelPrio(int exp) { return 2100 - 10 * exp; }

// ---------------------------------------------------------------- integrity primitives
uint16_t Crc16(const uint8_t* p, size_t n, uint16_t crc) {
  static uint16_t table[256];
  static bool init = [] {
    for (uint32_t i = 0; i < 256; ++i) {  // 0xC0C1/0x4003 construction == reflected 0xA001
      uint32_t eax = 0xC0C1, r = 0;
      for (uint32_t bit = 1; bit < 0x100; bit <<= 1) { if (i & bit) r ^= eax; eax = (eax + eax) ^ 0x4003; }
      table[i] = uint16_t(r);
    }
    return true;
  }();
  (void)init;
  for (size_t i = 0; i < n; ++i) crc = uint16_t((crc >> 8) ^ table[(crc ^ p[i]) & 0xFF]);
  return crc;
}

namespace {
// BSD random() TYPE_3, with the chained reseed the AT5 key block uses.
struct BsdRandom {
  uint32_t r[31];
  int f = 3, b = 0;
  BsdRandom() {
    static const uint32_t tbl[31] = {
        0x9a319039, 0x32d9c024, 0x9b663182, 0x5da1f342, 0xde3b81e0, 0xdf0a6fb5, 0xf103bc02, 0x48f340fb,
        0x7449e56b, 0xbeb1dbb0, 0xab5c5918, 0x946554fd, 0x8c2e680f, 0xeb3d799f, 0xb11ee0b7, 0x2d436b86,
        0xda672e2a, 0x1588ca88, 0xe369735d, 0x904f35f7, 0xd7158fd6, 0x6fa6f051, 0x616e6b96, 0xac94efdc,
        0x36413f93, 0xc622c298, 0xf5a42ab8, 0x8a88d77b, 0xf5ad9d0e, 0x8999220b, 0x27fb47b9};
    memcpy(r, tbl, sizeof r);
  }
  void Advance() {
    ++f; ++b;
    if (f >= 31) f = 0; else if (b >= 31) b = 0;
  }
  int32_t Next() {
    r[f] += r[b];
    int32_t out = int32_t(r[f]) >> 1;  // arithmetic shift
    Advance();
    return out;
  }
  void Seed(uint32_t x) {
    uint32_t v = uint32_t(Next());
    r[0] = v ^ x;
    for (int i = 1; i < 31; ++i) r[i] = r[i - 1] * 0x41C64E09u + 12345u;  // NB: 0x41C64E09 (not BSD's 0x41C64E6D)
    b = 0; f = 3;
    for (int i = 0; i < 310; ++i) { r[f] += r[b]; Advance(); }
  }
};

uint8_t ReadByteAt(FILE* f, uint64_t pos) {
  _fseeki64(f, int64_t(pos), SEEK_SET);
  int c = fgetc(f);
  if (c == EOF) throw std::runtime_error("read past end while deriving keys");
  return uint8_t(c);
}
}  // namespace

static std::array<uint32_t, 4> DeriveKeysRaw(FILE* f, const std::string& usage, uint32_t hs, uint32_t lenC,
                                            uint32_t off8, uint16_t crc, uint64_t regionPos, uint32_t regionLen,
                                            uint16_t ver, const std::string& atlasStr3) {
  std::vector<uint8_t> seed(usage.begin(), usage.end());
  Put16(seed, crc);
  seed.push_back(ReadByteAt(f, uint64_t(hs) + lenC + off8 - 1));
  for (uint32_t i = 0; i < regionLen; ++i) seed.push_back(ReadByteAt(f, regionPos + i));
  if (ver >= 13) PutStr(seed, atlasStr3);
  BsdRandom rng;
  std::array<uint32_t, 4> keys{};
  for (int k = 0; k < 4; ++k) {
    uint16_t c = Crc16(seed.data(), seed.size());
    rng.Seed(c);
    int32_t v = 0;
    for (int i = 0; i <= (c & 15); ++i) v = rng.Next();
    keys[k] = uint32_t(v);
    Put32(seed, uint32_t(v));
    for (int i = 0; i < 16; ++i) {
      uint64_t pos = (uint32_t(rng.Next()) % lenC) + uint64_t(hs) + off8;
      seed.push_back(ReadByteAt(f, pos));
    }
  }
  return keys;
}

// ---------------------------------------------------------------- fixed sections of the Atlas v12 layout
static const uint8_t kSec58[8] = {0x03, 0x80, 0x02, 0xe0, 0x01, 0x01, 0x40, 0x00};
static const uint8_t kSec68[9] = {0x09, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
static const uint8_t kLibMid[] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x00, 0x06, 0x00};
static const uint8_t kLibTail[] = {0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x2f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01};
static const char kTailHex[] =
    "0101010101010300000000000101084176696174696f6e08416972706f727473000000000000000000000000020841766961"
    "74696f6e04564f527300000000000000000000000003084176696174696f6e044e4442730000000000000000000000000408"
    "4176696174696f6e0d496e74657273656374696f6e7300000000000000000000000005084176696174696f6e0c4f62737472"
    "756374696f6e730000000000000000000000000000000000000000000000000000000000000000010101010101";
static std::vector<uint8_t> TailBytes() {
  std::vector<uint8_t> t;
  for (size_t i = 0; kTailHex[i] && kTailHex[i + 1]; i += 2) {
    auto h = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    t.push_back(uint8_t(h(kTailHex[i]) * 16 + h(kTailHex[i + 1])));
  }
  return t;
}
static constexpr uint32_t kTopR0Lib = 0x026D0700, kTopR0Blk = 0x026D0682;

static std::vector<uint8_t> TypeLib(int exp, bool top, uint32_t* field64) {
  std::string name = "CoastTexture" + LevelName(exp);
  double mpp = LevelMpp(exp);
  uint32_t r0 = top ? kTopR0Lib : uint32_t(std::floor(1.5 * mpp * 256));
  uint32_t r1 = uint32_t(std::floor(0.75 * mpp * 256));
  std::vector<uint8_t> b;
  Put32(b, uint32_t(9 + name.size()));
  PutBytes(b, "\x01\x01\x00\x00\x00", 5);
  PutStr(b, name);
  PutBytes(b, kLibMid, sizeof kLibMid);
  PutStr(b, "3D Data");
  PutStr(b, "3D Texture");
  Put32(b, r0); Put32(b, r1); Put32(b, 0); Put16(b, uint16_t(LevelPrio(exp)));
  PutBytes(b, kLibTail, sizeof kLibTail);
  PutF64(b, mpp);
  *field64 = uint32_t(22 + name.size());
  return b;
}

// Block = header + quadtree node + cell payloads.
struct BlockBytes { std::vector<uint8_t> head; int32_t bbox[4]; };

static BlockBytes BuildBlockHead(int exp, bool top, const BlockPlan& bp, uint8_t b1) {
  const int64_t cs = int64_t(std::llround(kTilePx * LevelMpp(exp)));
  const int64_t ox = bp.gx * kGrid * cs, oy = bp.gy * kGrid * cs;
  std::vector<int> present(kGrid * kGrid, 0);
  int minc = 99, maxc = -1, minr = 99, maxr = -1;
  for (auto& c : bp.cells) {
    present[c.first] = 1;
    int col = c.first % kGrid, row = c.first / kGrid;
    minc = std::min(minc, col); maxc = std::max(maxc, col); minr = std::min(minr, row); maxr = std::max(maxr, row);
  }
  const uint32_t cellBytes = 1 + 1 + 1 + uint32_t(VarLen(4 + kTileDxtBytes)) + 4 + kTileDxtBytes;
  const uint32_t triLen = 1 + 1 + 1 + uint32_t(VarLen(4 + kTileDxtBytes));
  std::vector<uint8_t> node;
  Put32(node, uint32_t(int32_t(ox))); Put32(node, uint32_t(int32_t(oy)));
  Put32(node, uint32_t(cs)); Put32(node, uint32_t(cs));
  Put16(node, kGrid); Put16(node, kGrid);
  PutVar(node, kGrid * kGrid);
  uint32_t off = 0;
  for (int i = 0; i < kGrid * kGrid; ++i) {
    if (!present[i]) { PutVar(node, 0); PutVar(node, 0); continue; }
    PutVar(node, off); PutVar(node, triLen);
    off += cellBytes;
  }
  BlockBytes bb;
  bb.bbox[0] = int32_t(ox + minc * cs); bb.bbox[1] = int32_t(oy + minr * cs);
  bb.bbox[2] = int32_t(ox + (maxc + 1) * cs); bb.bbox[3] = int32_t(oy + (maxr + 1) * cs);
  const uint32_t r0 = top ? kTopR0Blk : uint32_t(std::floor(1.5 * LevelMpp(exp) * 256));
  const uint32_t r1 = std::min<uint32_t>(r0, 457022);
  auto& h = bb.head;
  Put8(h, 16); Put8(h, b1); Put8(h, 2); Put8(h, 1);
  for (int i = 0; i < 4; ++i) Put32(h, uint32_t(bb.bbox[i]));
  Put16(h, 30); PutStr(h, "");
  PutVar(h, 1); Put32(h, 0); Put32(h, 20);
  PutVar(h, 20); PutVar(h, uint32_t(node.size())); PutVar(h, 0);
  Put32(h, r0); Put32(h, r1); Put32(h, 22851); Put32(h, 0);
  PutVar(h, 1); Put16(h, 0); Put8(h, 2);
  h.insert(h.end(), node.begin(), node.end());
  return bb;
}

uint64_t BlockEncodedSize(int exp, const BlockPlan& b) {
  auto bh = BuildBlockHead(exp, false, b, 5);
  return bh.head.size() + uint64_t(b.cells.size()) * (1 + 1 + 1 + VarLen(4 + kTileDxtBytes) + 4 + kTileDxtBytes);
}

uint64_t WriteTextureFile(const std::string& path, int exp, bool top, const std::vector<BlockPlan>& blocks,
                          FILE* store, const WriteOptions& opt) {
  std::string fname = path.substr(path.find_last_of("/\\") + 1);
  uint32_t f64 = 0;
  std::vector<uint8_t> lib = TypeLib(exp, top, &f64);

  std::vector<BlockBytes> heads; heads.reserve(blocks.size());
  std::vector<uint64_t> blockSize;
  for (auto& b : blocks) {
    heads.push_back(BuildBlockHead(exp, top, b, opt.b1));
    blockSize.push_back(heads.back().head.size() + uint64_t(b.cells.size()) * (3 + VarLen(4 + kTileDxtBytes) + 4 + kTileDxtBytes));
  }
  std::vector<uint8_t> idx;
  PutVar(idx, uint32_t(blocks.size()));
  uint64_t o = 0;
  for (auto s : blockSize) { PutVar(idx, uint32_t(o)); PutVar(idx, 36); o += s; }
  const uint64_t blocksTotal = o;

  const uint32_t tIdx = uint32_t(sizeof kSec58 + lib.size() + sizeof kSec68);
  const uint64_t x78_64 = tIdx + idx.size() + blocksTotal;
  if (x78_64 > 0x7FFFFF00ull) throw std::runtime_error("file too large for 32-bit offsets; lower --split-mb");
  const uint32_t x78 = uint32_t(x78_64);
  int32_t bbox[4] = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
  for (auto& h : heads) {
    bbox[0] = std::min(bbox[0], h.bbox[0]); bbox[1] = std::min(bbox[1], h.bbox[1]);
    bbox[2] = std::max(bbox[2], h.bbox[2]); bbox[3] = std::max(bbox[3], h.bbox[3]);
  }
  std::vector<uint32_t> F = {uint32_t(bbox[0]), uint32_t(bbox[1]), uint32_t(bbox[2]), uint32_t(bbox[3]),
                             0, 8, 8, f64, uint32_t(8 + lib.size()), 9,
                             tIdx + 1,  // the block-index offset is stored +1 for v>=6 (readers subtract it)
                             uint32_t(idx.size()), x78, 1};
  for (uint32_t k = 1; k <= 5; ++k) { F.push_back(x78 + k); F.push_back(1); }
  F.insert(F.end(), {x78 + 6, 6, x78 + 12, 1, uint32_t(LevelPrio(exp)), x78 + 13, 176, x78 + 189, 1, x78 + 190, 5});

  std::vector<uint8_t> atlas;
  PutStr(atlas, fname); PutStr(atlas, opt.date); PutStr(atlas, opt.desc);
  for (auto v : F) Put32(atlas, v);

  const uint32_t hs = 44;
  const std::vector<uint8_t> tail = TailBytes();
  const uint64_t size = hs + atlas.size() + x78_64 + tail.size();

  FILE* out = nullptr;
  if (fopen_s(&out, path.c_str(), "wb+") || !out) throw std::runtime_error("cannot create " + path);
  std::vector<uint8_t> hdr;
  Put8(hdr, 0x55); Put8(hdr, 0xAA); Put16(hdr, 5); Put16(hdr, 12); Put16(hdr, 0);
  Put32(hdr, 0); Put32(hdr, uint32_t(size - hs)); Put32(hdr, ~uint32_t(size)); Put32(hdr, uint32_t(atlas.size()));
  Put16(hdr, 0);                                         // checksum (patched last)
  Put16(hdr, Crc16(atlas.data(), atlas.size()));         // CRC-16 of atlas header
  for (int i = 0; i < 16; ++i) Put8(hdr, 0);             // keys (patched)

  std::vector<uint8_t> buf;
  buf.reserve(1 << 22);
  auto flush = [&] { if (!buf.empty() && fwrite(buf.data(), 1, buf.size(), out) != buf.size()) throw std::runtime_error("writing the chart file failed - the drive is probably full (free some space or choose a Save to folder on another drive)"); buf.clear(); };
  PutBytes(buf, hdr.data(), hdr.size());
  PutBytes(buf, atlas.data(), atlas.size());
  PutBytes(buf, kSec58, sizeof kSec58);
  PutBytes(buf, lib.data(), lib.size());
  PutBytes(buf, kSec68, sizeof kSec68);
  PutBytes(buf, idx.data(), idx.size());
  std::vector<uint8_t> tile(kTileDxtBytes);
  for (size_t bi = 0; bi < blocks.size(); ++bi) {
    PutBytes(buf, heads[bi].head.data(), heads[bi].head.size());
    auto cells = blocks[bi].cells;
    std::sort(cells.begin(), cells.end());
    for (auto& c : cells) {
      PutVar(buf, 1); PutVar(buf, 0); PutVar(buf, 0); PutVar(buf, 4 + kTileDxtBytes);
      Put32(buf, kTileDxtBytes);
      _fseeki64(store, int64_t(c.second), SEEK_SET);
      if (fread(tile.data(), 1, kTileDxtBytes, store) != size_t(kTileDxtBytes)) throw std::runtime_error("tile store read failed");
      PutBytes(buf, tile.data(), tile.size());
      if (buf.size() > (1u << 22)) flush();
    }
  }
  PutBytes(buf, tail.data(), tail.size());
  flush();
  fflush(out);

  // keys: region = [hs + atlasLen + F[31]], length F[32]
  const uint16_t crc = Crc16(atlas.data(), atlas.size());
  auto keys = DeriveKeysRaw(out, opt.usage, hs, uint32_t(size - hs), 0, crc,
                            uint64_t(hs) + atlas.size() + F[31], F[32], 12, opt.desc);
  for (int k = 0; k < 4; ++k) for (int i = 0; i < 4; ++i) hdr[28 + 4 * k + i] = uint8_t(keys[k] >> (8 * i));
  uint32_t sum = 0;
  for (size_t i = 0; i < hs; ++i) sum += (i == 24 || i == 25) ? 0 : hdr[i];
  uint16_t chk = uint16_t(~sum);
  hdr[24] = uint8_t(chk); hdr[25] = uint8_t(chk >> 8);
  _fseeki64(out, 0, SEEK_SET);
  fwrite(hdr.data(), 1, hdr.size(), out);
  fclose(out);
  return size;
}

uint64_t WriteBlocksFile(const std::string& path, const std::vector<uint8_t>& lib, uint32_t libHdrLen,
                         const std::vector<BlockRef>& blocks, FILE* store, const int32_t bbox[4], uint32_t fieldB8,
                         const WriteOptions& opt) {
  std::string fname = path.substr(path.find_last_of("/\\") + 1);
  std::vector<uint8_t> idx;
  PutVar(idx, uint32_t(blocks.size()));
  uint64_t o = 0;
  for (auto& b : blocks) {
    if (o > 0x1FFFFFFFull) throw std::runtime_error("block offset exceeds varint range; lower --split-mb");
    PutVar(idx, uint32_t(o)); PutVar(idx, 36); o += b.size;
  }
  const uint32_t tIdx = uint32_t(sizeof kSec58 + lib.size() + sizeof kSec68);
  const uint64_t x78_64 = tIdx + idx.size() + o;
  if (x78_64 > 0x7FFFFF00ull) throw std::runtime_error("file too large for 32-bit offsets; lower --split-mb");
  const uint32_t x78 = uint32_t(x78_64);
  std::vector<uint32_t> F = {uint32_t(bbox[0]), uint32_t(bbox[1]), uint32_t(bbox[2]), uint32_t(bbox[3]),
                             0, 8, 8, libHdrLen, uint32_t(8 + lib.size()), 9, tIdx + 1, uint32_t(idx.size()), x78, 1};
  for (uint32_t k = 1; k <= 5; ++k) { F.push_back(x78 + k); F.push_back(1); }
  F.insert(F.end(), {x78 + 6, 6, x78 + 12, 1, fieldB8, x78 + 13, 176, x78 + 189, 1, x78 + 190, 5});
  std::vector<uint8_t> atlas;
  PutStr(atlas, fname); PutStr(atlas, opt.date); PutStr(atlas, opt.desc);
  for (auto v : F) Put32(atlas, v);
  const uint32_t hs = 44;
  const std::vector<uint8_t> tail = TailBytes();
  const uint64_t size = hs + atlas.size() + x78_64 + tail.size();
  FILE* out = nullptr;
  if (fopen_s(&out, path.c_str(), "wb+") || !out) throw std::runtime_error("cannot create " + path);
  std::vector<uint8_t> hdr;
  Put8(hdr, 0x55); Put8(hdr, 0xAA); Put16(hdr, 5); Put16(hdr, 12); Put16(hdr, 0);
  Put32(hdr, 0); Put32(hdr, uint32_t(size - hs)); Put32(hdr, ~uint32_t(size)); Put32(hdr, uint32_t(atlas.size()));
  Put16(hdr, 0); Put16(hdr, Crc16(atlas.data(), atlas.size()));
  for (int i = 0; i < 16; ++i) Put8(hdr, 0);
  std::vector<uint8_t> buf;
  auto flush = [&] { if (!buf.empty() && fwrite(buf.data(), 1, buf.size(), out) != buf.size()) throw std::runtime_error("writing the chart file failed - the drive is probably full (free some space or choose a Save to folder on another drive)"); buf.clear(); };
  PutBytes(buf, hdr.data(), hdr.size()); PutBytes(buf, atlas.data(), atlas.size());
  PutBytes(buf, kSec58, sizeof kSec58); PutBytes(buf, lib.data(), lib.size()); PutBytes(buf, kSec68, sizeof kSec68);
  PutBytes(buf, idx.data(), idx.size());
  flush();
  std::vector<uint8_t> chunk(1 << 22);
  for (auto& b : blocks) {
    _fseeki64(store, int64_t(b.storeOffset), SEEK_SET);
    for (uint64_t left = b.size; left;) {
      size_t n = size_t(std::min<uint64_t>(left, chunk.size()));
      if (fread(chunk.data(), 1, n, store) != n) throw std::runtime_error("block store read failed");
      if (fwrite(chunk.data(), 1, n, out) != n) throw std::runtime_error("writing the chart file failed - the drive is probably full (free some space or choose a Save to folder on another drive)");
      left -= n;
    }
  }
  PutBytes(buf, tail.data(), tail.size());
  flush();
  fflush(out);
  const uint16_t crc = Crc16(atlas.data(), atlas.size());
  auto keys = DeriveKeysRaw(out, opt.usage, hs, uint32_t(size - hs), 0, crc, uint64_t(hs) + atlas.size() + F[31], F[32], 12, opt.desc);
  for (int k = 0; k < 4; ++k) for (int i = 0; i < 4; ++i) hdr[28 + 4 * k + i] = uint8_t(keys[k] >> (8 * i));
  uint32_t sum = 0;
  for (size_t i = 0; i < hs; ++i) sum += (i == 24 || i == 25) ? 0 : hdr[i];
  uint16_t chk = uint16_t(~sum);
  hdr[24] = uint8_t(chk); hdr[25] = uint8_t(chk >> 8);
  _fseeki64(out, 0, SEEK_SET);
  fwrite(hdr.data(), 1, hdr.size(), out);
  fclose(out);
  return size;
}

// ---------------------------------------------------------------- reading / verification
namespace {
struct Reader {
  FILE* f;
  uint8_t U8() { int c = fgetc(f); if (c == EOF) throw std::runtime_error("unexpected EOF"); return uint8_t(c); }
  void Raw(void* p, size_t n) { if (fread(p, 1, n, f) != n) throw std::runtime_error("unexpected EOF"); }
  uint16_t U16() { uint8_t b[2]; Raw(b, 2); return uint16_t(b[0] | b[1] << 8); }
  uint32_t U32() { uint8_t b[4]; Raw(b, 4); return uint32_t(b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24); }
  int32_t I32() { return int32_t(U32()); }
  uint32_t Var() {
    uint32_t b0 = U8();
    if (b0 & 1) return b0 >> 1;
    if (b0 & 2) return (b0 | U8() << 8) >> 2;
    if (b0 & 4) { uint32_t b1 = U8(), b2 = U8(); return (b0 | b1 << 8 | b2 << 16) >> 3; }
    uint32_t b1 = U8(), b2 = U8(), b3 = U8();
    return (b0 | b1 << 8 | b2 << 16 | b3 << 24) >> 3;
  }
  std::string Str() { uint32_t n = U8(); if (n == 0xFF) n = U16(); std::string s(n, '\0'); if (n) Raw(&s[0], n); return s; }
  uint64_t Pos() { return uint64_t(_ftelli64(f)); }
  void Seek(uint64_t p) { _fseeki64(f, int64_t(p), SEEK_SET); }
};

struct Header {
  uint8_t raw[44]; uint16_t ver, minor; uint32_t off8, lenC, notSize, secSize; uint16_t sum, crc; uint32_t keys[4];
  uint32_t hs; std::string s[3]; std::vector<uint32_t> F; uint64_t size;
  uint32_t Fld(uint32_t off) const { return F.at((off - 0x48) / 4); }
};

Header ReadHeader(FILE* f) {
  Header h{};
  _fseeki64(f, 0, SEEK_END); h.size = uint64_t(_ftelli64(f)); _fseeki64(f, 0, SEEK_SET);
  Reader r{f};
  r.Raw(h.raw, 26);
  memcpy(&h.ver, h.raw + 4, 2); memcpy(&h.minor, h.raw + 6, 2);
  memcpy(&h.off8, h.raw + 8, 4); memcpy(&h.lenC, h.raw + 12, 4); memcpy(&h.notSize, h.raw + 16, 4);
  memcpy(&h.secSize, h.raw + 20, 4); memcpy(&h.sum, h.raw + 24, 2);
  h.hs = h.ver >= 6 ? 44 : 26;
  if (h.ver >= 6) { r.Raw(h.raw + 26, 18); memcpy(&h.crc, h.raw + 26, 2); memcpy(h.keys, h.raw + 28, 16); }
  r.Seek(h.hs + h.off8);
  for (auto& s : h.s) s = r.Str();
  uint64_t used = r.Pos() - (h.hs + h.off8);
  for (uint64_t i = used; i + 4 <= h.secSize; i += 4) h.F.push_back(r.U32());
  return h;
}
}  // namespace

std::array<uint32_t, 4> DeriveKeys(FILE* f, const std::string& usage) {
  Header h = ReadHeader(f);
  return DeriveKeysRaw(f, usage, h.hs, h.lenC, h.off8, h.crc, uint64_t(h.hs) + h.secSize + h.Fld(0xc4), h.Fld(0xc8),
                       h.ver, h.s[2]);
}

ExtractedFile Extract(const std::string& path) {
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "rb") || !f) throw std::runtime_error("cannot open " + path);
  ExtractedFile ex;
  try {
    Header h = ReadHeader(f);
    ex.name = h.s[0]; ex.date = h.s[1]; ex.desc = h.s[2];
    Reader r{f};
    const uint64_t base = uint64_t(h.hs) + h.secSize;
    // level from type library name
    r.Seek(base + h.Fld(0x60));
    r.U32(); r.U8(); r.U32();
    std::string tname = r.Str();
    if (tname.rfind("CoastTexture", 0) != 0) throw std::runtime_error("not a 3D texture file (type '" + tname + "')");
    std::string lv = tname.substr(12);
    if (lv.rfind("1D", 0) == 0) { int d = std::stoi(lv.substr(2)); int e = 0; while ((1 << e) < d) ++e; ex.levelExp = -e; }
    else { int n = std::stoi(lv); int e = 0; while ((1 << e) < n) ++e; ex.levelExp = e; }
    r.Seek(base + h.Fld(0x70) - (h.ver >= 6 ? 1 : 0));
    uint32_t n = r.Var();
    std::vector<uint32_t> offs(n);
    for (auto& o : offs) { o = r.Var(); r.Var(); }
    const uint64_t bb = r.Pos();
    for (uint32_t o : offs) {
      r.Seek(bb + o);
      r.U8(); ex.b1 = r.U8(); r.U8(); r.U8();
      for (int i = 0; i < 4; ++i) r.I32();
      r.U16(); r.Str();
      uint32_t ns = r.Var(); for (uint32_t i = 0; i < ns; ++i) { r.U32(); r.U32(); }
      r.Var(); r.Var();
      uint32_t m = r.Var(); for (uint32_t i = 0; i < m; ++i) { r.U32(); r.U32(); }
      for (uint32_t i = 0; i < ns; ++i) {
        uint32_t r0 = r.U32(); r.U32(); r.U32(); r.U32();
        if (r0 == kTopR0Blk) ex.top = true;
        uint32_t k = r.Var(); for (uint32_t j = 0; j < k; ++j) { r.U16(); r.U8(); }
      }
      int32_t ox = r.I32(), oy = r.I32(), cs = r.I32(); r.I32();
      int16_t nx = int16_t(r.U16()), ny = int16_t(r.U16());
      uint32_t cnt = r.Var();
      if (cnt != uint32_t(nx * ny)) throw std::runtime_error("grid mismatch");
      std::vector<std::pair<uint32_t, uint32_t>> cells(cnt);
      for (auto& c : cells) { c.first = r.Var(); c.second = r.Var(); }
      const uint64_t ds = r.Pos();
      std::map<int, std::vector<uint8_t>> tiles;
      for (uint32_t i = 0; i < cnt; ++i) {
        if (!cells[i].second) continue;
        r.Seek(ds + cells[i].first);
        uint32_t k = r.Var();
        for (uint32_t j = 0; j < k; ++j) { r.Var(); r.Var(); r.Var(); }
        uint32_t len = r.U32();
        std::vector<uint8_t> t(len);
        r.Raw(t.data(), len);
        tiles[int(i)] = std::move(t);
      }
      const int64_t span = int64_t(cs) * kGrid;
      ex.blocks.push_back({{std::llround(std::floor(double(ox) / span)), std::llround(std::floor(double(oy) / span))}, std::move(tiles)});
    }
  } catch (...) { fclose(f); throw; }
  fclose(f);
  return ex;
}

bool Verify(const std::string& path, bool verbose) {
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "rb") || !f) { printf("  cannot open %s\n", path.c_str()); return false; }
  bool ok = true;
  auto check = [&](bool c, const char* what) { if (!c) { ok = false; printf("  FAIL: %s\n", what); } else if (verbose) printf("  ok:   %s\n", what); };
  try {
    Header h = ReadHeader(f);
    printf("%s\n  version %u, size %llu, name '%s', date '%s', desc '%s'\n", path.c_str(), h.ver,
           (unsigned long long)h.size, h.s[0].c_str(), h.s[1].c_str(), h.s[2].c_str());
    check(h.raw[0] == 0x55 && h.raw[1] == 0xAA, "magic 55 AA");
    check(h.ver >= 4 && h.minor == 0, "version supported by IMC reader (major>=4, minor==0)");
    check(h.lenC == h.size - h.hs, "length field == file size - header");
    check(h.notSize == ~uint32_t(h.size), "~size field");
    uint8_t tmp[44]; memcpy(tmp, h.raw, h.hs); tmp[24] = tmp[25] = 0;
    uint32_t sum = 0; for (uint32_t i = 0; i < h.hs; ++i) sum += tmp[i];
    check(uint16_t(~sum) == h.sum, "header checksum");
    if (h.ver >= 6) {
      std::vector<uint8_t> sec(h.secSize);
      _fseeki64(f, h.hs + h.off8, SEEK_SET);
      fread(sec.data(), 1, sec.size(), f);
      check(Crc16(sec.data(), sec.size()) == h.crc, "atlas header CRC-16");
      const char* usages[] = {"UnrestrictedUse", "RestrictedUse", "ContentID"};
      const char* matched = nullptr;
      for (auto u : usages) {
        auto k = DeriveKeys(f, u);
        if (k[0] == h.keys[0] && k[1] == h.keys[1] && k[2] == h.keys[2] && k[3] == h.keys[3]) { matched = u; break; }
      }
      check(matched != nullptr, "key block matches an IMC usage string");
      if (matched) printf("  key usage: %s\n", matched);
    }
    fclose(f); f = nullptr;
    try {
      ExtractedFile ex = Extract(path);
      size_t tiles = 0; bool sizes = true;
      for (auto& b : ex.blocks) for (auto& t : b.second) { ++tiles; sizes &= t.second.size() == size_t(kTileDxtBytes); }
      printf("  3D texture level %s (%g m/px), %zu blocks, %zu tiles%s\n", LevelName(ex.levelExp).c_str(),
             LevelMpp(ex.levelExp), ex.blocks.size(), tiles, ex.top ? ", top level" : "");
      check(sizes, "every tile is 256x256 DXT1 (32768 bytes)");
    } catch (const std::exception& e) {
      if (verbose) printf("  (not a 3D texture file: %s)\n", e.what());  // vector files are checked separately
    }
  } catch (const std::exception& e) {
    printf("  error: %s\n", e.what()); ok = false;
  }
  if (f) fclose(f);
  printf("  => %s\n", ok ? "VALID" : "INVALID");
  return ok;
}

}  // namespace at5

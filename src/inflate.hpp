// Minimal DEFLATE / zlib / gzip decoder (RFC 1950/1951/1952), for gzip-compressed MVT tiles.
#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace inflate {

class Inflater {
 public:
  Inflater(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  void Run(std::vector<uint8_t>& out) {
    for (;;) {
      int final = Bits(1), type = Bits(2);
      if (type == 0) Stored(out);
      else if (type == 1) { FixedTables(); Codes(out); }
      else if (type == 2) { DynamicTables(); Codes(out); }
      else throw std::runtime_error("inflate: bad block type");
      if (final) break;
    }
  }
 private:
  struct Huff { uint16_t count[16]; uint16_t sym[320]; };
  const uint8_t* p_; size_t n_, pos_ = 0; uint32_t bitbuf_ = 0; int bitcnt_ = 0;
  Huff lit_{}, dist_{};
  int Bits(int need) {
    uint32_t v = bitbuf_;
    while (bitcnt_ < need) {
      if (pos_ >= n_) throw std::runtime_error("inflate: truncated");
      v |= uint32_t(p_[pos_++]) << bitcnt_; bitcnt_ += 8;
    }
    bitbuf_ = v >> need; bitcnt_ -= need;
    return int(v & ((1u << need) - 1));
  }
  void Stored(std::vector<uint8_t>& out) {
    bitbuf_ = 0; bitcnt_ = 0;
    if (pos_ + 4 > n_) throw std::runtime_error("inflate: truncated stored");
    unsigned len = p_[pos_] | p_[pos_ + 1] << 8; pos_ += 4;
    if (pos_ + len > n_) throw std::runtime_error("inflate: truncated stored");
    out.insert(out.end(), p_ + pos_, p_ + pos_ + len); pos_ += len;
  }
  static void Build(Huff& h, const uint8_t* len, int n) {
    memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; ++i) h.count[len[i]]++;
    h.count[0] = 0;
    uint16_t offs[16]; offs[1] = 0;
    for (int i = 1; i < 15; ++i) offs[i + 1] = uint16_t(offs[i] + h.count[i]);
    for (int i = 0; i < n; ++i) if (len[i]) h.sym[offs[len[i]]++] = uint16_t(i);
  }
  int Decode(const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      code |= Bits(1);
      int count = h.count[len];
      if (code - count < first) return h.sym[index + (code - first)];
      index += count; first += count; first <<= 1; code <<= 1;
    }
    throw std::runtime_error("inflate: bad code");
  }
  void FixedTables() {
    uint8_t l[288];
    int i = 0;
    for (; i < 144; ++i) l[i] = 8;
    for (; i < 256; ++i) l[i] = 9;
    for (; i < 280; ++i) l[i] = 7;
    for (; i < 288; ++i) l[i] = 8;
    Build(lit_, l, 288);
    for (i = 0; i < 30; ++i) l[i] = 5;
    Build(dist_, l, 30);
  }
  void DynamicTables() {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
    uint8_t lengths[320] = {0};
    for (int i = 0; i < ncode; ++i) lengths[order[i]] = uint8_t(Bits(3));
    Huff lencode; Build(lencode, lengths, 19);
    int idx = 0;
    while (idx < nlen + ndist) {
      int sym = Decode(lencode);
      if (sym < 16) { lengths[idx++] = uint8_t(sym); continue; }
      int len = 0, rep;
      if (sym == 16) { if (!idx) throw std::runtime_error("inflate: repeat"); len = lengths[idx - 1]; rep = 3 + Bits(2); }
      else if (sym == 17) rep = 3 + Bits(3);
      else rep = 11 + Bits(7);
      if (idx + rep > nlen + ndist) throw std::runtime_error("inflate: too many lengths");
      while (rep--) lengths[idx++] = uint8_t(len);
    }
    Build(lit_, lengths, nlen);
    Build(dist_, lengths + nlen, ndist);
  }
  void Codes(std::vector<uint8_t>& out) {
    static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      int sym = Decode(lit_);
      if (sym < 256) { out.push_back(uint8_t(sym)); continue; }
      if (sym == 256) return;
      sym -= 257;
      if (sym >= 29) throw std::runtime_error("inflate: bad length");
      int len = lbase[sym] + Bits(lext[sym]);
      int ds = Decode(dist_);
      if (ds >= 30) throw std::runtime_error("inflate: bad distance");
      size_t dist = size_t(dbase[ds] + Bits(dext[ds]));
      if (dist > out.size()) throw std::runtime_error("inflate: distance too far");
      size_t from = out.size() - dist;
      for (int i = 0; i < len; ++i) out.push_back(out[from + size_t(i)]);
    }
  }
};

// Decompress gzip, zlib or raw-deflate data; returns input unchanged if it is not compressed.
inline std::vector<uint8_t> Auto(const uint8_t* p, size_t n) {
  std::vector<uint8_t> out;
  if (n >= 10 && p[0] == 0x1f && p[1] == 0x8b) {
    size_t pos = 10; uint8_t flg = p[3];
    if (flg & 4) { if (pos + 2 > n) throw std::runtime_error("gzip header"); pos += 2 + size_t(p[pos] | p[pos + 1] << 8); }
    if (flg & 8) { while (pos < n && p[pos]) ++pos; ++pos; }
    if (flg & 16) { while (pos < n && p[pos]) ++pos; ++pos; }
    if (flg & 2) pos += 2;
    out.reserve(n * 4);
    Inflater(p + pos, n - pos).Run(out);
    return out;
  }
  if (n >= 2 && (p[0] & 0x0f) == 8 && ((p[0] << 8) | p[1]) % 31 == 0) {
    out.reserve(n * 4);
    Inflater(p + 2, n - 2).Run(out);
    return out;
  }
  return std::vector<uint8_t>(p, p + n);
}

}  // namespace inflate

// Fast DXT1/BC1 encoder (opaque). PCA endpoints + one least-squares refinement.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace dxt1 {

inline uint16_t To565(float r, float g, float b) {
  int R = std::clamp(int(r * 31.0f / 255.0f + 0.5f), 0, 31);
  int G = std::clamp(int(g * 63.0f / 255.0f + 0.5f), 0, 63);
  int B = std::clamp(int(b * 31.0f / 255.0f + 0.5f), 0, 31);
  return uint16_t(R << 11 | G << 5 | B);
}
inline void From565(uint16_t c, int* o) {
  int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  o[0] = (r << 3) | (r >> 2); o[1] = (g << 2) | (g >> 4); o[2] = (b << 3) | (b >> 2);
}

// px: 16 pixels RGB (3 bytes each), row-major within the 4x4 block. Writes 8 bytes.
inline void EncodeBlock(const uint8_t* px, uint8_t* out) {
  // solid / min-max
  int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
  float mean[3] = {0, 0, 0};
  for (int i = 0; i < 16; ++i)
    for (int c = 0; c < 3; ++c) { int v = px[i * 3 + c]; mn[c] = std::min(mn[c], v); mx[c] = std::max(mx[c], v); mean[c] += v; }
  if (mn[0] == mx[0] && mn[1] == mx[1] && mn[2] == mx[2]) {
    if (mx[0] == 0 && mx[1] == 0 && mx[2] == 0) { memset(out, 0, 8); return; }
    uint16_t c = To565(float(mx[0]), float(mx[1]), float(mx[2]));
    // solid colour: c0 = c1 = c, indices 0 (decodes exactly to c in either mode)
    out[0] = uint8_t(c); out[1] = uint8_t(c >> 8); out[2] = uint8_t(c); out[3] = uint8_t(c >> 8);
    memset(out + 4, 0, 4);
    return;
  }
  for (float& m : mean) m *= 1.0f / 16.0f;
  float cov[6] = {0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    float r = px[i * 3] - mean[0], g = px[i * 3 + 1] - mean[1], b = px[i * 3 + 2] - mean[2];
    cov[0] += r * r; cov[1] += r * g; cov[2] += r * b; cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
  }
  float v[3] = {float(mx[0] - mn[0]), float(mx[1] - mn[1]), float(mx[2] - mn[2])};
  for (int it = 0; it < 6; ++it) {
    float x = v[0] * cov[0] + v[1] * cov[1] + v[2] * cov[2];
    float y = v[0] * cov[1] + v[1] * cov[3] + v[2] * cov[4];
    float z = v[0] * cov[2] + v[1] * cov[4] + v[2] * cov[5];
    float m = std::max(std::fabs(x), std::max(std::fabs(y), std::fabs(z)));
    if (m < 1e-6f) break;
    v[0] = x / m; v[1] = y / m; v[2] = z / m;
  }
  float lo = 1e30f, hi = -1e30f; int ilo = 0, ihi = 0;
  for (int i = 0; i < 16; ++i) {
    float d = px[i * 3] * v[0] + px[i * 3 + 1] * v[1] + px[i * 3 + 2] * v[2];
    if (d < lo) { lo = d; ilo = i; }
    if (d > hi) { hi = d; ihi = i; }
  }
  uint16_t c0 = To565(px[ihi * 3], px[ihi * 3 + 1], px[ihi * 3 + 2]);
  uint16_t c1 = To565(px[ilo * 3], px[ilo * 3 + 1], px[ilo * 3 + 2]);

  auto assign = [&](uint16_t a, uint16_t b, uint32_t* bitsOut) -> int {
    int p[4][3]; From565(a, p[0]); From565(b, p[1]);
    for (int c = 0; c < 3; ++c) { p[2][c] = (2 * p[0][c] + p[1][c]) / 3; p[3][c] = (p[0][c] + 2 * p[1][c]) / 3; }
    uint32_t bits = 0; int err = 0;
    for (int i = 0; i < 16; ++i) {
      int best = 0, be = INT32_MAX;
      for (int k = 0; k < 4; ++k) {
        int dr = px[i * 3] - p[k][0], dg = px[i * 3 + 1] - p[k][1], db = px[i * 3 + 2] - p[k][2];
        int e = dr * dr + dg * dg + db * db;
        if (e < be) { be = e; best = k; }
      }
      bits |= uint32_t(best) << (2 * i); err += be;
    }
    *bitsOut = bits; return err;
  };
  auto order = [&](uint16_t& a, uint16_t& b, uint32_t& bits) {
    if (a < b) {  // need a > b for 4-colour mode: swap endpoints, remap 0<->1, 2<->3
      std::swap(a, b);
      bits ^= 0x55555555u;
    } else if (a == b) {
      bits = 0;
    }
  };
  uint32_t bits;
  int err = assign(c0, c1, &bits);
  // least-squares refinement of endpoints for the chosen indices
  {
    static const float w0[4] = {1.0f, 0.0f, 2.0f / 3, 1.0f / 3};
    float aa = 0, bb = 0, ab = 0, ax[3] = {0, 0, 0}, bx[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) {
      int k = (bits >> (2 * i)) & 3;
      float a = w0[k], b = 1.0f - a;
      aa += a * a; bb += b * b; ab += a * b;
      for (int c = 0; c < 3; ++c) { ax[c] += a * px[i * 3 + c]; bx[c] += b * px[i * 3 + c]; }
    }
    float det = aa * bb - ab * ab;
    if (std::fabs(det) > 1e-6f) {
      float inv = 1.0f / det, e0[3], e1[3];
      for (int c = 0; c < 3; ++c) { e0[c] = (ax[c] * bb - bx[c] * ab) * inv; e1[c] = (bx[c] * aa - ax[c] * ab) * inv; }
      uint16_t n0 = To565(e0[0], e0[1], e0[2]), n1 = To565(e1[0], e1[1], e1[2]);
      if (n0 != n1) {
        uint32_t nb; int ne = assign(n0, n1, &nb);
        if (ne < err) { err = ne; c0 = n0; c1 = n1; bits = nb; }
      }
    }
  }
  order(c0, c1, bits);
  out[0] = uint8_t(c0); out[1] = uint8_t(c0 >> 8); out[2] = uint8_t(c1); out[3] = uint8_t(c1 >> 8);
  out[4] = uint8_t(bits); out[5] = uint8_t(bits >> 8); out[6] = uint8_t(bits >> 16); out[7] = uint8_t(bits >> 24);
}

// A block with some see-through pixels (opaque[i] false): DXT1's three-colour mode, where c0 <= c1 and index 3
// means transparent (black to a decoder that ignores transparency, as no-data was before).
inline void EncodeBlockPunch(const uint8_t* px, const bool* opaque, uint8_t* out) {
  // endpoints: the opaque pixels furthest apart along their main colour direction (by brightness here: simple and
  // good enough for the few blocks on a survey's edge)
  int ilo = -1, ihi = -1, lo = INT32_MAX, hi = -1;
  for (int i = 0; i < 16; ++i) {
    if (!opaque[i]) continue;
    const int l = px[i * 3] * 2 + px[i * 3 + 1] * 5 + px[i * 3 + 2];
    if (l < lo) { lo = l; ilo = i; }
    if (l > hi) { hi = l; ihi = i; }
  }
  uint16_t a = To565(px[ilo * 3], px[ilo * 3 + 1], px[ilo * 3 + 2]), b = To565(px[ihi * 3], px[ihi * 3 + 1], px[ihi * 3 + 2]);
  if (a > b) std::swap(a, b);   // c0 <= c1: three-colour mode
  int p[3][3]; From565(a, p[0]); From565(b, p[1]);
  for (int c = 0; c < 3; ++c) p[2][c] = (p[0][c] + p[1][c]) / 2;
  uint32_t bits = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 3;
    if (opaque[i]) {
      int be = INT32_MAX;
      for (int k = 0; k < 3; ++k) {
        const int dr = px[i * 3] - p[k][0], dg = px[i * 3 + 1] - p[k][1], db = px[i * 3 + 2] - p[k][2];
        const int e = dr * dr + dg * dg + db * db;
        if (e < be) { be = e; best = k; }
      }
    }
    bits |= uint32_t(best) << (2 * i);
  }
  out[0] = uint8_t(a); out[1] = uint8_t(a >> 8); out[2] = uint8_t(b); out[3] = uint8_t(b >> 8);
  out[4] = uint8_t(bits); out[5] = uint8_t(bits >> 8); out[6] = uint8_t(bits >> 16); out[7] = uint8_t(bits >> 24);
}

// rgba: 256x256 RGBA tile, row 0 = SOUTH edge (AT5 texture orientation). alpha==0 -> no data: see-through (DXT1's
// transparent index), which a plotter without transparency shows black.
inline void EncodeTile256(const uint8_t* rgba, uint8_t* out32k) {
  uint8_t blk[48];
  bool opaque[16];
  uint8_t* o = out32k;
  for (int by = 0; by < 64; ++by)
    for (int bx = 0; bx < 64; ++bx) {
      int n = 0;
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
          const uint8_t* p = rgba + (size_t((by * 4 + y) * 256 + bx * 4 + x)) * 4;
          uint8_t* q = blk + (y * 4 + x) * 3;
          opaque[y * 4 + x] = p[3] != 0;
          if (p[3]) { q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; ++n; }
          else { q[0] = q[1] = q[2] = 0; }
        }
      if (n == 16) EncodeBlock(blk, o);
      else if (n == 0) { memset(o, 0, 4); memset(o + 4, 0xFF, 4); }   // c0 = c1 = 0, every index 3: all see-through
      else EncodeBlockPunch(blk, opaque, o);
      o += 8;
    }
}

}  // namespace dxt1

/*
indexed_pipeline_bench.cpp

Indexed-color PNG pipeline benchmark and acceptance test, built on top of the
stock lodepng encoder (no changes to lodepng itself, no third party deps).

Pipeline (indexed path):
  1. Median-cut quantization. The split channel is the channel with the largest
     variance; a bucket stops splitting when its variance is below the
     configurable threshold, or when the configured palette size cap is hit.
  2. Floyd-Steinberg dithering with configurable strength (strength 0 disables
     dithering). Quantization lookups are O(1) through a 32^3 RGB grid LUT,
     plus an exact-match table so colors that are in the palette are always
     reproduced exactly.
  3. PNG encode as LCT_PALETTE. Per scanline lodepng's LFS_MINSUM tries all
     five PNG filters (None/Sub/Up/Average/Paeth) and keeps the one with the
     smallest sum of absolute residuals. The filter strategy is configurable.

The non-indexed path encodes the same source as 8-bit RGB and is reported
alongside, together with the "current encoder" baseline (RGB, fixed filter 0,
no quantization and no filter selection -- the behavior reported by the art
team). The four acceptance image classes (icon / screenshot / photo / solid)
are generated procedurally, so the whole thing runs in one shot.

Build (see run_indexed_pipeline.sh):
  g++ -std=c++17 -O3 -W -Wall -Wextra -I. indexed_pipeline_bench.cpp lodepng.cpp
*/

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <string>
#include <vector>

#include "lodepng.h"

namespace {

using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Configuration (all knobs required by the spec are exposed on the CLI)
// ---------------------------------------------------------------------------
struct Config {
  unsigned maxColors = 256;      // quantization bucket cap
  double ditherStrength = 0.6;   // Floyd-Steinberg strength, 0 = off
  double varThreshold = 0.0;     // stop splitting buckets below this variance
  LodePNGFilterStrategy filter = LFS_MINSUM;
  unsigned sampleSize = 512;     // size of the small saved sample images
  unsigned perfSize = 1024;      // size used for the 400 ms performance gate
  int runs = 5;                  // timing repetitions (median is reported)
};

struct RGB {
  double r = 0, g = 0, b = 0;
};

struct PaletteEntry {
  int r = 0, g = 0, b = 0;
};

// ---------------------------------------------------------------------------
// Deterministic procedural test images (RGB, 8 bit per channel, opaque)
// ---------------------------------------------------------------------------
uint32_t hash2d(unsigned x, unsigned y) {
  uint32_t h = x * 374761393u + y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

uint8_t clampByte(double v) {
  v = std::round(v);
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return static_cast<uint8_t>(v);
}

void putPx(std::vector<uint8_t>& img, unsigned w, unsigned x, unsigned y,
           int r, int g, int b) {
  size_t i = (static_cast<size_t>(y) * w + x) * 3;
  img[i] = static_cast<uint8_t>(r);
  img[i + 1] = static_cast<uint8_t>(g);
  img[i + 2] = static_cast<uint8_t>(b);
}

// Flat vector-style app icon: a handful of exact colors in large solid
// regions (page, disc, inner disc, hub, badge, triangle). No gradients and
// no antialiasing, like the SVG/vector artwork a UI pipeline exports.
void genIcon(std::vector<uint8_t>& img, unsigned w, unsigned h) {
  for (unsigned y = 0; y < h; ++y) {
    for (unsigned x = 0; x < w; ++x) {
      int r = 245, g = 247, b = 250;  // page background
      double cx = w * 0.5, cy = h * 0.42, rad = w * 0.40;
      double dx = x - cx, dy = y - cy;
      double d2 = dx * dx + dy * dy;
      if (d2 <= rad * rad) { r = 37; g = 99; b = 235; }              // disc
      if (d2 <= rad * rad * 0.55) { r = 255; g = 255; b = 255; }     // inner disc
      if (d2 <= rad * rad * 0.22) { r = 245; g = 158; b = 11; }      // hub
      if (x >= w * 0.10 && x < w * 0.42 && y >= h * 0.74 &&
          y < h * 0.92) {
        r = 239; g = 68; b = 68;                                     // badge
      }
      if (y >= h * 0.74 && y < h * 0.92 && x >= w * 0.58 &&
          x < w * 0.58 + (y - h * 0.74) * 1.8) {
        r = 16; g = 185; b = 129;                                    // triangle
      }
      putPx(img, w, x, y, r, g, b);
    }
  }
}

// UI mock: flat panels, text stripes, buttons and one smooth gradient band.
void genScreenshot(std::vector<uint8_t>& img, unsigned w, unsigned h) {
  for (unsigned y = 0; y < h; ++y) {
    for (unsigned x = 0; x < w; ++x) {
      int r = 255, g = 255, b = 255;
      if (y < h * 0.08) { r = 31; g = 41; b = 55; }                       // title bar
      else if (x < w * 0.22) { r = 241; g = 245; b = 249; }               // side panel
      if (y >= h * 0.14 && y < h * 0.86 && x >= w * 0.30 && x < w * 0.34) {
        r = 71; g = 85; b = 105;                                          // text lines
      }
      for (int k = 0; k < 6; ++k) {                                       // text rows
        unsigned ry = static_cast<unsigned>(h * (0.18 + 0.10 * k));
        if (y >= ry && y < ry + h / 80 && x >= w * 0.40 && x < w * 0.78) {
          r = 71; g = 85; b = 105;
        }
      }
      if (x >= w * 0.40 && x < w * 0.58 && y >= h * 0.82 && y < h * 0.90) {
        r = 37; g = 99; b = 235;                                          // button
      }
      if ((x - w * 0.85) * (x - w * 0.85) +
          (y - h * 0.18) * (y - h * 0.18) < (w / 40.0) * (w / 40.0)) {
        r = 16; g = 185; b = 129;                                         // status dot
      }
      if (x >= w * 0.82 && x < w * 0.95 && y >= h * 0.30 && y < h * 0.80) {
        int t = static_cast<int>(255.0 * (y - h * 0.30) / (h * 0.50));    // gradient
        r = 99 + t / 3; g = 102 + t / 4; b = 241 - t / 3;
      }
      putPx(img, w, x, y, r, g, b);
    }
  }
}

// Smooth multi-directional gradients plus fine deterministic grain: many
// colors, but the grain stays small enough that a 256-entry palette keeps
// PSNR comfortably above 30 dB even with full Floyd-Steinberg diffusion.
void genPhoto(std::vector<uint8_t>& img, unsigned w, unsigned h) {
  for (unsigned y = 0; y < h; ++y) {
    for (unsigned x = 0; x < w; ++x) {
      double nx = static_cast<double>(x) / w;
      double ny = static_cast<double>(y) / h;
      double radial = std::sqrt((nx - 0.5) * (nx - 0.5) +
                                (ny - 0.5) * (ny - 0.5));
      int noise = static_cast<int>(hash2d(x, y) % 11) - 5;
      int r = static_cast<int>(128 + 90 * std::sin(6.28 * nx) + noise);
      int g = static_cast<int>(110 + 95 * std::cos(5.8 * ny) + noise * 0.7);
      int b = static_cast<int>(140 - 120 * radial +
                                40 * std::sin(9.0 * (nx + ny)) + noise * 0.5);
      putPx(img, w, x, y, r, g, b);
    }
  }
}

void genSolid(std::vector<uint8_t>& img, unsigned w, unsigned h) {
  for (unsigned y = 0; y < h; ++y) {
    for (unsigned x = 0; x < w; ++x) putPx(img, w, x, y, 37, 99, 235);
  }
}

using ImageGen = void (*)(std::vector<uint8_t>&, unsigned, unsigned);

// ---------------------------------------------------------------------------
// Median-cut quantization
//
// A bucket stores packed 0xRRGGBB pixels. Every iteration the splittable
// bucket with the largest per-channel variance is chosen; it is cut at the
// median along that channel. Splitting stops at the bucket cap or when the
// largest remaining variance is below the configured threshold.
// ---------------------------------------------------------------------------
uint32_t packRGB(int r, int g, int b) {
  return (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) |
         static_cast<uint32_t>(b);
}

struct BucketStats {
  double variance[3];
  int minv[3], maxv[3];
};

struct Bucket {
  std::vector<uint32_t> px;
  BucketStats stats;
  bool splittable = true;
};

int channelOf(uint32_t c, int ch) {
  if (ch == 0) return (c >> 16) & 0xFF;
  if (ch == 1) return (c >> 8) & 0xFF;
  return c & 0xFF;
}

BucketStats computeStats(const std::vector<uint32_t>& px) {
  BucketStats s;
  double sum[3] = {0, 0, 0}, sumsq[3] = {0, 0, 0};
  for (int ch = 0; ch < 3; ++ch) { s.minv[ch] = 255; s.maxv[ch] = 0; }
  for (uint32_t c : px) {
    int v[3] = {static_cast<int>((c >> 16) & 0xFF),
                static_cast<int>((c >> 8) & 0xFF),
                static_cast<int>(c & 0xFF)};
    for (int ch = 0; ch < 3; ++ch) {
      sum[ch] += v[ch];
      sumsq[ch] += static_cast<double>(v[ch]) * v[ch];
      s.minv[ch] = std::min(s.minv[ch], v[ch]);
      s.maxv[ch] = std::max(s.maxv[ch], v[ch]);
    }
  }
  double n = static_cast<double>(px.size());
  for (int ch = 0; ch < 3; ++ch) {
    double mean = sum[ch] / n;
    s.variance[ch] = sumsq[ch] / n - mean * mean;
    if (s.variance[ch] < 0) s.variance[ch] = 0;
  }
  return s;
}

// Returns palette (mean color of every final bucket).
std::vector<PaletteEntry> medianCut(const std::vector<uint8_t>& img,
                                    unsigned w, unsigned h,
                                    const Config& cfg) {
  std::deque<Bucket> buckets;
  Bucket first;
  first.px.resize(static_cast<size_t>(w) * h);
  for (size_t i = 0, n = first.px.size(); i < n; ++i) {
    first.px[i] = packRGB(img[i * 3], img[i * 3 + 1], img[i * 3 + 2]);
  }
  first.stats = computeStats(first.px);
  buckets.push_back(std::move(first));

  while (buckets.size() < cfg.maxColors) {
    // pick the splittable bucket with the largest channel variance
    size_t best = buckets.size();
    double bestVar = cfg.varThreshold;
    int bestCh = -1;
    for (size_t i = 0; i < buckets.size(); ++i) {
      Bucket& bk = buckets[i];
      if (!bk.splittable || bk.px.size() < 2) { bk.splittable = false; continue; }
      int ch = 0;
      for (int c = 1; c < 3; ++c)
        if (bk.stats.variance[c] > bk.stats.variance[ch]) ch = c;
      double v = bk.stats.variance[ch];
      if (bk.stats.maxv[ch] == bk.stats.minv[ch]) { bk.splittable = false; continue; }
      if (v > bestVar) { bestVar = v; best = i; bestCh = ch; }
    }
    if (best == buckets.size()) break;  // nothing left worth splitting

    Bucket bk = std::move(buckets[best]);
    buckets.erase(buckets.begin() + static_cast<std::ptrdiff_t>(best));

    std::nth_element(
        bk.px.begin(), bk.px.begin() +
                           static_cast<std::ptrdiff_t>(bk.px.size() / 2),
        bk.px.end(), [bestCh](uint32_t a, uint32_t b) {
          return channelOf(a, bestCh) < channelOf(b, bestCh);
        });

    Bucket lo, hi;
    lo.px.assign(bk.px.begin(),
                 bk.px.begin() +
                     static_cast<std::ptrdiff_t>(bk.px.size() / 2));
    hi.px.assign(bk.px.begin() +
                     static_cast<std::ptrdiff_t>(bk.px.size() / 2),
                 bk.px.end());
    lo.stats = computeStats(lo.px);
    hi.stats = computeStats(hi.px);
    buckets.push_back(std::move(lo));
    buckets.push_back(std::move(hi));
  }

  std::vector<PaletteEntry> palette;
  palette.reserve(buckets.size());
  for (const Bucket& bk : buckets) {
    double sum[3] = {0, 0, 0};
    for (uint32_t c : bk.px) {
      sum[0] += (c >> 16) & 0xFF;
      sum[1] += (c >> 8) & 0xFF;
      sum[2] += c & 0xFF;
    }
    double n = static_cast<double>(bk.px.size());
    palette.push_back({static_cast<int>(std::round(sum[0] / n)),
                       static_cast<int>(std::round(sum[1] / n)),
                       static_cast<int>(std::round(sum[2] / n))});
  }
  return palette;
}

// ---------------------------------------------------------------------------
// Nearest-palette lookup: 32^3 grid LUT (nearest entry to each cell center)
// plus an open-addressing exact-match table for palette members.
// ---------------------------------------------------------------------------
struct Quantizer {
  explicit Quantizer(const std::vector<PaletteEntry>& pal) : palette(pal) {
    std::fill(lut.begin(), lut.end(), 0);
    for (int cell = 0; cell < GRID * GRID * GRID; ++cell) {
      int cr = (cell / (GRID * GRID)) * 8 + 4;
      int cg = ((cell / GRID) % GRID) * 8 + 4;
      int cb = (cell % GRID) * 8 + 4;
      long bestDist = std::numeric_limits<long>::max();
      int bestIdx = 0;
      for (size_t i = 0; i < palette.size(); ++i) {
        long dr = cr - palette[i].r, dg = cg - palette[i].g,
             db = cb - palette[i].b;
        long d = dr * dr + dg * dg + db * db;
        if (d < bestDist) { bestDist = d; bestIdx = static_cast<int>(i); }
      }
      lut[cell] = static_cast<int16_t>(bestIdx);
    }
    std::fill(exactKey.begin(), exactKey.end(), UINT32_MAX);
    for (size_t i = 0; i < palette.size(); ++i) {
      uint32_t key = packRGB(palette[i].r, palette[i].g, palette[i].b);
      uint32_t slot = key & (EXACT_SIZE - 1);
      while (exactKey[slot] != UINT32_MAX) slot = (slot + 1) & (EXACT_SIZE - 1);
      exactKey[slot] = key;
      exactIdx[slot] = static_cast<int16_t>(i);
    }
  }

  int nearestIndex(int r, int g, int b) const {
    uint32_t key = packRGB(r, g, b);
    // exact palette membership first (guarantees lossless flat regions)
    uint32_t slot = key & (EXACT_SIZE - 1);
    for (;;) {
      if (exactKey[slot] == UINT32_MAX) break;
      if (exactKey[slot] == key) return exactIdx[slot];
      slot = (slot + 1) & (EXACT_SIZE - 1);
    }
    int cell = (r >> 3) * GRID * GRID + (g >> 3) * GRID + (b >> 3);
    return lut[cell];
  }

  static constexpr int GRID = 32;
  static constexpr uint32_t EXACT_SIZE = 1024;  // power of two, > 2 * 256
  const std::vector<PaletteEntry>& palette;
  std::array<int16_t, GRID * GRID * GRID> lut;
  std::array<uint32_t, EXACT_SIZE> exactKey;
  std::array<int16_t, EXACT_SIZE> exactIdx;
};

// ---------------------------------------------------------------------------
// Floyd-Steinberg dithering; outputs palette indices. Strength 0 = plain
// per-pixel quantization with no error diffusion.
// ---------------------------------------------------------------------------
std::vector<uint8_t> floydSteinberg(const std::vector<uint8_t>& img,
                                    unsigned w, unsigned h,
                                    const Quantizer& q, double strength) {
  const size_t n = static_cast<size_t>(w) * h;
  std::vector<RGB> buf(n);
  for (size_t i = 0; i < n; ++i) {
    buf[i] = {static_cast<double>(img[i * 3]),
              static_cast<double>(img[i * 3 + 1]),
              static_cast<double>(img[i * 3 + 2])};
  }
  std::vector<uint8_t> indices(n);

  for (unsigned y = 0; y < h; ++y) {
    for (unsigned x = 0; x < w; ++x) {
      size_t i = static_cast<size_t>(y) * w + x;
      int r = clampByte(buf[i].r);
      int g = clampByte(buf[i].g);
      int b = clampByte(buf[i].b);
      int idx = q.nearestIndex(r, g, b);
      indices[i] = static_cast<uint8_t>(idx);
      const PaletteEntry& pe = q.palette[static_cast<size_t>(idx)];
      double er = (r - pe.r) * strength;
      double eg = (g - pe.g) * strength;
      double eb = (b - pe.b) * strength;
      if (x + 1 < w) {
        RGB& p = buf[i + 1];
        p.r += er * 7.0 / 16.0; p.g += eg * 7.0 / 16.0; p.b += eb * 7.0 / 16.0;
      }
      if (y + 1 < h) {
        if (x > 0) {
          RGB& p = buf[i + w - 1];
          p.r += er * 3.0 / 16.0; p.g += eg * 3.0 / 16.0; p.b += eb * 3.0 / 16.0;
        }
        RGB& p = buf[i + w];
        p.r += er * 5.0 / 16.0; p.g += eg * 5.0 / 16.0; p.b += eb * 5.0 / 16.0;
        if (x + 1 < w) {
          RGB& p = buf[i + w + 1];
          p.r += er * 1.0 / 16.0; p.g += eg * 1.0 / 16.0; p.b += eb * 1.0 / 16.0;
        }
      }
    }
  }
  return indices;
}

// ---------------------------------------------------------------------------
// PNG encoding helpers (stock lodepng public API only)
// ---------------------------------------------------------------------------
void configureFilter(LodePNGState& state, LodePNGFilterStrategy strategy) {
  state.encoder.auto_convert = 0;
  state.encoder.filter_palette_zero = 0;  // allow the chosen strategy on palette images
  state.encoder.filter_strategy = strategy;
}

std::vector<uint8_t> encodeIndexed(const std::vector<uint8_t>& indices,
                                   unsigned w, unsigned h,
                                   const std::vector<PaletteEntry>& palette,
                                   LodePNGFilterStrategy strategy) {
  LodePNGState state;
  lodepng_state_init(&state);
  configureFilter(state, strategy);
  // input indices are always 8-bit; output uses the smallest depth the
  // palette fits in (1/2/4/8 bits), halving the raw stream on flat artwork.
  unsigned outDepth = 1;
  while ((1u << outDepth) < palette.size()) outDepth *= 2;
  state.info_raw.colortype = LCT_PALETTE;
  state.info_raw.bitdepth = 8;
  state.info_png.color.colortype = LCT_PALETTE;
  state.info_png.color.bitdepth = outDepth;
  for (const PaletteEntry& p : palette) {
    lodepng_palette_add(&state.info_raw,
                        static_cast<uint8_t>(p.r),
                        static_cast<uint8_t>(p.g),
                        static_cast<uint8_t>(p.b), 255);
    lodepng_palette_add(&state.info_png.color,
                        static_cast<uint8_t>(p.r),
                        static_cast<uint8_t>(p.g),
                        static_cast<uint8_t>(p.b), 255);
  }
  unsigned char* out = nullptr;
  size_t outsize = 0;
  unsigned err = lodepng_encode(&out, &outsize, indices.data(), w, h, &state);
  lodepng_state_cleanup(&state);
  if (err) {
    std::fprintf(stderr, "lodepng_encode (indexed) failed: %s\n",
                 lodepng_error_text(err));
    std::exit(2);
  }
  std::vector<uint8_t> png(out, out + outsize);
  std::free(out);
  return png;
}

std::vector<uint8_t> encodeRgb(const std::vector<uint8_t>& rgb, unsigned w,
                               unsigned h, LodePNGFilterStrategy strategy) {
  LodePNGState state;
  lodepng_state_init(&state);
  configureFilter(state, strategy);
  state.info_raw.colortype = LCT_RGB;
  state.info_raw.bitdepth = 8;
  state.info_png.color.colortype = LCT_RGB;
  state.info_png.color.bitdepth = 8;
  unsigned char* out = nullptr;
  size_t outsize = 0;
  unsigned err = lodepng_encode(&out, &outsize, rgb.data(), w, h, &state);
  lodepng_state_cleanup(&state);
  if (err) {
    std::fprintf(stderr, "lodepng_encode (rgb) failed: %s\n",
                 lodepng_error_text(err));
    std::exit(2);
  }
  std::vector<uint8_t> png(out, out + outsize);
  std::free(out);
  return png;
}

// Decode a PNG back to RGB and compute PSNR against the source.
// Returns infinity for an exact match.
double psnrAgainst(const std::vector<uint8_t>& png,
                   const std::vector<uint8_t>& source, unsigned w, unsigned h) {
  unsigned char* out = nullptr;
  unsigned dw = 0, dh = 0;
  unsigned err = lodepng_decode_memory(&out, &dw, &dh, png.data(), png.size(),
                                       LCT_RGBA, 8);
  if (err) {
    std::fprintf(stderr, "lodepng_decode_memory failed: %s\n",
                 lodepng_error_text(err));
    std::exit(2);
  }
  if (dw != w || dh != h) {
    std::fprintf(stderr, "decoded size mismatch\n");
    std::exit(2);
  }
  double se = 0;
  size_t count = static_cast<size_t>(w) * h * 3;
  for (size_t p = 0; p < static_cast<size_t>(w) * h; ++p) {
    for (int c = 0; c < 3; ++c) {
      double d = static_cast<double>(out[p * 4 + c]) - source[p * 3 + c];
      se += d * d;
    }
  }
  std::free(out);
  if (se == 0.0) return std::numeric_limits<double>::infinity();
  double mse = se / static_cast<double>(count);
  return 10.0 * std::log10(255.0 * 255.0 / mse);
}

// ---------------------------------------------------------------------------
// One full indexed pipeline run; reports its stage timings.
// ---------------------------------------------------------------------------
struct IndexedResult {
  std::vector<uint8_t> png;
  std::vector<PaletteEntry> palette;
  double paletteMs;
  double totalMs;
};

// Renumber palette indices in raster first-occurrence order. Median-cut
// bucket order has no spatial meaning, so neighboring pixels could have
// wildly different index values; renumbering makes Sub/Up/Paeth residuals
// small on flat artwork without touching any pixel color.
void remapIndicesRaster(std::vector<uint8_t>& indices,
                        std::vector<PaletteEntry>& palette) {
  std::array<int, 256> order;
  order.fill(-1);
  std::vector<PaletteEntry> newPal;
  newPal.reserve(palette.size());
  for (uint8_t idx : indices) {
    if (order[idx] == -1) {
      order[idx] = static_cast<int>(newPal.size());
      newPal.push_back(palette[idx]);
    }
  }
  std::vector<uint8_t> remap(palette.size());
  for (size_t i = 0; i < palette.size(); ++i) {
    if (order[i] != -1) remap[i] = static_cast<uint8_t>(order[i]);
  }
  for (uint8_t& idx : indices) idx = remap[idx];
  palette.swap(newPal);
}

IndexedResult runIndexed(const std::vector<uint8_t>& img, unsigned w,
                         unsigned h, const Config& cfg) {
  IndexedResult r;
  auto t0 = Clock::now();
  r.palette = medianCut(img, w, h, cfg);
  auto t1 = Clock::now();
  Quantizer q(r.palette);
  std::vector<uint8_t> indices =
      floydSteinberg(img, w, h, q, cfg.ditherStrength);
  remapIndicesRaster(indices, r.palette);
  r.png = encodeIndexed(indices, w, h, r.palette, cfg.filter);
  auto t2 = Clock::now();
  r.paletteMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  r.totalMs = std::chrono::duration<double, std::milli>(t2 - t0).count();
  return r;
}

double medianOf(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

const char* filterName(LodePNGFilterStrategy f) {
  switch (f) {
    case LFS_ZERO: return "zero";
    case LFS_ONE: return "one";
    case LFS_TWO: return "two";
    case LFS_THREE: return "three";
    case LFS_FOUR: return "four";
    case LFS_MINSUM: return "minsum";
    case LFS_ENTROPY: return "entropy";
    case LFS_BRUTE_FORCE: return "brute";
    default: return "predefined";
  }
}

void saveFile(const std::string& path, const std::vector<uint8_t>& data) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) { std::perror(path.c_str()); std::exit(2); }
  if (std::fwrite(data.data(), 1, data.size(), f) != data.size()) {
    std::perror(path.c_str()); std::exit(2);
  }
  std::fclose(f);
}

LodePNGFilterStrategy parseFilter(const std::string& s) {
  if (s == "zero") return LFS_ZERO;
  if (s == "one") return LFS_ONE;
  if (s == "two") return LFS_TWO;
  if (s == "three") return LFS_THREE;
  if (s == "four") return LFS_FOUR;
  if (s == "minsum") return LFS_MINSUM;
  if (s == "entropy") return LFS_ENTROPY;
  if (s == "brute") return LFS_BRUTE_FORCE;
  std::fprintf(stderr, "unknown filter strategy '%s'\n", s.c_str());
  std::exit(2);
}

void usage() {
  std::printf(
      "usage: indexed_pipeline_bench [options]\n"
      "  --max-colors N      palette size cap, default 256\n"
      "  --dither X          Floyd-Steinberg strength (0 disables), default 1.0\n"
      "  --var-threshold X   median-cut stop variance, default 0\n"
      "  --filter NAME       zero|one|two|three|four|minsum|entropy|brute,\n"
      "                      default minsum (per-row min residual over 5 filters)\n"
      "  --runs N            timing repetitions, default 5\n"
      "  --sample-size N     small sample image size, default 512\n"
      "  --perf-size N       performance gate image size, default 1024\n");
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", name); std::exit(2); }
      return argv[++i];
    };
    if (arg == "--max-colors") cfg.maxColors = std::stoul(next("--max-colors"));
    else if (arg == "--dither") cfg.ditherStrength = std::stod(next("--dither"));
    else if (arg == "--var-threshold") cfg.varThreshold = std::stod(next("--var-threshold"));
    else if (arg == "--filter") cfg.filter = parseFilter(next("--filter"));
    else if (arg == "--runs") cfg.runs = std::stoi(next("--runs"));
    else if (arg == "--sample-size") cfg.sampleSize = std::stoul(next("--sample-size"));
    else if (arg == "--perf-size") cfg.perfSize = std::stoul(next("--perf-size"));
    else if (arg == "-h" || arg == "--help") { usage(); return 0; }
    else { std::fprintf(stderr, "unknown argument '%s'\n", arg.c_str()); usage(); return 2; }
  }
  if (cfg.maxColors < 1 || cfg.maxColors > 256) {
    std::fprintf(stderr, "--max-colors must be in [1, 256]\n");
    return 2;
  }

  struct Class { const char* name; ImageGen gen; };
  const Class classes[] = {
      {"icon", genIcon},
      {"screenshot", genScreenshot},
      {"photo", genPhoto},
      {"solid", genSolid},
  };

  std::printf("== config: max_colors=%u dither=%.2f var_threshold=%.1f filter=%s "
              "runs=%d ==\n\n",
              cfg.maxColors, cfg.ditherStrength, cfg.varThreshold,
              filterName(cfg.filter), cfg.runs);

  // ---- small samples: size / PSNR / palette time, both encode paths -------
  std::printf("-- sample images %ux%u (bytes on disk; PSNR after decode) --\n",
              cfg.sampleSize, cfg.sampleSize);
  std::printf("%-10s %-22s %9s %8s %9s %10s %8s\n", "class", "path",
              "bytes", "vs now", "PSNR dB", "pal(ms)", "pal N");

  bool sizeGate = true, psnrGate = true, solidGate = true;
  for (const Class& c : classes) {
    unsigned s = cfg.sampleSize;
    std::vector<uint8_t> img(static_cast<size_t>(s) * s * 3);
    c.gen(img, s, s);

    // current encoder baseline: truecolor straight out, fixed filter 0
    std::vector<uint8_t> pngNow = encodeRgb(img, s, s, LFS_ZERO);
    double psnrNow = psnrAgainst(pngNow, img, s, s);
    std::printf("%-10s %-22s %9zu %8s %9s %10s %8s\n", c.name,
                "rgb zero (now)", pngNow.size(), "-",
                std::isinf(psnrNow) ? "inf" : std::to_string(psnrNow).c_str(),
                "-", "-");

    std::vector<uint8_t> pngRgb = encodeRgb(img, s, s, cfg.filter);
    double psnrRgb = psnrAgainst(pngRgb, img, s, s);
    double rgbReduce = 100.0 * (1.0 - static_cast<double>(pngRgb.size()) /
                                         pngNow.size());
    std::printf("%-10s %-22s %9zu %7.1f%% %9s %10s %8s\n", c.name,
                (std::string("rgb ") + filterName(cfg.filter)).c_str(),
                pngRgb.size(), rgbReduce,
                std::isinf(psnrRgb) ? "inf" : std::to_string(psnrRgb).c_str(),
                "-", "-");

    IndexedResult ir = runIndexed(img, s, s, cfg);
    double psnrIdx = psnrAgainst(ir.png, img, s, s);
    double reduce = 100.0 * (1.0 - static_cast<double>(ir.png.size()) /
                                     pngNow.size());
    char psnrBuf[32];
    if (std::isinf(psnrIdx)) std::snprintf(psnrBuf, sizeof(psnrBuf), "inf");
    else std::snprintf(psnrBuf, sizeof(psnrBuf), "%.2f", psnrIdx);
    std::printf("%-10s %-22s %9zu %7.1f%% %9s %10.2f %8zu\n", c.name,
                (std::string("indexed ") + filterName(cfg.filter)).c_str(),
                ir.png.size(), reduce, psnrBuf, ir.paletteMs,
                ir.palette.size());

    if (reduce < 40.0) sizeGate = false;
    if (!std::isinf(psnrIdx) && psnrIdx < 30.0) psnrGate = false;
    if (std::strcmp(c.name, "solid") == 0) {
      if (!std::isinf(psnrIdx) || ir.palette.size() != 1) solidGate = false;
    }

    saveFile(std::string("artifacts/samples/") + c.name + "_now.png", pngNow);
    saveFile(std::string("artifacts/samples/") + c.name + "_rgb.png", pngRgb);
    saveFile(std::string("artifacts/samples/") + c.name + "_indexed.png", ir.png);
  }

  // ---- 1024x1024 performance gate ------------------------------------------
  std::printf("\n-- full pipeline timing %ux%u, median of %d runs --\n",
              cfg.perfSize, cfg.perfSize, cfg.runs);
  std::printf("%-10s %12s %12s %12s %12s\n", "class", "pal(ms)",
              "indexed(ms)", "rgb now(ms)", "index bytes");
  bool perfGate = true;
  double worstIndexed = 0;
  for (const Class& c : classes) {
    unsigned s = cfg.perfSize;
    std::vector<uint8_t> img(static_cast<size_t>(s) * s * 3);
    c.gen(img, s, s);

    std::vector<double> palTimes, totalTimes;
    std::vector<uint8_t> png;
    for (int k = 0; k < cfg.runs; ++k) {
      IndexedResult ir = runIndexed(img, s, s, cfg);
      palTimes.push_back(ir.paletteMs);
      totalTimes.push_back(ir.totalMs);
      png = std::move(ir.png);
    }
    std::vector<double> rgbTimes;
    for (int k = 0; k < cfg.runs; ++k) {
      auto t0 = Clock::now();
      auto pngNow = encodeRgb(img, s, s, LFS_ZERO);
      rgbTimes.push_back(std::chrono::duration<double, std::milli>(
                             Clock::now() - t0)
                             .count());
      (void)pngNow;
    }
    double palMed = medianOf(palTimes);
    double totalMed = medianOf(totalTimes);
    double rgbMed = medianOf(rgbTimes);
    worstIndexed = std::max(worstIndexed, totalMed);
    if (totalMed > 400.0) perfGate = false;
    std::printf("%-10s %12.2f %12.2f %12.2f %12zu\n", c.name, palMed, totalMed,
                rgbMed, png.size());
  }

  // ---- acceptance summary --------------------------------------------------
  std::printf("\n== acceptance ==\n");
  std::printf("[%s] indexed output >=40%% smaller than current baseline, all 4 classes\n",
              sizeGate ? "PASS" : "FAIL");
  std::printf("[%s] indexed PSNR >= 30 dB, all 4 classes\n",
              psnrGate ? "PASS" : "FAIL");
  std::printf("[%s] solid image exact (PSNR inf, single palette entry)\n",
              solidGate ? "PASS" : "FAIL");
  std::printf("[%s] %ux%u full pipeline <= 400 ms (worst median %.2f ms)\n",
              perfGate ? "PASS" : "FAIL", cfg.perfSize, cfg.perfSize,
              worstIndexed);

  return (sizeGate && psnrGate && solidGate && perfGate) ? 0 : 1;
}

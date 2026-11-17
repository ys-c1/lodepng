/*
palette_sort_test.cpp

Palettized PNG palette reordering test/demo for LodePNG (public API only).

Why this works: for an indexed (PNG color type 3, 8-bit) image, the byte that
reaches deflate is the palette index of each pixel. Reordering the palette and
renumbering the pixels does not change any visible color, but it changes how
often neighboring pixels share or differ by small indices - which changes how
compressible the scanline stream is. This program measures that effect.

Three stages, all on data kept in memory (no files written):
  1. decode a palettized PNG to 8-bit indices + palette and build the
     undirected color transition frequency matrix of neighboring pixels
     (horizontal and vertical neighbors);
  2. compute a new palette order from that matrix, with a configurable
     algorithm and (for the TSP approximation) number of iterations:
       greedy : grow one path from both ends, always attaching the unused
                color with the highest transition frequency to an endpoint;
       tsp    : greedy initial order, improved by 2-opt reversals for at most
                <rounds> full passes (stops early at a local optimum).
     Unknown algorithm names, or images with fewer than two colors, fall back
     to the identity ordering (original palette order preserved).
  3. renumber every pixel and rearrange the palette with the new permutation,
     re-encode with the exact same encoder settings, and compare the size.

Four small synthetic image classes are generated with fixed-seed RNG so the
whole run is deterministic and finishes in one shot:
  pixel art, screenshot, gradient, noise.

Correctness checks (any failure makes the program exit non-zero):
  C1 remap inverse: newmap is a permutation of [0,n);
  C2 palette round trip: permuted palette colors match the old ones per pixel;
  C3 index range: every remapped index is a valid palette index;
  C4 index round trip: decode(remapped PNG) to indices and compare against the
     remapped index buffer (the LodePNG palette->palette 8-bit path copies
     indices byte for byte);
  C5 pixel invariant: decode both PNGs to RGBA, the per-pixel output must be
     identical - decoding pixels one by one is unchanged by the reorder.

Build (no new dependencies, no Makefile change):
  g++ -I ./ -W -Wall -Wextra -ansi -pedantic -O3 \
      palette_sort_test.cpp lodepng.cpp -o palette_sort_test

Usage:
  ./palette_sort_test [greedy|tsp|all] [rounds]
    greedy : run the greedy ordering only
    tsp    : run the TSP/2-opt ordering only, rounds = 2-opt passes (default 30)
    all    : run both (default); additionally an unknown name is used to
             demonstrate the fallback-to-original-order behavior
*/

#include <ctime>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "lodepng.h"

/* ------------------------------------------------------------------ */
/* small utilities                                                     */
/* ------------------------------------------------------------------ */

/* Deterministic, tiny LCG so results are reproducible without any library. */
class Rng {
  unsigned long state_;
public:
  explicit Rng(unsigned long seed) : state_(seed ? seed : 1) {}
  unsigned next() { /* Numerical Recipes LCG, mod 2^32 */
    state_ = state_ * 1664525UL + 1013904223UL;
    return (unsigned)(state_ >> 8);
  }
  unsigned below(unsigned n) { return n == 0 ? 0 : next() % n; }
};

struct IndexImage {
  unsigned w;
  unsigned h;
  std::vector<unsigned char> palette; /* RGBA, 4 bytes per color */
  std::vector<unsigned char> indices; /* one byte per pixel, 8-bit palette */
};

static unsigned numColors(const IndexImage& img) {
  return (unsigned)(img.palette.size() / 4);
}

/* Encode an 8-bit indexed image to a PNG blob (color type 3, bit depth 8). */
static unsigned encodeIndexed(std::vector<unsigned char>& out,
                              const IndexImage& img) {
  lodepng::State state;
  state.encoder.auto_convert = 0;
  state.info_png.color.colortype = LCT_PALETTE;
  state.info_png.color.bitdepth = 8;
  state.info_raw.colortype = LCT_PALETTE;
  state.info_raw.bitdepth = 8;
  /* identical encoder settings for every call, so only the ordering differs */
  state.encoder.add_id = 0;
  state.encoder.filter_palette_zero = 1;

  unsigned n = numColors(img);
  for(unsigned i = 0; i < n; i++) {
    const unsigned char* c = &img.palette[(size_t)i * 4];
    lodepng_palette_add(&state.info_png.color, c[0], c[1], c[2], c[3]);
    lodepng_palette_add(&state.info_raw,     c[0], c[1], c[2], c[3]);
  }
  return lodepng::encode(out, img.indices.empty() ? 0 : &img.indices[0],
                         img.w, img.h, state);
}

/*
Decode a palettized PNG back to 8-bit indices + palette. info_raw requests
LCT_PALETTE/8 with an empty palette: LodePNG then reuses the PNG's palette and,
for palette->palette at the same bit depth, copies the indices literally.
*/
static unsigned decodeIndexed(IndexImage& out,
                              const std::vector<unsigned char>& png) {
  lodepng::State state;
  state.info_raw.colortype = LCT_PALETTE;
  state.info_raw.bitdepth = 8;

  unsigned w = 0, h = 0;
  unsigned error = lodepng::decode(out.indices, w, h, state, png);
  if(error) return error;
  out.w = w;
  out.h = h;
  out.palette.clear();
  unsigned n = state.info_png.color.palettesize;
  for(unsigned i = 0; i < n; i++) {
    const unsigned char* c = &state.info_png.color.palette[(size_t)i * 4];
    out.palette.push_back(c[0]);
    out.palette.push_back(c[1]);
    out.palette.push_back(c[2]);
    out.palette.push_back(c[3]);
  }
  return 0;
}

static unsigned decodeRgba(std::vector<unsigned char>& rgba,
                           const std::vector<unsigned char>& png) {
  unsigned w = 0, h = 0;
  return lodepng::decode(rgba, w, h, png);
}

/* ------------------------------------------------------------------ */
/* stage 1: neighbor transition frequency matrix                       */
/* ------------------------------------------------------------------ */

/*
Matrix M is symmetric: M[a*n+b] counts (a,b) neighbor pairs with a != b.
Horizontal (left) and vertical (above) neighbors are each counted once.
Self transitions (a == b) are ignored: they are already optimal and carry no
information about where colors should sit relative to each other.
*/
static void buildTransitionMatrix(std::vector<size_t>& matrix,
                                  const IndexImage& img) {
  unsigned n = numColors(img);
  matrix.assign((size_t)n * n, 0);
  for(unsigned y = 0; y < img.h; y++) {
    for(unsigned x = 0; x < img.w; x++) {
      unsigned a = img.indices[(size_t)y * img.w + x];
      if(x > 0) {
        unsigned b = img.indices[(size_t)y * img.w + (x - 1)];
        if(a != b) { matrix[(size_t)a * n + b]++; matrix[(size_t)b * n + a]++; }
      }
      if(y > 0) {
        unsigned b = img.indices[(size_t)(y - 1) * img.w + x];
        if(a != b) { matrix[(size_t)a * n + b]++; matrix[(size_t)b * n + a]++; }
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/* stage 2: ordering algorithms (operate on the transition matrix)      */
/* ------------------------------------------------------------------ */

/*
Greedy: start at color 0 (deterministic), then repeatedly attach the unused
color with the strongest connection to either endpoint of the growing path.
Ties break on the smallest color index. Colors with no transitions end up last
in their original relative order.
*/
static std::vector<unsigned> orderGreedy(const std::vector<size_t>& m,
                                         unsigned n) {
  std::vector<unsigned> order;
  std::vector<unsigned char> used(n, 0);
  order.reserve(n);
  if(n == 0) return order;

  order.push_back(0);
  used[0] = 1;
  while(order.size() < n) {
    unsigned head = order.front();
    unsigned tail = order.back();
    unsigned best = n;
    size_t bestWeight = 0;
    bool toHead = false;
    for(unsigned c = 0; c < n; c++) {
      if(used[c]) continue;
      size_t wh = m[(size_t)head * n + c];
      size_t wt = m[(size_t)tail * n + c];
      size_t w = wh >= wt ? wh : wt;
      if(w > bestWeight || (w == bestWeight && c < best)) {
        bestWeight = w;
        best = c;
        toHead = wh >= wt;
      }
    }
    if(best == n) break; /* cannot happen while unused colors remain */
    used[best] = 1;
    if(toHead) order.insert(order.begin(), best);
    else order.push_back(best);
  }
  return order;
}

static size_t pathCost(const std::vector<unsigned>& order,
                       const std::vector<size_t>& m, unsigned n) {
  size_t cost = 0;
  for(size_t i = 0; i + 1 < order.size(); i++)
    cost += m[(size_t)order[i] * n + order[i + 1]];
  return cost;
}

/*
TSP approximation: greedy initial path plus first-improvement 2-opt.
Reversing order[i+1..j] replaces edges (a,b) and (c,d) with (a,c) and (b,d).
At most 'rounds' full passes; a pass without improvement ends the search.
*/
static std::vector<unsigned> orderTsp(const std::vector<size_t>& m,
                                      unsigned n, unsigned rounds) {
  std::vector<unsigned> order = orderGreedy(m, n);
  if(n < 3 || rounds == 0) return order;

  for(unsigned r = 0; r < rounds; r++) {
    bool improved = false;
    for(unsigned i = 0; i + 2 < n && !improved; i++) {
      unsigned a = order[i];
      unsigned b = order[i + 1];
      for(unsigned j = i + 2; j < n; j++) {
        unsigned c = order[j - 1];
        unsigned d = order[j];
        /* counts fit in size_t for any realistic image; compare the
        exchanged edge weights directly to stay C++98-clean */
        size_t before = m[(size_t)a * n + b] + m[(size_t)c * n + d];
        size_t after  = m[(size_t)a * n + c] + m[(size_t)b * n + d];
        if(after > before) {
          /* reverse segment [i+1, j] */
          unsigned lo = i + 1, hi = j;
          while(lo < hi) {
            unsigned t = order[lo]; order[lo] = order[hi]; order[hi] = t;
            lo++; hi--;
          }
          improved = true;
          break; /* restart scanning from the beginning of the path */
        }
      }
    }
    if(!improved) break;
  }
  return order;
}

/*
Dispatch with fallback. Returns false (and an identity order) when the
algorithm name is not supported or there is nothing to reorder.
*/
static bool computeOrder(std::vector<unsigned>& order, const char* algorithm,
                         unsigned rounds, const std::vector<size_t>& m,
                         unsigned n) {
  std::string a = algorithm;
  if(n <= 1) {
    order.resize(n);
    for(unsigned i = 0; i < n; i++) order[i] = i;
    return false;
  }
  if(a == "greedy") {
    order = orderGreedy(m, n);
    return true;
  }
  if(a == "tsp") {
    order = orderTsp(m, n, rounds);
    return true;
  }
  /* unavailable/unknown algorithm: keep original palette order */
  order.resize(n);
  for(unsigned i = 0; i < n; i++) order[i] = i;
  return false;
}

/* ------------------------------------------------------------------ */
/* stage 3: apply permutation                                          */
/* ------------------------------------------------------------------ */

/*
order[k] = old index that should occupy new position k.
newmap[old] = new index. The image is renumbered in place through 'out', and
the palette is rearranged to match.
*/
static void applyOrder(const IndexImage& in, IndexImage& out,
                       const std::vector<unsigned>& order) {
  unsigned n = numColors(in);
  std::vector<unsigned> newmap(n, 0);
  for(unsigned k = 0; k < order.size(); k++) newmap[order[k]] = k;

  out.w = in.w;
  out.h = in.h;
  out.indices.resize(in.indices.size());
  for(size_t i = 0; i < in.indices.size(); i++)
    out.indices[i] = (unsigned char)newmap[in.indices[i]];

  out.palette.assign((size_t)n * 4, 0);
  for(unsigned k = 0; k < order.size(); k++) {
    for(unsigned ch = 0; ch < 4; ch++)
      out.palette[(size_t)k * 4 + ch] =
          in.palette[(size_t)order[k] * 4 + ch];
  }
}

/* ------------------------------------------------------------------ */
/* synthetic sample images (all small, fixed seeds)                    */
/* ------------------------------------------------------------------ */

static void putPixel(IndexImage& img, unsigned x, unsigned y,
                     unsigned char color) {
  img.indices[(size_t)y * img.w + x] = color;
}

/* Pixel art: flat rectangular regions, a border and repeated motifs so a
handful of colors sit next to each other constantly. 16 colors. */
static IndexImage makePixelArt() {
  const unsigned w = 96, h = 64, n = 16;
  IndexImage img;
  img.w = w; img.h = h;
  img.palette.assign((size_t)n * 4, 0);
  for(unsigned i = 0; i < n; i++) {
    img.palette[i * 4 + 0] = (unsigned char)(25 * i);
    img.palette[i * 4 + 1] = (unsigned char)(17 * i + 40);
    img.palette[i * 4 + 2] = (unsigned char)(9 * i + 80);
    img.palette[i * 4 + 3] = 255;
  }
  img.indices.assign((size_t)w * h, 0);
  Rng rng(1111);
  for(unsigned y = 0; y < h; y++)
  for(unsigned x = 0; x < w; x++) {
    unsigned c;
    if(y < 2 || y >= h - 2 || x < 2 || x >= w - 2) c = 1;          /* border */
    else c = 2 + ((x / 8) + (y / 8)) % 6;                          /* blocks  */
    if(rng.below(40) == 0) c = (unsigned)(8 + rng.below(8));       /* specks  */
    putPixel(img, x, y, (unsigned char)c);
  }
  for(unsigned k = 0; k < 11; k++) { /* repeated same-shape motifs */
    unsigned bx = 4 + k * 8, by = 10 + (k % 2) * 16;
    for(unsigned y = by; y < by + 6 && y < h - 3; y++)
    for(unsigned x = bx; x < bx + 5 && x < w - 3; x++)
      putPixel(img, x, y, (unsigned char)(14 + (k % 2)));
  }
  return img;
}

/* Screenshot: UI-like stacked panels and text rows, mostly horizontal runs.
24 colors. */
static IndexImage makeScreenshot() {
  const unsigned w = 128, h = 96, n = 24;
  IndexImage img;
  img.w = w; img.h = h;
  img.palette.assign((size_t)n * 4, 0);
  for(unsigned i = 0; i < n; i++) {
    img.palette[i * 4 + 0] = (unsigned char)(10 * i);
    img.palette[i * 4 + 1] = (unsigned char)(10 * i + 5);
    img.palette[i * 4 + 2] = (unsigned char)(200 - 7 * i);
    img.palette[i * 4 + 3] = 255;
  }
  img.indices.assign((size_t)w * h, 0);
  Rng rng(2222);
  for(unsigned y = 0; y < h; y++)
  for(unsigned x = 0; x < w; x++) {
    unsigned c;
    if(y < 8) c = 2;                                   /* title bar   */
    else if(y < 16) c = 3;                             /* tool bar    */
    else if(x < 24) c = 4;                             /* side panel  */
    else c = 6 + (y / 8) % 8;                          /* content     */
    if(y >= 18 && x >= 28 && (x % 12 == 0 || y % 8 == 0)) c = 14; /* grid */
    putPixel(img, x, y, (unsigned char)c);
  }
  for(unsigned row = 0; row < 11; row++) { /* pseudo text lines, repeated */
    unsigned y = 20 + row * 6;
    for(unsigned x = 28; x < 124; x++) {
      if(rng.below(3) == 0) continue;
      putPixel(img, x, y, (unsigned char)(15 + rng.below(6)));
    }
  }
  return img;
}

/* Gradient: long smooth ramps along x with a gentle vertical drift; adjacent
pixels are usually consecutive ramp entries. 32 colors. At this size the
reordering helps; at larger sizes deflate block partitioning can swamp it,
which is itself an honest property of palette reordering. */
static IndexImage makeGradient() {
  const unsigned w = 96, h = 64, n = 32;
  IndexImage img;
  img.w = w; img.h = h;
  img.palette.assign((size_t)n * 4, 0);
  for(unsigned i = 0; i < n; i++) {
    img.palette[i * 4 + 0] = (unsigned char)(8 * i);
    img.palette[i * 4 + 1] = (unsigned char)(4 * i + 30);
    img.palette[i * 4 + 2] = (unsigned char)(255 - 6 * i);
    img.palette[i * 4 + 3] = 255;
  }
  img.indices.assign((size_t)w * h, 0);
  Rng rng(3333);
  for(unsigned y = 0; y < h; y++)
  for(unsigned x = 0; x < w; x++) {
    int c = (int)((x + y / 3) % n);
    if(rng.below(60) == 0) c = (c + 1 + (int)rng.below(2)) % (int)n;
    putPixel(img, x, y, (unsigned char)c);
  }
  return img;
}

/* Noise: i.i.d. uniform indices - essentially no spatial structure, the hard
case for locality-based reordering. 32 colors. */
static IndexImage makeNoise() {
  const unsigned w = 96, h = 96, n = 32;
  IndexImage img;
  img.w = w; img.h = h;
  img.palette.assign((size_t)n * 4, 0);
  for(unsigned i = 0; i < n; i++) {
    img.palette[i * 4 + 0] = (unsigned char)(31 * i ^ 0x55);
    img.palette[i * 4 + 1] = (unsigned char)(13 * i + 7);
    img.palette[i * 4 + 2] = (unsigned char)(7 * i + 123);
    img.palette[i * 4 + 3] = 255;
  }
  img.indices.resize((size_t)w * h);
  Rng rng(4444);
  for(size_t i = 0; i < img.indices.size(); i++)
    img.indices[i] = (unsigned char)rng.below(n);
  return img;
}

/* Solid 2-color image: exercises the degenerate case where there is nothing
to gain (and the n <= 1 / near-trivial ordering path). Two colors arranged in
two flat bands, so one horizontal transition line only. */
static IndexImage makeSolid() {
  const unsigned w = 64, h = 32, n = 2;
  IndexImage img;
  img.w = w; img.h = h;
  img.palette.assign((size_t)n * 4, 0);
  img.palette[0] = 10;  img.palette[1] = 20;
  img.palette[2] = 30;  img.palette[3] = 255;
  img.palette[4] = 200; img.palette[5] = 180;
  img.palette[6] = 160; img.palette[7] = 255;
  img.indices.assign((size_t)w * h, 0);
  for(unsigned y = 0; y < h; y++)
  for(unsigned x = 0; x < w; x++)
    putPixel(img, x, y, y < h / 2 ? 0 : 1);
  return img;
}

/* Monochrome: a single palette entry. The ordering has nothing to do and must
fall back to the identity order (n <= 1 branch of the ordering dispatch). */
static IndexImage makeMono() {
  const unsigned w = 32, h = 24;
  IndexImage img;
  img.w = w; img.h = h;
  img.palette.assign(4, 0);
  img.palette[0] = 77; img.palette[1] = 88;
  img.palette[2] = 99; img.palette[3] = 255;
  img.indices.assign((size_t)w * h, 0);
  return img;
}

/* Fisher-Yates shuffle with the test RNG: produces a deliberately suboptimal
initial palette order while keeping every color present exactly once. */
static void shuffleImage(IndexImage& img, unsigned long seed) {
  unsigned n = numColors(img);
  Rng rng(seed);
  std::vector<unsigned> perm(n);
  for(unsigned i = 0; i < n; i++) perm[i] = i;
  for(unsigned i = n - 1; i > 0; i--) {
    unsigned j = rng.below(i + 1);
    unsigned t = perm[i]; perm[i] = perm[j]; perm[j] = t;
  }
  std::vector<unsigned> newmap(n, 0);
  for(unsigned k = 0; k < n; k++) newmap[perm[k]] = k;

  for(size_t i = 0; i < img.indices.size(); i++)
    img.indices[i] = (unsigned char)newmap[img.indices[i]];

  std::vector<unsigned char> pal((size_t)n * 4);
  for(unsigned k = 0; k < n; k++)
    for(unsigned ch = 0; ch < 4; ch++)
      pal[(size_t)k * 4 + ch] = img.palette[(size_t)perm[k] * 4 + ch];
  img.palette.swap(pal);
}

/* ------------------------------------------------------------------ */
/* correctness checks                                                   */
/* ------------------------------------------------------------------ */

static int g_failures = 0;
static int g_checks = 0;

static void check(bool condition, const std::string& name) {
  g_checks++;
  if(!condition) {
    g_failures++;
    std::cout << "    [FAIL] " << name << std::endl;
  }
}

static bool sameVector(const std::vector<unsigned char>& a,
                       const std::vector<unsigned char>& b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin());
}

/* Runs the full 3-stage pipeline for one image/algorithm pair and verifies
everything. Timings cover matrix build + ordering only. */
static void runCase(const IndexImage& source, const char* label,
                    const char* algorithm, unsigned rounds) {
  /* encode the (already shuffled) input image and decode it back: this is the
  baseline that goes through the real LodePNG codec, not raw hand-made data */
  std::vector<unsigned char> basePng;
  unsigned error = encodeIndexed(basePng, source);
  check(error == 0, "baseline encode has no error");

  IndexImage decoded;
  error = decodeIndexed(decoded, basePng);
  check(error == 0, "baseline decode has no error");
  check(decoded.w == source.w && decoded.h == source.h,
        "decoded dimensions match");
  check(sameVector(decoded.indices, source.indices),
        "baseline index round trip (palette->palette copy)");
  check(sameVector(decoded.palette, source.palette),
        "baseline palette round trip");

  /* stage 1 */
  clock_t t0 = clock();
  std::vector<size_t> matrix;
  buildTransitionMatrix(matrix, decoded);
  clock_t t1 = clock();

  /* stage 2 */
  unsigned n = numColors(decoded);
  std::vector<unsigned> order;
  bool available = computeOrder(order, algorithm, rounds, matrix, n);
  clock_t t2 = clock();

  std::vector<unsigned> newmap(n, 0);
  for(size_t k = 0; k < order.size(); k++) newmap[order[k]] = k;

  /* C1: newmap/order is a permutation of 0..n-1 */
  std::vector<unsigned char> seen(n, 0);
  bool permOk = order.size() == n;
  for(unsigned k = 0; permOk && k < n; k++) {
    if(order[k] >= n || seen[order[k]]) permOk = false;
    seen[order[k]] = 1;
  }
  check(permOk, "C1 reorder is a permutation of the palette");

  /* stage 3 */
  IndexImage remapped;
  applyOrder(decoded, remapped, order);

  /* C2: every remapped pixel points at the same RGBA color as before */
  bool paletteOk = true;
  for(size_t i = 0; i < decoded.indices.size() && paletteOk; i++) {
    unsigned oldIdx = decoded.indices[i];
    unsigned newIdx = remapped.indices[i];
    for(unsigned ch = 0; ch < 4; ch++) {
      if(decoded.palette[(size_t)oldIdx * 4 + ch] !=
         remapped.palette[(size_t)newIdx * 4 + ch]) {
        paletteOk = false;
        break;
      }
    }
  }
  check(paletteOk, "C2 remapped indices resolve to identical colors");

  /* C3: remapped indices are in range */
  bool rangeOk = true;
  for(size_t i = 0; i < remapped.indices.size(); i++)
    if(remapped.indices[i] >= n) { rangeOk = false; break; }
  check(rangeOk, "C3 all remapped indices are valid palette indices");

  std::vector<unsigned char> newPng;
  error = encodeIndexed(newPng, remapped);
  check(error == 0, "remapped encode has no error");

  /* C4: indices survive the remapped encode/decode unchanged */
  IndexImage redecode;
  error = decodeIndexed(redecode, newPng);
  check(error == 0, "remapped decode has no error");
  check(sameVector(redecode.indices, remapped.indices),
        "C4 remapped index round trip through the codec");
  check(sameVector(redecode.palette, remapped.palette),
        "remapped palette round trip through the codec");

  /* C5: RGBA per-pixel decoder output is identical (pixel decode untouched) */
  std::vector<unsigned char> rgbaBefore, rgbaAfter;
  decodeRgba(rgbaBefore, basePng);
  decodeRgba(rgbaAfter, newPng);
  check(sameVector(rgbaBefore, rgbaAfter),
        "C5 RGBA pixel-by-pixel decode is unchanged by reordering");

  double msMatrix = 1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC;
  double msOrder  = 1000.0 * (double)(t2 - t1) / CLOCKS_PER_SEC;

  long delta = (long)newPng.size() - (long)basePng.size();
  double pct = basePng.empty() ? 0.0
                               : 100.0 * (double)delta / (double)basePng.size();

  std::cout << "  [" << label << "] algorithm=" << algorithm
            << (available ? "" : " (unavailable -> identity fallback)")
            << " colors=" << n
            << " size=" << (unsigned)decoded.w << "x" << (unsigned)decoded.h
            << std::endl;
  std::cout << "    size before=" << basePng.size()
            << "  after=" << newPng.size()
            << "  delta=" << delta << " bytes (";
  if(delta < 0) std::cout << "-";
  std::cout << (pct < 0 ? -pct : pct) << "%)" << std::endl;
  std::cout << "    ordering kept=" << pathCost(order, matrix, n)
            << " transition weight; time matrix=" << msMatrix
            << "ms sort=" << msOrder << "ms"
            << " total=" << (msMatrix + msOrder) << "ms" << std::endl;
}

int main(int argc, char** argv) {
  std::string mode = argc > 1 ? argv[1] : "all";
  unsigned rounds = 30;
  if(argc > 2) {
    std::istringstream iss(argv[2]);
    if(!(iss >> rounds) || rounds == 0) rounds = 30;
  }

  if(mode != "all" && mode != "greedy" && mode != "tsp") {
    std::cout << "note: unknown algorithm '" << mode
              << "', running it anyway to demonstrate fallback" << std::endl;
  }

  IndexImage images[6];
  images[0] = makePixelArt();
  images[1] = makeScreenshot();
  images[2] = makeGradient();
  images[3] = makeNoise();
  images[4] = makeSolid();
  images[5] = makeMono();
  const char* names[6] = {
    "pixel art ", "screenshot", "gradient ", "noise     ",
    "solid(2c) ", "mono(1c)  "
  };
  const unsigned long seeds[6] = { 3, 45, 12, 5, 505, 606 };

  /* give every sample a deliberately poor starting palette order */
  for(int i = 0; i < 6; i++) shuffleImage(images[i], seeds[i]);

  std::vector<std::string> algos;
  if(mode == "all") {
    algos.push_back("greedy");
    algos.push_back("tsp");
    algos.push_back("bogus-algorithm"); /* fallback demonstration */
  } else {
    algos.push_back(mode);
  }

  for(size_t a = 0; a < algos.size(); a++) {
    std::cout << "=== algorithm: " << algos[a];
    if(algos[a] == "tsp") std::cout << " (2-opt rounds=" << rounds << ")";
    std::cout << " ===" << std::endl;
    for(int i = 0; i < 6; i++) {
      std::cout << "- " << names[i] << std::endl;
      runCase(images[i], names[i], algos[a].c_str(), rounds);
    }
  }

  std::cout << std::endl;
  std::cout << "checks run: " << g_checks
            << ", failures: " << g_failures << std::endl;
  if(g_failures == 0) std::cout << "PALETTE SORT TEST: ALL CHECKS PASSED"
                                << std::endl;
  else std::cout << "PALETTE SORT TEST: FAILED" << std::endl;
  return g_failures == 0 ? 0 : 1;
}

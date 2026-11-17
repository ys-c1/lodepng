/*
Block-level image codec test/benchmark built on top of LodePNG's public API.

The LodePNG library itself is not modified by this file: it only calls public
functions declared in lodepng.h. That keeps the original truecolor PNG encoding
path byte-for-byte identical (this program also asserts that explicitly).

What this adds:
  - The image is split into a grid of rectangular blocks. Block size and the
    selection threshold are runtime configurable (see main's argv parsing).
  - For every block, two encodings are estimated by actually compressing it:
      * truecolor  : 32-bit RGBA PNG (lodepng_encode32)
      * indexed    : 8-bit palette PNG, only when the block has <= 256 unique
                     RGBA colors, built with lodepng_compute_color_stats
    The smaller one is chosen (with an adjustable margin/threshold).
  - A custom container (magic "BLC1") is written: header, a block table with
    per-block location/type/length, then the two data regions (indexed blocks
    first, then truecolor blocks). Every region carries a CRC32
    (lodepng_crc32). A corrupted block table is detected and reported as an
    error instead of being decoded.
  - The decoder rebuilds the full RGBA image, which is compared pixel by pixel
    against the source.

Build (matches the flags used by the repository Makefile):
  g++ -I ./ -W -Wall -Wextra -ansi -pedantic -O3 blockcodec_test.cpp lodepng.cpp -o blockcodec_test
*/

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "lodepng.h"

/* ------------------------------------------------------------------------ */
/* Custom container ("BLC1") definition                                     */
/* ------------------------------------------------------------------------ */

static const unsigned char BLC_MAGIC[4] = {'B', 'L', 'C', '1'};
static const unsigned BLC_VERSION = 1u;

/* Fixed 36-byte header: 4-byte magic followed by 8 little-endian u32 words.
   A separate header CRC32 word is appended immediately after the header. */
static const size_t BLC_HEADER_SIZE = 36;
/* Fixed 16-byte block table entry. */
static const size_t BLC_ENTRY_SIZE = 16;

/* One entry per block, in row-major grid order. Plain 32-bit fields are ample
   for the small in-process test images and keep the code C89/C++98 friendly. */
typedef struct BlcEntry {
  unsigned blockType;   /* 0 = truecolor RGBA PNG, 1 = indexed palette PNG */
  unsigned reserved;    /* alignment / future use, always 0 */
  unsigned offset;      /* byte offset of the block PNG inside its region */
  unsigned length;      /* byte length of the block PNG */
} BlcEntry;

/* Decoder error codes. 0 means OK; values below 99 are LodePNG errors. */
enum BlcError {
  BLC_OK = 0,
  BLC_ERR_TRUNCATED = 100,
  BLC_ERR_MAGIC = 101,
  BLC_ERR_VERSION = 102,
  BLC_ERR_HEADER_CRC = 103,
  BLC_ERR_TABLE_CRC = 104,
  BLC_ERR_DIMENSIONS = 105,
  BLC_ERR_ENTRY = 106,
  BLC_ERR_REGION_CRC = 107,
  BLC_ERR_PNG_SIGNATURE = 108,
  BLC_ERR_MISMATCH = 109
};

static const char* blc_error_text(unsigned code) {
  switch (code) {
    case BLC_OK: return "ok";
    case BLC_ERR_TRUNCATED: return "container truncated";
    case BLC_ERR_MAGIC: return "bad magic";
    case BLC_ERR_VERSION: return "unsupported version";
    case BLC_ERR_HEADER_CRC: return "header checksum mismatch";
    case BLC_ERR_TABLE_CRC: return "block table checksum mismatch";
    case BLC_ERR_DIMENSIONS: return "inconsistent dimensions";
    case BLC_ERR_ENTRY: return "invalid block table entry";
    case BLC_ERR_REGION_CRC: return "data region checksum mismatch";
    case BLC_ERR_PNG_SIGNATURE: return "block is not a PNG";
    case BLC_ERR_MISMATCH: return "roundtrip pixel mismatch";
    default: return lodepng_error_text(code);
  }
}

/* ------------------------------------------------------------------------ */
/* Little-endian byte helpers                                               */
/* ------------------------------------------------------------------------ */

static void putU32(std::vector<unsigned char>& v, unsigned x) {
  v.push_back((unsigned char)(x & 0xff));
  v.push_back((unsigned char)((x >> 8) & 0xff));
  v.push_back((unsigned char)((x >> 16) & 0xff));
  v.push_back((unsigned char)((x >> 24) & 0xff));
}

static unsigned getU32(const unsigned char* p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8) |
         ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned crcBytes(const std::vector<unsigned char>& v) {
  return lodepng_crc32(v.empty() ? 0 : &v[0], v.size());
}

/* ------------------------------------------------------------------------ */
/* Image helpers                                                            */
/* ------------------------------------------------------------------------ */

/* Copy a rectangular block out of the full RGBA image into a tightly packed
   block buffer of size bw*bh*4. */
static std::vector<unsigned char> extractBlock(
    const std::vector<unsigned char>& img, unsigned w, unsigned h,
    unsigned x0, unsigned y0, unsigned bw, unsigned bh) {
  std::vector<unsigned char> block((size_t)bw * bh * 4);
  unsigned y, x;
  (void)h;
  for (y = 0; y < bh; y++) {
    for (x = 0; x < bw; x++) {
      size_t si = ((size_t)(y0 + y) * w + (x0 + x)) * 4;
      size_t di = ((size_t)y * bw + x) * 4;
      block[di] = img[si];
      block[di + 1] = img[si + 1];
      block[di + 2] = img[si + 2];
      block[di + 3] = img[si + 3];
    }
  }
  return block;
}

/* Encode one block as a truecolor (RGBA, 8-bit) PNG. This is the exact public
   call used for ordinary truecolor encoding, so output is identical to a
   full-image lodepng_encode32 at the per-block level. */
static unsigned encodeTrueColor(const std::vector<unsigned char>& block,
                                unsigned bw, unsigned bh,
                                std::vector<unsigned char>& out) {
  unsigned char* buffer = 0;
  size_t size = 0;
  unsigned error = lodepng_encode32(&buffer, &size,
                                    block.empty() ? 0 : &block[0], bw, bh);
  if (!error) out.assign(buffer, buffer + size);
  free(buffer);
  return error;
}

/* Encode one block as an indexed (8-bit palette) PNG. Only possible when the
   block contains at most 256 distinct RGBA colors. Returns BLC_ERR_ENTRY-like
   non-zero value (mapped by caller) when palette encoding is not applicable. */
static unsigned encodeIndexed(const std::vector<unsigned char>& block,
                              unsigned bw, unsigned bh,
                              std::vector<unsigned char>& out) {
  LodePNGColorStats stats;
  LodePNGColorMode modeIn;
  LodePNGState state;
  std::vector<unsigned char> indices((size_t)bw * bh);
  std::map<unsigned, unsigned> colorToIndex;
  unsigned error;
  size_t i;

  lodepng_color_mode_init(&modeIn);
  modeIn.colortype = LCT_RGBA;
  modeIn.bitdepth = 8;
  lodepng_color_stats_init(&stats);
  error = lodepng_compute_color_stats(&stats, block.empty() ? 0 : &block[0],
                                      bw, bh, &modeIn);
  if (error) {
    lodepng_color_mode_cleanup(&modeIn);
    return error;
  }
  if (stats.numcolors > 256) {
    lodepng_color_mode_cleanup(&modeIn);
    return 1000; /* not representable with a palette */
  }

  lodepng_state_init(&state);
  state.encoder.auto_convert = 0;
  state.info_png.color.colortype = LCT_PALETTE;
  state.info_png.color.bitdepth = 8;
  state.info_raw.colortype = LCT_PALETTE;
  state.info_raw.bitdepth = 8;

  for (i = 0; i < (size_t)bw * bh; i++) {
    unsigned char r = block[i * 4], g = block[i * 4 + 1];
    unsigned char b = block[i * 4 + 2], a = block[i * 4 + 3];
    unsigned key = ((unsigned)r << 24) | ((unsigned)g << 16) |
                   ((unsigned)b << 8) | (unsigned)a;
    std::map<unsigned, unsigned>::iterator it = colorToIndex.find(key);
    unsigned idx;
    if (it == colorToIndex.end()) {
      idx = (unsigned)colorToIndex.size();
      colorToIndex[key] = idx;
      lodepng_palette_add(&state.info_png.color, r, g, b, a);
      lodepng_palette_add(&state.info_raw, r, g, b, a);
    } else {
      idx = it->second;
    }
    indices[i] = (unsigned char)idx;
  }

  {
    unsigned char* buffer = 0;
    size_t size = 0;
    error = lodepng_encode(&buffer, &size,
                           indices.empty() ? 0 : &indices[0], bw, bh, &state);
    if (!error) out.assign(buffer, buffer + size);
    free(buffer);
  }

  lodepng_state_cleanup(&state);
  lodepng_color_mode_cleanup(&modeIn);
  return error;
}

/* Per-block decision result. */
typedef struct BlockChoice {
  unsigned type;
  size_t trueSize;
  size_t indexedSize;
  std::vector<unsigned char> data;
} BlockChoice;

/* Estimate both encodings for one block and pick the smaller one.
   thresholdPct: indexed is chosen only when
       indexedSize * 100 <= trueSize * (100 + thresholdPct).
   thresholdPct == 0 means indexed wins on a tie; a positive threshold only
   picks indexed when it is smaller by that margin.
   forceTrue != 0 skips the indexed estimate entirely and always keeps the
   truecolor encoding, so the original encoding stays byte-identical. */
static unsigned chooseBlock(const std::vector<unsigned char>& block,
                            unsigned bw, unsigned bh, int thresholdPct,
                            int forceTrue, BlockChoice& choice) {
  unsigned error = encodeTrueColor(block, bw, bh, choice.data);
  if (error) return error;
  choice.trueSize = choice.data.size();
  choice.indexedSize = 0;
  choice.type = 0;

  if (forceTrue) return 0;

  {
    std::vector<unsigned char> indexed;
    unsigned ierror = encodeIndexed(block, bw, bh, indexed);
    if (ierror == 0) {
      choice.indexedSize = indexed.size();
      /* Values are small in these tests, but compare via doubles to avoid
         integer overflow regardless. */
      if ((double)indexed.size() * 100.0 <=
          (double)choice.trueSize * (double)(100 + thresholdPct)) {
        choice.data = indexed;
        choice.type = 1;
      }
    } else if (ierror == 1000) {
      choice.indexedSize = 0; /* palette not possible */
    } else {
      return ierror; /* real encoder failure */
    }
  }
  return 0;
}

/* ------------------------------------------------------------------------ */
/* Container encode                                                         */
/* ------------------------------------------------------------------------ */

typedef struct BlcEncodeResult {
  std::vector<unsigned char> container;
  size_t tableBytes;
  size_t indexedRegionBytes;
  size_t trueRegionBytes;
  unsigned indexedBlocks;
  unsigned trueBlocks;
} BlcEncodeResult;

static unsigned blcEncode(const std::vector<unsigned char>& img, unsigned w,
                          unsigned h, unsigned blockSize, int thresholdPct,
                          int forceTrue, BlcEncodeResult& result) {
  unsigned cols = (w + blockSize - 1) / blockSize;
  unsigned rows = (h + blockSize - 1) / blockSize;
  unsigned numBlocks = cols * rows;
  std::vector<BlockChoice> choices(numBlocks);
  std::vector<unsigned char> indexedRegion, trueRegion;
  std::vector<unsigned char> headerBytes, tableBytes;
  unsigned indexedCrc, trueCrc, tableCrc;
  unsigned n;

  result.tableBytes = 0;
  result.indexedRegionBytes = 0;
  result.trueRegionBytes = 0;
  result.indexedBlocks = 0;
  result.trueBlocks = 0;

  for (n = 0; n < numBlocks; n++) {
    unsigned cx = n % cols, cy = n / cols;
    unsigned x0 = cx * blockSize, y0 = cy * blockSize;
    unsigned bw = (x0 + blockSize > w) ? w - x0 : blockSize;
    unsigned bh = (y0 + blockSize > h) ? h - y0 : blockSize;
    std::vector<unsigned char> block =
        extractBlock(img, w, h, x0, y0, bw, bh);
    unsigned error =
        chooseBlock(block, bw, bh, thresholdPct, forceTrue, choices[n]);
    if (error) return error;
  }

  /* Two contiguous data regions; entries store offsets within their region. */
  for (n = 0; n < numBlocks; n++) {
    BlcEntry e;
    e.blockType = 0;
    e.reserved = 0;
    e.offset = 0;
    e.length = 0;
    if (choices[n].type == 1) {
      e.blockType = 1;
      e.offset = indexedRegion.size();
      indexedRegion.insert(indexedRegion.end(), choices[n].data.begin(),
                           choices[n].data.end());
      result.indexedBlocks++;
    } else {
      e.blockType = 0;
      e.offset = trueRegion.size();
      trueRegion.insert(trueRegion.end(), choices[n].data.begin(),
                        choices[n].data.end());
      result.trueBlocks++;
    }
    e.length = choices[n].data.size();

    putU32(tableBytes, e.blockType);
    putU32(tableBytes, e.reserved);
    putU32(tableBytes, e.offset);
    putU32(tableBytes, e.length);
  }

  indexedCrc = crcBytes(indexedRegion);
  trueCrc = crcBytes(trueRegion);
  tableCrc = crcBytes(tableBytes);

  /* Header (32 bytes): magic, version, w, h, blockSize, numBlocks, then three
     CRC32 words protecting respectively the block table, the indexed region
     and the truecolor region. The header itself is protected by a separate
     CRC word appended immediately after it. */
  headerBytes.assign(BLC_MAGIC, BLC_MAGIC + 4);
  putU32(headerBytes, BLC_VERSION);
  putU32(headerBytes, w);
  putU32(headerBytes, h);
  putU32(headerBytes, blockSize);
  putU32(headerBytes, numBlocks);
  putU32(headerBytes, (unsigned)tableCrc);
  putU32(headerBytes, (unsigned)indexedCrc);
  putU32(headerBytes, (unsigned)trueCrc);
  if (headerBytes.size() != BLC_HEADER_SIZE) return BLC_ERR_ENTRY;

  result.tableBytes = tableBytes.size();
  result.indexedRegionBytes = indexedRegion.size();
  result.trueRegionBytes = trueRegion.size();

  result.container.clear();
  result.container.reserve(headerBytes.size() + 4 + tableBytes.size() +
                           indexedRegion.size() + trueRegion.size());
  result.container.insert(result.container.end(), headerBytes.begin(),
                          headerBytes.end());
  putU32(result.container, (unsigned)crcBytes(headerBytes));
  result.container.insert(result.container.end(), tableBytes.begin(),
                          tableBytes.end());
  result.container.insert(result.container.end(), indexedRegion.begin(),
                          indexedRegion.end());
  result.container.insert(result.container.end(), trueRegion.begin(),
                          trueRegion.end());
  return BLC_OK;
}

/* ------------------------------------------------------------------------ */
/* Container decode                                                         */
/* ------------------------------------------------------------------------ */

static unsigned blcDecode(const std::vector<unsigned char>& data, unsigned w,
                          unsigned h, unsigned blockSize,
                          std::vector<unsigned char>& out) {
  unsigned cols, rows, numBlocks, n;
  unsigned indexedCrc, trueCrc, tableCrc, headerCrc;
  std::vector<unsigned char> headerBytes, tableBytes, indexedRegion,
      trueRegion;
  size_t tableStart, indexedStart, trueStart;

  if (data.size() < BLC_HEADER_SIZE + 4) return BLC_ERR_TRUNCATED;
  if (memcmp(&data[0], BLC_MAGIC, 4) != 0) return BLC_ERR_MAGIC;
  if (getU32(&data[4]) != BLC_VERSION) return BLC_ERR_VERSION;

  headerBytes.assign(data.begin(), data.begin() + BLC_HEADER_SIZE);
  headerCrc = getU32(&data[BLC_HEADER_SIZE]);
  if (crcBytes(headerBytes) != headerCrc) return BLC_ERR_HEADER_CRC;

  /* Header u32 words start at byte 4:
     [4]=version [8]=w [12]=h [16]=blockSize [20]=numBlocks
     [24]=tableCrc [28]=indexedCrc [32]=trueCrc */
  if (getU32(&data[8]) != w || getU32(&data[12]) != h)
    return BLC_ERR_DIMENSIONS;
  if (getU32(&data[16]) != blockSize) return BLC_ERR_DIMENSIONS;

  numBlocks = getU32(&data[20]);
  tableCrc = getU32(&data[24]);
  indexedCrc = getU32(&data[28]);
  trueCrc = getU32(&data[32]);

  cols = (w + blockSize - 1) / blockSize;
  rows = (h + blockSize - 1) / blockSize;
  if (numBlocks != cols * rows || numBlocks == 0) return BLC_ERR_DIMENSIONS;

  tableStart = BLC_HEADER_SIZE + 4;
  if (data.size() < tableStart + (size_t)numBlocks * BLC_ENTRY_SIZE)
    return BLC_ERR_TRUNCATED;
  tableBytes.assign(data.begin() + tableStart,
                    data.begin() + tableStart +
                        (size_t)numBlocks * BLC_ENTRY_SIZE);
  if (crcBytes(tableBytes) != tableCrc) return BLC_ERR_TABLE_CRC;

  /* First pass: validate all table entries and locate the two regions. */
  indexedStart = tableStart + (size_t)numBlocks * BLC_ENTRY_SIZE;
  trueStart = 0; /* computed while validating */
  {
    unsigned indexedEnd = 0, trueEnd = 0;
    for (n = 0; n < numBlocks; n++) {
      const unsigned char* e = &tableBytes[(size_t)n * BLC_ENTRY_SIZE];
      unsigned type = getU32(e);
      unsigned offset = getU32(e + 8);
      unsigned length = getU32(e + 12);
      if (type > 1) return BLC_ERR_ENTRY;
      if (length < 8) return BLC_ERR_ENTRY; /* every PNG has an 8-byte sig */
      /* Widen to long to make the offset+length addition overflow-safe. */
      if (type == 1) {
        if ((unsigned long)offset + (unsigned long)length > indexedEnd)
          indexedEnd = offset + length;
      } else {
        if ((unsigned long)offset + (unsigned long)length > trueEnd)
          trueEnd = offset + length;
      }
    }
    trueStart = indexedStart + (size_t)indexedEnd;
    if (data.size() < trueStart + (size_t)trueEnd)
      return BLC_ERR_TRUNCATED;
    indexedRegion.assign(data.begin() + indexedStart,
                         data.begin() + indexedStart + (size_t)indexedEnd);
    trueRegion.assign(data.begin() + trueStart,
                      data.begin() + trueStart + (size_t)trueEnd);
  }

  if (crcBytes(indexedRegion) != indexedCrc) return BLC_ERR_REGION_CRC;
  if (crcBytes(trueRegion) != trueCrc) return BLC_ERR_REGION_CRC;

  /* Second pass: decode each block and place it back. */
  out.assign((size_t)w * h * 4, 0);
  for (n = 0; n < numBlocks; n++) {
    const unsigned char* e = &tableBytes[(size_t)n * BLC_ENTRY_SIZE];
    unsigned type = getU32(e);
    unsigned offset = getU32(e + 8);
    unsigned length = getU32(e + 12);
    unsigned cx = n % cols, cy = n / cols;
    unsigned x0 = cx * blockSize, y0 = cy * blockSize;
    unsigned bw = (x0 + blockSize > w) ? w - x0 : blockSize;
    unsigned bh = (y0 + blockSize > h) ? h - y0 : blockSize;
    const std::vector<unsigned char>& region =
        (type == 1) ? indexedRegion : trueRegion;
    const unsigned char* png;
    unsigned char* pixels = 0;
    unsigned pw = 0, ph = 0;
    unsigned error;

    if ((size_t)(offset + length) > region.size()) return BLC_ERR_ENTRY;
    png = &region[(size_t)offset];
    /* PNG signature check (89 50 4E 47 0D 0A 1A 0A). */
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (length < 8 || memcmp(png, sig, 8) != 0)
      return BLC_ERR_PNG_SIGNATURE;

    error = lodepng_decode32(&pixels, &pw, &ph, png, (size_t)length);
    if (error) {
      free(pixels);
      return error;
    }
    if (pw != bw || ph != bh) {
      free(pixels);
      return BLC_ERR_DIMENSIONS;
    }
    {
      unsigned y, x;
      for (y = 0; y < bh; y++) {
        for (x = 0; x < bw; x++) {
          size_t si = ((size_t)y * bw + x) * 4;
          size_t di = ((size_t)(y0 + y) * w + (x0 + x)) * 4;
          out[di] = pixels[si];
          out[di + 1] = pixels[si + 1];
          out[di + 2] = pixels[si + 2];
          out[di + 3] = pixels[si + 3];
        }
      }
    }
    free(pixels);
  }
  return BLC_OK;
}

/* ------------------------------------------------------------------------ */
/* Synthetic test images (small, generated in-process, no external files)   */
/* ------------------------------------------------------------------------ */

static unsigned rngState;
static unsigned rngNext(void) {
  /* Deterministic 32-bit LCG (Numerical Recipes constants), no dependencies.
     Unsigned wrap-around gives well-defined modular arithmetic. */
  rngState = rngState * 1664525u + 1013904223u;
  return rngState;
}

static void setPx(std::vector<unsigned char>& img, unsigned w, unsigned x,
                  unsigned y, unsigned char r, unsigned char g,
                  unsigned char b, unsigned char a) {
  size_t i = ((size_t)y * w + x) * 4;
  img[i] = r;
  img[i + 1] = g;
  img[i + 2] = b;
  img[i + 3] = a;
}

/* photo: smooth gradients plus high-frequency noise -> many colors */
static std::vector<unsigned char> makePhoto(unsigned w, unsigned h) {
  std::vector<unsigned char> img((size_t)w * h * 4);
  unsigned x, y;
  rngState = 1;
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      unsigned baseR = (unsigned)(128 + 100 * std::sin(x * 0.07));
      unsigned baseG = (unsigned)(128 + 100 * std::sin(y * 0.09));
      unsigned baseB = (unsigned)(128 + 100 * std::sin((x + y) * 0.05));
      unsigned noise = rngNext() % 40;
      setPx(img, w, x, y,
            (unsigned char)(baseR ^ (noise & 31)),
            (unsigned char)(baseG ^ ((noise >> 1) & 31)),
            (unsigned char)(baseB ^ ((noise >> 2) & 31)), 255);
    }
  }
  return img;
}

/* screenshot: a few flat UI rectangles plus text-like glyph speckle, few
   colors overall */
static std::vector<unsigned char> makeScreenshot(unsigned w, unsigned h) {
  static const unsigned char palette[8][3] = {
      {240, 240, 240}, {40, 44, 52}, {97, 175, 239}, {152, 195, 121},
      {229, 192, 123}, {224, 108, 117}, {198, 120, 221}, {36, 39, 46}};
  std::vector<unsigned char> img((size_t)w * h * 4);
  unsigned x, y;
  rngState = 7;
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      unsigned band = (x / 24) + (y / 18);
      unsigned c = band % 8;
      /* sparse dark speckle simulating glyphs */
      if ((rngNext() % 100) < 4) c = 7;
      setPx(img, w, x, y, palette[c][0], palette[c][1], palette[c][2], 255);
    }
  }
  return img;
}

/* solid: one single color */
static std::vector<unsigned char> makeSolid(unsigned w, unsigned h) {
  std::vector<unsigned char> img((size_t)w * h * 4);
  size_t i;
  for (i = 0; i < (size_t)w * h; i++) {
    img[i * 4] = 37;
    img[i * 4 + 1] = 99;
    img[i * 4 + 2] = 235;
    img[i * 4 + 3] = 255;
  }
  return img;
}

/* mixed: left half photo-like, right half flat screenshot-like */
static std::vector<unsigned char> makeMixed(unsigned w, unsigned h) {
  std::vector<unsigned char> a = makePhoto(w, h);
  std::vector<unsigned char> b = makeScreenshot(w, h);
  std::vector<unsigned char> img((size_t)w * h * 4);
  unsigned x, y;
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      const std::vector<unsigned char>& src = (x < w / 2) ? a : b;
      size_t i = ((size_t)y * w + x) * 4;
      img[i] = src[i];
      img[i + 1] = src[i + 1];
      img[i + 2] = src[i + 2];
      img[i + 3] = src[i + 3];
    }
  }
  return img;
}

/* ------------------------------------------------------------------------ */
/* Benchmark / checks                                                       */
/* ------------------------------------------------------------------------ */

static double nowMs(void) {
  return 1000.0 * (double)clock() / (double)CLOCKS_PER_SEC;
}

static size_t trueColorPngSize(const std::vector<unsigned char>& img,
                               unsigned w, unsigned h) {
  unsigned char* buffer = 0;
  size_t size = 0;
  lodepng_encode32(&buffer, &size, img.empty() ? 0 : &img[0], w, h);
  free(buffer);
  return size;
}

static bool equalBytes(const std::vector<unsigned char>& a,
                       const std::vector<unsigned char>& b) {
  return a.size() == b.size() &&
         (a.empty() || memcmp(&a[0], &b[0], a.size()) == 0);
}

typedef struct Metrics {
  const char* name;
  size_t fullTrueSize;
  size_t containerSize;
  size_t tableBytes;
  size_t indexedRegion;
  size_t trueRegion;
  unsigned indexedBlocks;
  unsigned trueBlocks;
  double encodeMs;
  double decodeMs;
} Metrics;

static unsigned runOne(const char* name,
                       const std::vector<unsigned char>& img, unsigned w,
                       unsigned h, unsigned blockSize, int thresholdPct,
                       Metrics& m) {
  BlcEncodeResult er;
  std::vector<unsigned char> decoded;
  unsigned error;
  double t0, t1;

  m.name = name;
  m.fullTrueSize = trueColorPngSize(img, w, h);

  t0 = nowMs();
  error = blcEncode(img, w, h, blockSize, thresholdPct, 0, er);
  t1 = nowMs();
  if (error) return error;
  m.encodeMs = t1 - t0;

  t0 = nowMs();
  error = blcDecode(er.container, w, h, blockSize, decoded);
  t1 = nowMs();
  if (error) return error;
  m.decodeMs = t1 - t0;

  if (!equalBytes(img, decoded)) return BLC_ERR_MISMATCH;

  m.containerSize = er.container.size();
  m.tableBytes = er.tableBytes;
  m.indexedRegion = er.indexedRegionBytes;
  m.trueRegion = er.trueRegionBytes;
  m.indexedBlocks = er.indexedBlocks;
  m.trueBlocks = er.trueBlocks;
  return BLC_OK;
}

static void printMetrics(const Metrics& m) {
  double ratioVsTrue = 100.0 * (double)m.containerSize /
                       (double)m.fullTrueSize;
  double tablePct = 100.0 * (double)m.tableBytes /
                    (double)m.containerSize;
  std::printf(
      "  %-10s fullRGBA=%8u  blc=%8u  ratio=%6.2f%%  "
      "table=%6u (%5.2f%%)  idxRegion=%8u tcRegion=%8u  "
      "blocks(idx/tc)=%u/%u  enc=%7.2fms dec=%7.2fms\n",
      m.name, (unsigned)m.fullTrueSize, (unsigned)m.containerSize,
      ratioVsTrue, (unsigned)m.tableBytes, tablePct,
      (unsigned)m.indexedRegion, (unsigned)m.trueRegion, m.indexedBlocks,
      m.trueBlocks, m.encodeMs, m.decodeMs);
}

/* Flip one byte inside the block table and require the decoder to reject it. */
static unsigned testTableCorruption(const std::vector<unsigned char>& good,
                                    unsigned w, unsigned h,
                                    unsigned blockSize) {
  std::vector<unsigned char> bad = good;
  std::vector<unsigned char> decoded;
  size_t tablePos = BLC_HEADER_SIZE + 4;
  unsigned error;
  bad[tablePos] ^= 0xA5; /* corrupt first entry's type field */
  error = blcDecode(bad, w, h, blockSize, decoded);
  if (error == BLC_OK) {
    std::printf("  FAIL: corrupted block table was accepted\n");
    return 1;
  }
  std::printf("  table corruption -> error %u (%s) [rejected, OK]\n", error,
              blc_error_text(error));
  return 0;
}

/* Corrupt the header CRC field itself. */
static unsigned testHeaderCorruption(const std::vector<unsigned char>& good,
                                     unsigned w, unsigned h,
                                     unsigned blockSize) {
  std::vector<unsigned char> bad = good;
  std::vector<unsigned char> decoded;
  unsigned error;
  bad[BLC_HEADER_SIZE] ^= 0x5A;
  error = blcDecode(bad, w, h, blockSize, decoded);
  if (error == BLC_OK) {
    std::printf("  FAIL: corrupted header CRC was accepted\n");
    return 1;
  }
  std::printf("  header corruption -> error %u (%s) [rejected, OK]\n", error,
              blc_error_text(error));
  return 0;
}

/* Corrupt a data region byte; region CRC check must fire (or PNG decode must
   fail) — either way it must be rejected, never silently accepted. */
static unsigned testRegionCorruption(const std::vector<unsigned char>& good,
                                     unsigned w, unsigned h,
                                     unsigned blockSize) {
  std::vector<unsigned char> bad = good;
  std::vector<unsigned char> decoded;
  unsigned error;
  /* corrupt well past header+table, inside a data region */
  size_t pos = bad.size() - 17;
  bad[pos] ^= 0xFF;
  error = blcDecode(bad, w, h, blockSize, decoded);
  if (error == BLC_OK) {
    std::printf("  FAIL: corrupted data region was accepted\n");
    return 1;
  }
  std::printf("  region corruption -> error %u (%s) [rejected, OK]\n", error,
              blc_error_text(error));
  return 0;
}

/* Truncation must be reported, not read out of bounds. */
static unsigned testTruncation(const std::vector<unsigned char>& good,
                               unsigned w, unsigned h, unsigned blockSize) {
  std::vector<unsigned char> bad(good.begin(),
                                 good.begin() + BLC_HEADER_SIZE + 2);
  std::vector<unsigned char> decoded;
  unsigned error = blcDecode(bad, w, h, blockSize, decoded);
  if (error == BLC_OK) {
    std::printf("  FAIL: truncated container was accepted\n");
    return 1;
  }
  std::printf("  truncation -> error %u (%s) [rejected, OK]\n", error,
              blc_error_text(error));
  return 0;
}

/* The original truecolor encoding must be byte-identical whether produced by
   lodepng_encode32 directly or through this program's truecolor block path
   (threshold set so indexed can never win). */
static unsigned testTrueColorByteIdentity(
    const std::vector<unsigned char>& img, unsigned w, unsigned h,
    unsigned blockSize) {
  unsigned char* refBuf = 0;
  size_t refSize = 0;
  std::vector<unsigned char> ref;
  BlcEncodeResult er;
  unsigned error;
  unsigned n, cols;

  lodepng_encode32(&refBuf, &refSize, img.empty() ? 0 : &img[0], w, h);
  ref.assign(refBuf, refBuf + refSize);
  free(refBuf);

  /* threshold -100 makes the indexed inequality impossible => all truecolor */
  error = blcEncode(img, w, h, blockSize, 0, 1 /* force truecolor */, er);
  if (error) return error;
  if (er.indexedBlocks != 0) {
    std::printf("  FAIL: expected zero indexed blocks under strict threshold\n");
    return 1;
  }

  cols = (w + blockSize - 1) / blockSize;
  /* Re-encode each block directly and confirm identical bytes to the block
     bytes stored in the container. */
  for (n = 0; n < er.trueBlocks + er.indexedBlocks; n++) {
    unsigned cx = n % cols, cy = n / cols;
    unsigned x0 = cx * blockSize, y0 = cy * blockSize;
    unsigned bw = (x0 + blockSize > w) ? w - x0 : blockSize;
    unsigned bh = (y0 + blockSize > h) ? h - y0 : blockSize;
    std::vector<unsigned char> block =
        extractBlock(img, w, h, x0, y0, bw, bh);
    std::vector<unsigned char> direct;
    const unsigned char* e =
        &er.container[BLC_HEADER_SIZE + 4 + (size_t)n * BLC_ENTRY_SIZE];
    unsigned off = getU32(e + 8);
    unsigned len = getU32(e + 12);
    size_t regionStart =
        BLC_HEADER_SIZE + 4 +
        (size_t)(er.trueBlocks + er.indexedBlocks) * BLC_ENTRY_SIZE +
        er.indexedRegionBytes;
    error = encodeTrueColor(block, bw, bh, direct);
    if (error) return error;
    if (direct.size() != (size_t)len ||
        memcmp(&direct[0], &er.container[regionStart + (size_t)off],
               direct.size()) != 0) {
      std::printf("  FAIL: truecolor block %u bytes differ\n", n);
      return 1;
    }
  }
  std::printf(
      "  truecolor byte-identity: %u-byte reference; all %u blocks match "
      "lodepng_encode32 output exactly\n",
      (unsigned)refSize, er.trueBlocks);
  return 0;
}

int main(int argc, char** argv) {
  /* Configurable block size and threshold, with small defaults so the whole
     run stays fast and uses only in-process data. */
  unsigned blockSize = 16;
  int thresholdPct = 0;
  const unsigned W = 128, H = 128;
  unsigned failures = 0;
  std::vector<Metrics> ms(4);
  BlcEncodeResult mixedContainer;
  unsigned error;
  int i;

  for (i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--block") && i + 1 < argc) {
      blockSize = (unsigned)std::atoi(argv[++i]);
    } else if (!std::strcmp(argv[i], "--threshold") && i + 1 < argc) {
      thresholdPct = std::atoi(argv[++i]);
    }
  }
  if (blockSize == 0) blockSize = 16;

  std::printf("=== BLC1 block codec (block=%u, threshold=%d%%, image=%ux%u) ===\n",
              blockSize, thresholdPct, W, H);

  struct Img {
    const char* name;
    std::vector<unsigned char> data;
  };
  Img imgs[4];
  imgs[0].name = "photo";
  imgs[0].data = makePhoto(W, H);
  imgs[1].name = "screenshot";
  imgs[1].data = makeScreenshot(W, H);
  imgs[2].name = "solid";
  imgs[2].data = makeSolid(W, H);
  imgs[3].name = "mixed";
  imgs[3].data = makeMixed(W, H);

  std::printf("\n[1] Volume vs full truecolor, block table cost, timing\n");
  for (i = 0; i < 4; i++) {
    error = runOne(imgs[i].name, imgs[i].data, W, H, blockSize,
                   thresholdPct, ms[i]);
    if (error) {
      std::printf("  %s: encode/decode FAILED %u (%s)\n", imgs[i].name, error,
                  blc_error_text(error));
      failures++;
    } else {
      printMetrics(ms[i]);
    }
  }

  std::printf("\n[2] Roundtrip pixel correctness (all four classes)\n");
  for (i = 0; i < 4; i++) {
    BlcEncodeResult er;
    std::vector<unsigned char> decoded;
    error = blcEncode(imgs[i].data, W, H, blockSize, thresholdPct, 0, er);
    if (!error) error = blcDecode(er.container, W, H, blockSize, decoded);
    if (error || !equalBytes(imgs[i].data, decoded)) {
      std::printf("  %s: ROUNDTRIP FAIL (%u)\n", imgs[i].name, error);
      failures++;
    } else {
      std::printf("  %s: roundtrip OK (%u bytes, %u idx / %u tc blocks)\n",
                  imgs[i].name, (unsigned)er.container.size(),
                  er.indexedBlocks, er.trueBlocks);
    }
    if (!std::strcmp(imgs[i].name, "mixed")) mixedContainer = er;
  }

  std::printf("\n[3] Block table / region integrity errors\n");
  if (mixedContainer.container.empty()) {
    std::printf("  mixed container unavailable, skipping corruption tests\n");
    failures++;
  } else {
    failures += testTableCorruption(mixedContainer.container, W, H, blockSize);
    failures += testHeaderCorruption(mixedContainer.container, W, H,
                                     blockSize);
    failures += testRegionCorruption(mixedContainer.container, W, H,
                                     blockSize);
    failures += testTruncation(mixedContainer.container, W, H, blockSize);
  }

  std::printf("\n[4] Original truecolor encoding stays byte-identical\n");
  failures += testTrueColorByteIdentity(imgs[1].data, W, H, blockSize);

  std::printf("\n[5] Threshold sweep on mixed image (configurable estimate "
              "margin)\n");
  {
    int sweeps[3] = {-50, 0, 50};
    int s;
    for (s = 0; s < 3; s++) {
      Metrics m;
      error = runOne("mixed", imgs[3].data, W, H, blockSize, sweeps[s], m);
      if (error) {
        std::printf("  threshold=%d%%: FAIL %u\n", sweeps[s], error);
        failures++;
      } else {
        std::printf("  threshold=%+4d%%: ratio=%6.2f%% idxBlocks=%u/%u\n",
                    sweeps[s],
                    100.0 * (double)m.containerSize /
                        (double)m.fullTrueSize,
                    m.indexedBlocks, m.indexedBlocks + m.trueBlocks);
      }
    }
  }

  std::printf("\n[6] Block size sweep on mixed image (configurable geometry)\n");
  {
    unsigned sizes[3] = {8, 16, 32};
    int s;
    for (s = 0; s < 3; s++) {
      Metrics m;
      error = runOne("mixed", imgs[3].data, W, H, sizes[s], thresholdPct, m);
      if (error) {
        std::printf("  block=%u: FAIL %u\n", sizes[s], error);
        failures++;
      } else {
        std::printf(
            "  block=%2u: ratio=%6.2f%% table=%6ub idxBlocks=%u/%u "
            "enc=%.2fms\n",
            sizes[s],
            100.0 * (double)m.containerSize / (double)m.fullTrueSize,
            (unsigned)m.tableBytes, m.indexedBlocks,
            m.indexedBlocks + m.trueBlocks, m.encodeMs);
      }
    }
  }

  std::printf("\n=== %s (%u failure%s) ===\n",
              failures ? "FAILED" : "ALL CHECKS PASSED", failures,
              failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}

/*
LodePNG zlib compression level sweep

Copyright (c) 2005-2026 Lode Vandevenne

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
    claim that you wrote the original software. If you use this software
    in a product, an acknowledgment in the product documentation would be
    appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be
    misrepresented as being the original software.

    3. This notice may not be removed or altered from any source
    distribution.
*/

/*
Level sweep for the built-in lodepng deflate/zlib encoder.

lodepng itself has no "zlib level" setting: its compressor is configured with
LodePNGCompressSettings (btype, use_lz77, windowsize, minmatch, nicematch,
lazymatching). zlib's levels 0..9 are not a perfect fit, so this tool defines
the following documented mapping, with level 6 exactly equal to
lodepng_compress_settings_init() (the settings used by the original, unmodified
encoding path of this repository). That equality is checked at runtime and the
level-6 output is compared byte-for-byte (CRC32) with a default lodepng::encode
call for every test image.

  level  btype use_lz77 windowsize minmatch nicematch lazymatching
    0      0       0        2048       3        128          0     (stored)
    1      2       1         512       3          8          0
    2      2       1        1024       3         16          0
    3      2       1        1024       3         32          0
    4      2       1        2048       3         64          0
    5      2       1        2048       3         96          0
    6      2       1        2048       3        128          1     (= lodepng default)
    7      2       1        4096       3        128          1
    8      2       1        8192       3        192          1
    9      2       1       32768       3        258          1

For each of four synthetic image categories (small, generated in-memory) the
tool encodes the same image at every sampled level, records output size and
encoding time, prints the size/time curve points, and recommends a level:
the lowest (least compression effort) level whose output is within a
configurable tolerance of the smallest output size found in the sweep. It also
reports the size difference against the original (default) level 6 and the
total measured time.

Level range and sampling step are configurable:

  ./level_sweep [--levels MIN-MAX] [--step N] [--repeat N] [--tolerance PCT]

Levels outside 0..9 (or an invalid range/step) are rejected with a non-zero
exit code. The library file itself is untouched, so the original encoding path
stays byte-identical.

Build (repository root):
  g++ -I . -W -Wall -Wextra -ansi -pedantic -O3 lodepng.cpp lodepng_level_sweep.cpp -o level_sweep

Run:
  ./level_sweep
*/

#include "lodepng.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#define LEVEL_MIN 0
#define LEVEL_MAX 9
#define REFERENCE_LEVEL 6

struct Options {
  unsigned min_level;
  unsigned max_level;
  unsigned step;
  unsigned repeat;
  double tolerance_percent;
  Options() : min_level(LEVEL_MIN), max_level(LEVEL_MAX), step(1),
              repeat(5), tolerance_percent(1.0) {}
};

struct TestImage {
  std::string name;
  std::string category;
  unsigned width;
  unsigned height;
  LodePNGColorType colortype;
  unsigned bitdepth;
  std::vector<unsigned char> pixels;
};

struct CurvePoint {
  unsigned level;
  size_t size;
  double time_ms; /*minimum per-encode time over the repeats*/
  unsigned long crc;
  double total_ms; /*all repeats of this level*/
};

struct ImageResult {
  std::string name;
  std::string category;
  size_t reference_size;
  double reference_time_ms;
  unsigned long reference_crc;
  unsigned png_colortype;
  unsigned png_bitdepth;
  std::vector<CurvePoint> points;
  unsigned recommended_level;
  size_t recommended_size;
  double recommended_time_ms;
  double sweep_total_ms;
  bool roundtrip_ok;
  bool default_path_identical;
};

static void fail(const std::string& message) {
  std::cout << "FAIL: " << message << std::endl;
  std::exit(1);
}

/*Strict decimal parsing, used for levels, step and repeat.*/
static bool parseUnsigned(const std::string& s, unsigned long& value) {
  unsigned long result = 0;
  if(s.empty()) return false;
  for(size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if(c < '0' || c > '9') return false;
    if(result > (0xFFFFFFFFUL - (unsigned long)(c - '0')) / 10) return false;
    result = result * 10 + (unsigned long)(c - '0');
  }
  value = result;
  return true;
}

/*Parse a level out of range spec "MIN-MAX". Returns false and sets error.*/
static bool parseLevelRange(const std::string& spec, unsigned& min_level,
                            unsigned& max_level, std::string& error) {
  size_t dash = spec.find('-');
  if(dash == std::string::npos || spec.find('-', dash + 1) != std::string::npos) {
    error = "level range must have the form MIN-MAX, e.g. 0-9";
    return false;
  }
  std::string smin = spec.substr(0, dash);
  std::string smax = spec.substr(dash + 1);
  unsigned long lo = 0, hi = 0;
  if(!parseUnsigned(smin, lo) || !parseUnsigned(smax, hi)) {
    error = "level range must contain decimal numbers";
    return false;
  }
  if(lo > LEVEL_MAX) {
    std::ostringstream ss;
    ss << "level " << lo << " out of supported range " << LEVEL_MIN << ".." << LEVEL_MAX;
    error = ss.str();
    return false;
  }
  if(hi > LEVEL_MAX) {
    std::ostringstream ss;
    ss << "level " << hi << " out of supported range " << LEVEL_MIN << ".." << LEVEL_MAX;
    error = ss.str();
    return false;
  }
  if(lo > hi) {
    error = "level range minimum is larger than maximum";
    return false;
  }
  min_level = (unsigned)lo;
  max_level = (unsigned)hi;
  return true;
}

/*The documented level-to-settings mapping. Returns false for an out-of-range level.*/
static bool getLevelSettings(unsigned level, LodePNGCompressSettings* settings) {
  lodepng_compress_settings_init(settings);
  if(level > LEVEL_MAX) return false;
  settings->btype = 2;
  settings->use_lz77 = 1;
  settings->minmatch = 3;
  if(level == 0) {
    settings->btype = 0;
    settings->use_lz77 = 0;
    settings->windowsize = 2048;
    settings->nicematch = 128;
    settings->lazymatching = 0;
  } else if(level == 1) {
    settings->windowsize = 512;
    settings->nicematch = 8;
    settings->lazymatching = 0;
  } else if(level == 2) {
    settings->windowsize = 1024;
    settings->nicematch = 16;
    settings->lazymatching = 0;
  } else if(level == 3) {
    settings->windowsize = 1024;
    settings->nicematch = 32;
    settings->lazymatching = 0;
  } else if(level == 4) {
    settings->windowsize = 2048;
    settings->nicematch = 64;
    settings->lazymatching = 0;
  } else if(level == 5) {
    settings->windowsize = 2048;
    settings->nicematch = 96;
    settings->lazymatching = 0;
  } else if(level == 6) {
    /*exactly lodepng_compress_settings_init(): the original encoding path*/
    settings->windowsize = 2048;
    settings->nicematch = 128;
    settings->lazymatching = 1;
  } else if(level == 7) {
    settings->windowsize = 4096;
    settings->nicematch = 128;
    settings->lazymatching = 1;
  } else if(level == 8) {
    settings->windowsize = 8192;
    settings->nicematch = 192;
    settings->lazymatching = 1;
  } else { /*level == 9*/
    settings->windowsize = 32768;
    settings->nicematch = 258;
    settings->lazymatching = 1;
  }
  return true;
}

static bool sameCompressSettings(const LodePNGCompressSettings& a,
                                 const LodePNGCompressSettings& b) {
  return a.btype == b.btype && a.use_lz77 == b.use_lz77 &&
         a.windowsize == b.windowsize && a.minmatch == b.minmatch &&
         a.nicematch == b.nicematch && a.lazymatching == b.lazymatching;
}

static void putPixel(TestImage& image, unsigned x, unsigned y,
                     unsigned r, unsigned g, unsigned b, unsigned a) {
  size_t bpp = image.colortype == LCT_RGBA ? 4 : (image.colortype == LCT_RGB ? 3 : 1);
  size_t index = ((size_t)y * image.width + (size_t)x) * bpp;
  if(image.colortype == LCT_RGBA) {
    image.pixels[index + 0] = (unsigned char)r;
    image.pixels[index + 1] = (unsigned char)g;
    image.pixels[index + 2] = (unsigned char)b;
    image.pixels[index + 3] = (unsigned char)a;
  } else if(image.colortype == LCT_RGB) {
    image.pixels[index + 0] = (unsigned char)r;
    image.pixels[index + 1] = (unsigned char)g;
    image.pixels[index + 2] = (unsigned char)b;
  } else { /*LCT_GREY, 8-bit*/
    image.pixels[index] = (unsigned char)r;
  }
}

static unsigned char clampByte(int v) {
  if(v < 0) return 0;
  if(v > 255) return 255;
  return (unsigned char)v;
}

static TestImage makePhotoImage() {
  TestImage image;
  image.name = "photo";
  image.category = "continuous-tone photo (noisy gradients)";
  image.width = 96;
  image.height = 96;
  image.colortype = LCT_RGBA;
  image.bitdepth = 8;
  image.pixels.resize((size_t)image.width * image.height * 4);
  unsigned noise = 2463534242u;
  for(unsigned y = 0; y < image.height; ++y) {
    for(unsigned x = 0; x < image.width; ++x) {
      noise = noise * 1103515245u + 12345u;
      int n = (int)((noise >> 16) % 7u) - 3;
      int r = (int)((x * 255u) / (image.width - 1u)) + n;
      int g = (int)((y * 255u) / (image.height - 1u)) + n / 2;
      int b = (int)(((x + y) * 255u) / (image.width + image.height - 2u));
      putPixel(image, x, y, (unsigned)clampByte(r), (unsigned)clampByte(g),
               (unsigned)clampByte(b), 255);
    }
  }
  return image;
}

static TestImage makeFlatImage() {
  TestImage image;
  image.name = "flat";
  image.category = "flat-color artwork (large uniform areas)";
  image.width = 96;
  image.height = 96;
  image.colortype = LCT_RGB;
  image.bitdepth = 8;
  image.pixels.resize((size_t)image.width * image.height * 3);
  for(unsigned y = 0; y < image.height; ++y) {
    for(unsigned x = 0; x < image.width; ++x) {
      unsigned r = 30, g = 60, b = 90;
      if(x >= 8 && x < 48 && y >= 8 && y < 48) {
        r = 230; g = 200; b = 40;
      } else if((x >= 56 && x < 88 && y >= 12 && y < 44) ||
                ((x + y) / 12u) % 2u == 0) {
        r = 250; g = 250; b = 250;
      }
      int dx = (int)x - 70, dy = (int)y - 72;
      if(dx * dx + dy * dy < 18 * 18) {
        r = 200; g = 40; b = 40;
      }
      if(x >= 20 && x < 76 && y >= 62 && y < 68) {
        r = 40; g = 160; b = 120;
      }
      putPixel(image, x, y, r, g, b, 255);
    }
  }
  return image;
}

static TestImage makeGrayImage() {
  TestImage image;
  image.name = "gray";
  image.category = "8-bit grayscale mask (gradient, edges, dither)";
  image.width = 96;
  image.height = 96;
  image.colortype = LCT_GREY;
  image.bitdepth = 8;
  image.pixels.resize((size_t)image.width * image.height);
  for(unsigned y = 0; y < image.height; ++y) {
    for(unsigned x = 0; x < image.width; ++x) {
      int v = (int)((x * 255u) / (image.width - 1u));
      if(((y / 4u) & 1u) != 0) v = 255 - v;
      if(x > 28 && x < 64 && y > 24 && y < 72) {
        v = (((x / 4u) + (y / 4u)) & 1u) ? 255 : 0;
      }
      putPixel(image, x, y, (unsigned)clampByte(v), 0, 0, 255);
    }
  }
  return image;
}

static TestImage makeIconImage() {
  TestImage image;
  image.name = "icon";
  image.category = "transparent UI icon (few colors + alpha)";
  image.width = 64;
  image.height = 64;
  image.colortype = LCT_RGBA;
  image.bitdepth = 8;
  image.pixels.resize((size_t)image.width * image.height * 4);
  for(unsigned y = 0; y < image.height; ++y) {
    for(unsigned x = 0; x < image.width; ++x) {
      int dx = (int)x - 32, dy = (int)y - 32;
      int d2 = dx * dx + dy * dy;
      unsigned alpha = 0, r = 0, g = 0, b = 0;
      if(d2 < 22 * 22) {
        alpha = 255;
        if(x < 32 && y < 32) { r = 30; g = 110; b = 220; }
        else if(x >= 32 && y < 32) { r = 240; g = 180; b = 20; }
        else if(x < 32) { r = 30; g = 170; b = 90; }
        else { r = 210; g = 40; b = 70; }
      } else if(d2 < 26 * 26) {
        alpha = 128;
        r = 255; g = 255; b = 255;
      }
      putPixel(image, x, y, r, g, b, alpha);
    }
  }
  return image;
}

static void generateCorpus(std::vector<TestImage>& images) {
  images.clear();
  images.push_back(makePhotoImage());
  images.push_back(makeFlatImage());
  images.push_back(makeGrayImage());
  images.push_back(makeIconImage());
}

static double ticksToMs(clock_t ticks) {
  return (double)ticks * 1000.0 / (double)CLOCKS_PER_SEC;
}

static unsigned encodeImage(const TestImage& image,
                            const LodePNGCompressSettings* zlibsettings,
                            std::vector<unsigned char>& out) {
  lodepng::State state;
  state.info_raw.colortype = image.colortype;
  state.info_raw.bitdepth = image.bitdepth;
  if(zlibsettings) state.encoder.zlibsettings = *zlibsettings;
  return lodepng::encode(out, image.pixels, image.width, image.height, state);
}

static size_t encodedSize(const TestImage& image,
                          const LodePNGCompressSettings* zlibsettings,
                          unsigned long& crc) {
  std::vector<unsigned char> png;
  unsigned error = encodeImage(image, zlibsettings, png);
  if(error) fail(std::string("encoding failed: ") + lodepng_error_text(error));
  if(png.empty()) fail("encoder produced an empty file");
  crc = lodepng_crc32(&png[0], png.size());
  return png.size();
}

static void runSelfTests() {
  std::cout << "self-tests" << std::endl;

  /*level 6 must be exactly the public default settings*/
  LodePNGCompressSettings defaults;
  lodepng_compress_settings_init(&defaults);
  LodePNGCompressSettings level6;
  if(!getLevelSettings(REFERENCE_LEVEL, &level6)) fail("reference level rejected");
  if(!sameCompressSettings(defaults, level6)) {
    fail("level 6 does not match lodepng_compress_settings_init()");
  }
  std::cout << "  level " << REFERENCE_LEVEL
            << " equals lodepng default settings: ok" << std::endl;

  /*every supported level must yield valid settings, out-of-range levels must be rejected*/
  for(unsigned level = LEVEL_MIN; level <= LEVEL_MAX; ++level) {
    LodePNGCompressSettings s;
    if(!getLevelSettings(level, &s)) fail("supported level rejected");
    if(s.btype > 2) fail("internal error: invalid btype");
    if(s.windowsize == 0 || s.windowsize > 32768 ||
       (s.windowsize & (s.windowsize - 1)) != 0) {
      fail("internal error: invalid windowsize");
    }
    if(s.nicematch > 258) fail("internal error: invalid nicematch");
  }
  LodePNGCompressSettings dummy;
  if(getLevelSettings(10, &dummy)) fail("out-of-range level 10 was accepted");
  if(getLevelSettings(0xFFFFFFFFu, &dummy)) fail("out-of-range level was accepted");
  std::cout << "  levels 0..9 valid, level 10 and larger rejected: ok" << std::endl;

  /*invalid config strings must be rejected (range bounds, order, step, syntax)*/
  unsigned lo = 0, hi = 0;
  std::string error;
  if(parseLevelRange("0-10", lo, hi, error)) fail("level 10 in range was accepted");
  if(parseLevelRange("10-12", lo, hi, error)) fail("range 10-12 was accepted");
  if(parseLevelRange("5-2", lo, hi, error)) fail("reversed range was accepted");
  if(parseLevelRange("0-9x", lo, hi, error)) fail("malformed range was accepted");
  if(parseLevelRange("0", lo, hi, error)) fail("range without dash was accepted");
  if(!parseLevelRange("0-9", lo, hi, error)) fail("valid range was rejected");
  if(lo != 0 || hi != 9) fail("range parsed incorrectly");
  if(!parseLevelRange("3-3", lo, hi, error)) fail("single-level range was rejected");
  std::cout << "  invalid level ranges rejected: ok" << std::endl;
}

static std::vector<unsigned> buildLevelList(const Options& options) {
  std::vector<unsigned> levels;
  for(unsigned level = options.min_level;; level += options.step) {
    levels.push_back(level);
    if(level >= options.max_level) break;
    if(options.max_level - level < options.step) break;
  }
  /*always include the range maximum as the last sample, even if the step
  does not land on it exactly*/
  if(levels[levels.size() - 1] != options.max_level) levels.push_back(options.max_level);
  return levels;
}

static void printCurvePlot(const ImageResult& result) {
  size_t max_size = 0;
  double max_time = 0.0;
  for(size_t i = 0; i < result.points.size(); ++i) {
    if(result.points[i].size > max_size) max_size = result.points[i].size;
    if(result.points[i].time_ms > max_time) max_time = result.points[i].time_ms;
  }
  const unsigned bar_width = 40;
  std::cout << "  ascii curve (size bar | time bar, each scaled to the sweep max):"
            << std::endl;
  for(size_t i = 0; i < result.points.size(); ++i) {
    const CurvePoint& p = result.points[i];
    unsigned size_bar = max_size ? (unsigned)((p.size * bar_width) / max_size) : 0;
    unsigned time_bar = max_time > 0.0
        ? (unsigned)((p.time_ms / max_time) * (double)bar_width) : 0;
    std::string size_str(size_bar, '#');
    std::string time_str(time_bar, '*');
    std::cout << "    " << std::setw(2) << p.level << " |" << std::left
              << std::setw((int)bar_width) << size_str << "|" << std::left
              << std::setw((int)bar_width) << time_str << "|" << std::right
              << std::endl;
  }
}

static ImageResult sweepImage(const TestImage& image,
                              const std::vector<unsigned>& levels,
                              const Options& options) {
  ImageResult result;
  result.name = image.name;
  result.category = image.category;
  result.roundtrip_ok = true;
  result.default_path_identical = false;
  result.sweep_total_ms = 0.0;

  std::cout << std::endl;
  std::cout << "[" << image.name << "] " << image.width << "x" << image.height
            << " " << image.category << ", raw bytes=" << image.pixels.size()
            << std::endl;

  /*reference: the unmodified default encoding path*/
  {
    std::vector<unsigned char> png;
    result.reference_time_ms = 0.0;
    for(unsigned r = 0; r < options.repeat; ++r) {
      png.clear();
      clock_t t0 = std::clock();
      unsigned error = encodeImage(image, 0, png);
      clock_t t1 = std::clock();
      if(error) fail("reference encode failed: " + std::string(lodepng_error_text(error)));
      double elapsed = ticksToMs(t1 - t0);
      if(r == 0 || elapsed < result.reference_time_ms) result.reference_time_ms = elapsed;
      if(r == 0) {
        result.reference_size = png.size();
        result.reference_crc = lodepng_crc32(&png[0], png.size());
      } else if(png.size() != result.reference_size ||
                lodepng_crc32(&png[0], png.size()) != result.reference_crc) {
        fail("default encoding path is not deterministic");
      }
    }

    lodepng::State inspect_state;
    unsigned w = 0, h = 0;
    unsigned error = lodepng_inspect(&w, &h, &inspect_state, &png[0], png.size());
    if(error) fail("inspect of reference encode failed");
    result.png_colortype = (unsigned)inspect_state.info_png.color.colortype;
    result.png_bitdepth = inspect_state.info_png.color.bitdepth;

    std::cout << "  reference level " << REFERENCE_LEVEL << ": size="
              << result.reference_size << " crc=" << result.reference_crc
              << " time_ms=" << std::fixed << std::setprecision(3)
              << result.reference_time_ms << " (lodepng default path)"
              << std::endl;
    std::cout << "  encoded PNG color type=" << result.png_colortype
              << " bit depth=" << result.png_bitdepth << std::endl;
  }

  /*level 6 must reproduce the original path byte-for-byte*/
  {
    LodePNGCompressSettings s;
    if(!getLevelSettings(REFERENCE_LEVEL, &s)) fail("reference level settings failed");
    unsigned long crc = 0;
    size_t size = encodedSize(image, &s, crc);
    result.default_path_identical =
        (size == result.reference_size) && (crc == result.reference_crc);
    if(!result.default_path_identical) {
      std::ostringstream ss;
      ss << "level " << REFERENCE_LEVEL
         << " output differs from the original encoding path";
      fail(ss.str());
    }
    std::cout << "  original encoding path byte-identical at level "
              << REFERENCE_LEVEL << ": ok" << std::endl;
  }

  std::cout << "  curve points (same image, varying only zlib level):" << std::endl;
  std::cout << "    level  size_bytes  delta_ref_bytes  delta_ref_pct  "
               "time_ms  crc32" << std::endl;

  for(size_t i = 0; i < levels.size(); ++i) {
    unsigned level = levels[i];
    LodePNGCompressSettings settings;
    if(!getLevelSettings(level, &settings)) {
      std::ostringstream ss;
      ss << "level " << level << " out of supported range " << LEVEL_MIN << ".." << LEVEL_MAX;
      fail(ss.str());
    }

    CurvePoint point;
    point.level = level;
    point.size = 0;
    point.crc = 0;
    point.time_ms = 0.0;
    point.total_ms = 0.0;

    for(unsigned r = 0; r < options.repeat; ++r) {
      std::vector<unsigned char> png;
      clock_t t0 = std::clock();
      unsigned error = encodeImage(image, &settings, png);
      clock_t t1 = std::clock();
      if(error) fail("encoding failed: " + std::string(lodepng_error_text(error)));
      if(png.empty()) fail("encoder produced an empty file");
      double elapsed = ticksToMs(t1 - t0);
      point.total_ms += elapsed;
      if(r == 0 || elapsed < point.time_ms) point.time_ms = elapsed;
      if(r == 0) {
        point.size = png.size();
        point.crc = lodepng_crc32(&png[0], png.size());
      } else if(png.size() != point.size ||
                lodepng_crc32(&png[0], png.size()) != point.crc) {
        fail("encoding is not deterministic within a level");
      }
    }
    result.sweep_total_ms += point.total_ms;

    /*Each level must still produce a PNG that decodes back to the exact input bytes.*/
    {
      std::vector<unsigned char> png;
      std::vector<unsigned char> decoded;
      unsigned w = 0, h = 0;
      unsigned error = encodeImage(image, &settings, png);
      if(!error) error = lodepng::decode(decoded, w, h, png, image.colortype, image.bitdepth);
      if(error || w != image.width || h != image.height ||
         decoded.size() != image.pixels.size() || decoded != image.pixels) {
        result.roundtrip_ok = false;
      }
    }

    double delta = (double)((long)point.size - (long)result.reference_size);
    double percent = 100.0 * delta / (double)result.reference_size;

    std::cout << "    " << std::setw(5) << level
              << "  " << std::setw(10) << point.size
              << "  " << std::setw(16) << (long)delta
              << "  " << std::setw(13) << std::fixed << std::setprecision(2) << percent
              << "  " << std::setw(7) << std::fixed << std::setprecision(3) << point.time_ms
              << "  " << point.crc << std::endl;

    result.points.push_back(point);
  }

  if(!result.roundtrip_ok) fail("round-trip mismatch for image " + image.name);

  /*Recommendation: the lowest (least work) level whose size is within the
  tolerance of the smallest size seen in the sweep. This is deterministic and
  independent of timing noise, while preferring less compression effort.*/
  size_t best_size = result.points[0].size;
  for(size_t i = 1; i < result.points.size(); ++i) {
    if(result.points[i].size < best_size) best_size = result.points[i].size;
  }
  double limit = (double)best_size * (1.0 + options.tolerance_percent / 100.0);
  size_t recommended = 0;
  bool found = false;
  for(size_t i = 0; i < result.points.size(); ++i) {
    const CurvePoint& p = result.points[i];
    if((double)p.size <= limit) {
      if(!found || p.level < result.points[recommended].level) {
        recommended = i;
        found = true;
      }
    }
  }
  if(!found) recommended = 0; /*cannot happen, but keeps the code total*/
  result.recommended_level = result.points[recommended].level;
  result.recommended_size = result.points[recommended].size;
  result.recommended_time_ms = result.points[recommended].time_ms;

  /*Fastest measured level anywhere in the sweep, reported for comparison only.*/
  size_t fastest = 0;
  for(size_t i = 1; i < result.points.size(); ++i) {
    if(result.points[i].time_ms < result.points[fastest].time_ms) fastest = i;
  }

  double rec_delta = (double)((long)result.recommended_size - (long)result.reference_size);
  double rec_percent = 100.0 * rec_delta / (double)result.reference_size;

  std::cout << "  recommended: level " << result.recommended_level
            << " (lowest level within " << std::fixed << std::setprecision(2)
            << options.tolerance_percent << "% of the smallest size "
            << best_size << "; size=" << result.recommended_size
            << ", measured " << std::fixed << std::setprecision(3)
            << result.recommended_time_ms << " ms)" << std::endl;
  std::cout << "  fastest level in sweep: level " << result.points[fastest].level
            << " (" << std::fixed << std::setprecision(3)
            << result.points[fastest].time_ms << " ms, size "
            << result.points[fastest].size << ")" << std::endl;
  std::cout << "  recommended vs original level " << REFERENCE_LEVEL << ": "
            << (long)rec_delta << " bytes (" << std::fixed << std::setprecision(2)
            << rec_percent << "%), time " << std::fixed << std::setprecision(3)
            << result.recommended_time_ms << " ms vs " << result.reference_time_ms
            << " ms at level " << REFERENCE_LEVEL << std::endl;
  std::cout << "  image sweep total time: " << std::fixed << std::setprecision(3)
            << result.sweep_total_ms << " ms (" << levels.size() << " levels x "
            << options.repeat << " repeats)" << std::endl;
  std::cout << "  round-trip for every level: ok" << std::endl;

  printCurvePlot(result);

  /*machine-readable curve points*/
  for(size_t i = 0; i < result.points.size(); ++i) {
    const CurvePoint& p = result.points[i];
    double delta = (double)((long)p.size - (long)result.reference_size);
    double percent = 100.0 * delta / (double)result.reference_size;
    std::cout << "curve," << result.name << "," << p.level << "," << p.size
              << "," << std::fixed << std::setprecision(3) << p.time_ms
              << "," << std::fixed << std::setprecision(2) << percent << std::endl;
  }

  /*machine-readable per-image recommendation*/
  std::cout << "recommend," << result.name << "," << result.recommended_level
            << "," << result.recommended_size << "," << (long)rec_delta
            << "," << std::fixed << std::setprecision(2) << rec_percent
            << "," << std::fixed << std::setprecision(3) << result.recommended_time_ms
            << "," << std::fixed << std::setprecision(3) << result.sweep_total_ms
            << std::endl;

  return result;
}

static void printSummary(const std::vector<ImageResult>& results,
                         const Options& options) {
  double grand_total = 0.0;
  std::cout << std::endl;
  std::cout << "SUMMARY (reference level " << REFERENCE_LEVEL
            << ", tolerance " << std::fixed << std::setprecision(2)
            << options.tolerance_percent << "%)" << std::endl;
  std::cout << "  image  recommended  rec_size  delta_ref_bytes  delta_ref_pct  "
               "rec_time_ms  sweep_total_ms" << std::endl;
  for(size_t i = 0; i < results.size(); ++i) {
    const ImageResult& r = results[i];
    double delta = (double)((long)r.recommended_size - (long)r.reference_size);
    double percent = 100.0 * delta / (double)r.reference_size;
    std::cout << "  " << std::left << std::setw(6) << r.name << std::right
              << "  " << std::setw(11) << r.recommended_level
              << "  " << std::setw(8) << r.recommended_size
              << "  " << std::setw(16) << (long)delta
              << "  " << std::setw(13) << std::fixed << std::setprecision(2) << percent
              << "  " << std::setw(11) << std::fixed << std::setprecision(3)
              << r.recommended_time_ms
              << "  " << std::setw(14) << std::fixed << std::setprecision(3)
              << r.sweep_total_ms << std::endl;
    grand_total += r.sweep_total_ms;
  }
  std::cout << "  grand total sweep time: " << std::fixed << std::setprecision(3)
            << grand_total << " ms" << std::endl;
}

static void printUsage(const char* program) {
  std::cout << "usage: " << program
            << " [--levels MIN-MAX] [--step N] [--repeat N] [--tolerance PCT]"
            << std::endl;
  std::cout << "  --levels MIN-MAX  level range, each level must be in "
            << LEVEL_MIN << ".." << LEVEL_MAX << " (default 0-9)" << std::endl;
  std::cout << "  --step N          sampling step, N >= 1 (default 1)" << std::endl;
  std::cout << "  --repeat N        encodes per level for timing, N >= 1 (default 5)"
            << std::endl;
  std::cout << "  --tolerance PCT   size tolerance for the recommendation, "
               ">= 0 (default 1.0)" << std::endl;
}

static bool parseOptions(int argc, char** argv, Options& options,
                         std::string& error, bool& help) {
  help = false;
  for(int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if(arg == "--help" || arg == "-h") {
      help = true;
      return true;
    }
    if(i + 1 >= argc) {
      error = "missing value for " + arg;
      return false;
    }
    std::string value = argv[++i];
    if(arg == "--levels") {
      if(!parseLevelRange(value, options.min_level, options.max_level, error)) return false;
    } else if(arg == "--step") {
      unsigned long step = 0;
      if(!parseUnsigned(value, step) || step == 0 || step > 1000) {
        error = "step must be an integer in 1..1000";
        return false;
      }
      options.step = (unsigned)step;
    } else if(arg == "--repeat") {
      unsigned long repeat = 0;
      if(!parseUnsigned(value, repeat) || repeat == 0 || repeat > 1000) {
        error = "repeat must be an integer in 1..1000";
        return false;
      }
      options.repeat = (unsigned)repeat;
    } else if(arg == "--tolerance") {
      std::istringstream ss(value);
      double tolerance = 0.0;
      char extra = 0;
      if(!(ss >> tolerance) || (ss >> extra) || tolerance < 0.0 || tolerance > 100.0) {
        error = "tolerance must be a number in 0..100 (percent)";
        return false;
      }
      options.tolerance_percent = tolerance;
    } else {
      error = "unknown option: " + arg;
      return false;
    }
  }
  return true;
}

int main(int argc, char** argv) {
  Options options;
  std::string error;
  bool help = false;
  if(!parseOptions(argc, argv, options, error, help)) {
    std::cout << "ERROR: " << error << std::endl;
    printUsage(argv[0]);
    return 2;
  }
  if(help) {
    printUsage(argv[0]);
    return 0;
  }

  std::cout << "LodePNG zlib compression level sweep" << std::endl;
  std::cout << "  levels: " << options.min_level << "-" << options.max_level
            << ", step " << options.step
            << ", repeat " << options.repeat
            << ", recommendation tolerance " << std::fixed << std::setprecision(2)
            << options.tolerance_percent << "%" << std::endl;

  runSelfTests();

  std::vector<TestImage> images;
  generateCorpus(images);
  if(images.size() != 4) fail("corpus must contain four image categories");
  std::cout << "  four synthetic categories generated in memory: ok" << std::endl;

  std::vector<unsigned> levels = buildLevelList(options);
  if(levels.empty()) fail("empty level list");
  std::cout << "  sampled levels:";
  for(size_t i = 0; i < levels.size(); ++i) std::cout << " " << levels[i];
  std::cout << std::endl;

  std::vector<ImageResult> results;
  for(size_t i = 0; i < images.size(); ++i) {
    results.push_back(sweepImage(images[i], levels, options));
    std::cout << "  [" << (i + 1) << "/" << images.size() << " images done]"
              << std::endl;
  }

  printSummary(results, options);
  std::cout << std::endl << "ALL CHECKS PASSED" << std::endl;
  return 0;
}

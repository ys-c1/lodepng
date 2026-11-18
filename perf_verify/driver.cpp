/*
Verification + benchmark for the single-pass multi-candidate LFS_MINSUM change.

The pristine baseline lodepng and the modified one are built as two separate
shared libraries and each is dlopen'ed in its own linker namespace via dlmopen,
so identical symbol names do not collide. API symbols are C++-mangled (lodepng
has no extern "C"); the mangled names used here were obtained with nm.

Part 1 (correctness): 200 small self-generated images across four content
  classes and seven color/bitdepth configurations. For each image both the
  baseline (five attempts per row) and the new (single scan) encoders run.
  The full pre-deflate filtered data is compared row by row: filter type byte
  and every filtered byte must match exactly; the PNG files must be identical.

Part 2 (performance): four 512x512 images, encode time and output size for
  both implementations under identical settings.

No third-party libraries: only glibc dlfcn and clock_gettime.
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lodepng.h"

#include <dlfcn.h>
#include <link.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <vector>

/* lodepng color types (values match lodepng.h) */
enum { CT_GREY = 0, CT_RGB = 2, CT_PALETTE = 3, CT_GREY_ALPHA = 4, CT_RGBA = 6 };

typedef unsigned (*encode_fn)(unsigned char**, size_t*, const unsigned char*,
                              unsigned, unsigned, LodePNGState*);
typedef void (*state_init_fn)(LodePNGState*);
typedef void (*state_cleanup_fn)(LodePNGState*);
typedef unsigned (*zdec_fn)(unsigned char**, size_t*, const unsigned char*, size_t,
                            const LodePNGDecompressSettings*);
typedef void (*libc_free_fn)(void*);
typedef unsigned (*inspect_fn)(unsigned*, unsigned*, LodePNGState*,
                               const unsigned char*, size_t);
typedef unsigned (*get_bpp_fn)(const LodePNGColorMode*);
typedef unsigned (*palette_add_fn)(LodePNGColorMode*,
                                   unsigned char, unsigned char, unsigned char, unsigned char);

struct Lib {
  void* handle;
  encode_fn encode;
  state_init_fn state_init;
  state_cleanup_fn state_cleanup;
  zdec_fn zdec;
  inspect_fn inspect;
  get_bpp_fn get_bpp;
  palette_add_fn palette_add;
  libc_free_fn lib_free; /* libc free inside this namespace; every buffer from this lib needs it */
  const LodePNGDecompressSettings* default_dec;
};

static Lib load_lib(const char* path) {
  Lib lib;
  std::memset(&lib, 0, sizeof(lib));
  /* own link-map namespace so the two lodepng builds never share symbols */
  lib.handle = dlmopen(LM_ID_NEWLM, path, RTLD_NOW | RTLD_LOCAL);
  if(!lib.handle) {
    std::fprintf(stderr, "dlmopen %s failed: %s\n", path, dlerror());
    std::exit(2);
  }
  lib.encode = (encode_fn)dlsym(lib.handle, "_Z14lodepng_encodePPhPmPKhjjP12LodePNGState");
  lib.state_init = (state_init_fn)dlsym(lib.handle, "_Z18lodepng_state_initP12LodePNGState");
  lib.state_cleanup = (state_cleanup_fn)dlsym(lib.handle, "_Z21lodepng_state_cleanupP12LodePNGState");
  lib.zdec = (zdec_fn)dlsym(lib.handle,
      "_Z23lodepng_zlib_decompressPPhPmPKhmPK25LodePNGDecompressSettings");
  lib.inspect = (inspect_fn)dlsym(lib.handle, "_Z15lodepng_inspectPjS_P12LodePNGStatePKhm");
  lib.get_bpp = (get_bpp_fn)dlsym(lib.handle, "_Z15lodepng_get_bppPK16LodePNGColorMode");
  lib.palette_add = (palette_add_fn)dlsym(lib.handle, "_Z19lodepng_palette_addP16LodePNGColorModehhhh");
  lib.lib_free = (libc_free_fn)dlsym(lib.handle, "free");
  lib.default_dec = (const LodePNGDecompressSettings*)dlsym(
      lib.handle, "lodepng_default_decompress_settings");
  if(!lib.encode || !lib.state_init || !lib.state_cleanup || !lib.zdec || !lib.inspect ||
     !lib.get_bpp || !lib.palette_add || !lib.lib_free || !lib.default_dec) {
    std::fprintf(stderr, "dlsym failed in %s: %s\n", path, dlerror());
    std::exit(2);
  }
  return lib;
}

static unsigned channels_of(int colortype) {
  switch(colortype) {
    case CT_GREY: return 1;
    case CT_RGB: return 3;
    case CT_PALETTE: return 1;
    case CT_GREY_ALPHA: return 2;
    case CT_RGBA: return 4;
    default: return 0;
  }
}

/* deterministic PRNG so the whole run is reproducible */
static uint64_t rng_state = 0x123456789abcdefULL;
static unsigned rng_next() {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return (unsigned)(rng_state >> 16);
}

enum { CLASS_PHOTO = 0, CLASS_GRADIENT = 1, CLASS_TEXT = 2, CLASS_FLAT = 3 };
static const char* class_name(int c) {
  static const char* n[4] = {"photo-noise", "gradient", "text-like", "flat-solids"};
  return n[c];
}

static std::vector<unsigned char> gen_image(unsigned w, unsigned h, int cls,
                                            int colortype, unsigned bitdepth) {
  unsigned ch = channels_of(colortype);
  unsigned bpp_bytes = (ch * bitdepth + 7) / 8;
  size_t rowbytes = (size_t)w * bpp_bytes;
  std::vector<unsigned char> img(rowbytes * h, 0);
  unsigned maxv = (1u << bitdepth) - 1u;

  for(unsigned y = 0; y < h; ++y) {
    for(unsigned x = 0; x < w; ++x) {
      unsigned vals[4] = {0, 0, 0, maxv};
      switch(cls) {
        case CLASS_PHOTO: {
          unsigned base = (unsigned)(((x * 7 + y * 13) & 0xff) * maxv / 255u);
          for(unsigned c = 0; c < ch && c < 3; ++c)
            vals[c] = (base + rng_next() % (maxv + 1) + c * 17) & maxv;
          if(ch == 2 || ch == 4) vals[ch - 1] = rng_next() % 4 == 0 ? (maxv / 2) : maxv;
          break;
        }
        case CLASS_GRADIENT: {
          for(unsigned c = 0; c < ch && c < 3; ++c)
            vals[c] = (unsigned)(((x * (maxv + 1) / (w + 1))
                                + (y * (maxv + 1) / (h + 1)) / 2 + c * 31)) & maxv;
          if(ch == 2 || ch == 4) vals[ch - 1] = maxv;
          break;
        }
        case CLASS_TEXT: {
          int ink = ((x / 3 + y / 5) % 7 < 3) ^ ((x ^ y) % 11 < 2);
          unsigned fg = maxv, bg = maxv / 8;
          for(unsigned c = 0; c < ch && c < 3; ++c) vals[c] = ink ? fg : bg;
          if(ch == 2 || ch == 4) vals[ch - 1] = maxv;
          break;
        }
        case CLASS_FLAT: {
          unsigned band = (x / (w / 3 + 1) + y / (h / 2 + 1)) % (maxv + 1);
          for(unsigned c = 0; c < ch && c < 3; ++c) vals[c] = (band + c * 64) & maxv;
          if(ch == 2 || ch == 4) vals[ch - 1] = maxv;
          break;
        }
      }
      unsigned char* px = &img[(size_t)y * rowbytes + (size_t)x * bpp_bytes];
      if(bitdepth == 8) {
        for(unsigned c = 0; c < ch; ++c) px[c] = (unsigned char)vals[c];
      } else if(bitdepth == 16) {
        for(unsigned c = 0; c < ch; ++c) {
          px[2 * c] = (unsigned char)(vals[c] >> 8);
          px[2 * c + 1] = (unsigned char)(vals[c] & 255);
        }
      } else {
        /* sub-byte: big-endian bit packing per PNG */
        size_t byte_index = (size_t)y * rowbytes + ((size_t)x * bitdepth) / 8;
        unsigned bit_off = (unsigned)(((size_t)x * bitdepth) % 8);
        img[byte_index] |= (unsigned char)((vals[0] & maxv) << (8 - bitdepth - bit_off));
      }
    }
  }
  return img;
}

struct ColorCfg { int colortype; unsigned bitdepth; unsigned palette_zero; };

/* Realistic 8-bit performance image. Each class keeps its characteristic
structure but carries per-pixel texture/noise (the anti-aliasing, grain and
sensor noise present in real content), so that the filter stage is a meaningful
fraction of encode time instead of being drowned out by an almost-free deflate
on perfectly clean synthetic data. Deterministic via the shared PRNG. */
static std::vector<unsigned char> gen_perf_image(unsigned w, unsigned h, int cls,
                                                 int colortype) {
  unsigned ch = channels_of(colortype);
  std::vector<unsigned char> img((size_t)w * h * ch, 0);
  const unsigned noise_amp = 40; /* ~16% amplitude grain, realistic for text/flat scans */
  for(unsigned y = 0; y < h; ++y) {
    for(unsigned x = 0; x < w; ++x) {
      unsigned char* px = &img[((size_t)y * w + x) * ch];
      unsigned vals[4] = {0, 0, 0, 255};
      for(unsigned c = 0; c < ch && c < 3; ++c) {
        unsigned base;
        switch(cls) {
          case CLASS_PHOTO:
            base = ((x * 7 + y * 13) & 0xff);
            vals[c] = (base + rng_next() /*full-range photo noise*/ + c * 17) & 255;
            continue;
          case CLASS_GRADIENT:
            base = (x * 255u / w) + (y * 255u / h) / 2 + c * 31;
            break;
          case CLASS_TEXT: {
            int ink = ((x / 3 + y / 5) % 7 < 3) ^ ((x ^ y) % 11 < 2);
            base = ink ? 255 : 31;
            break;
          }
          default: /* CLASS_FLAT: large constant bands with sharp boundaries */
            base = (x / (w / 3 + 1) + y / (h / 2 + 1)) * 64 + c * 64;
            break;
        }
        /* bounded grain on top of the structured content */
        vals[c] = (base + rng_next() % (noise_amp + 1)) & 255;
      }
      if(ch == 4) px[3] = 255;
      for(unsigned c = 0; c < ch; ++c) px[c] = (unsigned char)vals[c];
    }
  }
  return img;
}

static unsigned encode_with(const Lib& lib, unsigned char** out, size_t* outsize,
                            const unsigned char* img, unsigned w, unsigned h,
                            const ColorCfg& cfg) {
  LodePNGState state;
  lib.state_init(&state);
  state.info_raw.colortype = (LodePNGColorType)cfg.colortype;
  state.info_raw.bitdepth = cfg.bitdepth;
  state.encoder.filter_strategy = LFS_MINSUM;
  state.encoder.filter_palette_zero = cfg.palette_zero;
  state.encoder.auto_convert = 0; /* keep requested color type so MINSUM really runs */
  /* with auto_convert off the PNG output color mode comes from info_png, not info_raw */
  state.info_png.color.colortype = (LodePNGColorType)cfg.colortype;
  state.info_png.color.bitdepth = cfg.bitdepth;
  if(cfg.colortype == CT_PALETTE) {
    /* a palette image needs a real PLTE, otherwise the encoder cannot work in this mode */
    for(unsigned i = 0; i < 256; ++i)
      lib.palette_add(&state.info_png.color,
                      (unsigned char)i, (unsigned char)(255 - i),
                      (unsigned char)((i * 7) & 255), 255);
  }
  unsigned err = lib.encode(out, outsize, img, w, h, &state);
  lib.state_cleanup(&state);
  return err;
}

/* linebytes of the actual PNG output, read back from its header */
static bool output_linebytes(const Lib& lib, const unsigned char* png, size_t pngsize,
                             unsigned w, unsigned h, size_t& linebytes) {
  unsigned bw, bh;
  LodePNGState ins;
  lib.state_init(&ins);
  unsigned err = lib.inspect(&bw, &bh, &ins, png, pngsize);
  if(err != 0 || bw != w || bh != h) { lib.state_cleanup(&ins); return false; }
  unsigned bpp = lib.get_bpp(&ins.info_png.color);
  linebytes = ((size_t)w * bpp + 7) / 8;
  lib.state_cleanup(&ins);
  return true;
}

static bool inflate_idat(const Lib& lib, const unsigned char* png, size_t pngsize,
                         std::vector<unsigned char>& raw) {
  if(pngsize < 8 || png[0] != 137 || png[1] != 80) return false;
  std::vector<unsigned char> idat;
  size_t p = 8;
  while(p + 8 <= pngsize) {
    size_t len = (png[p] << 24) | (png[p + 1] << 16) | (png[p + 2] << 8) | png[p + 3];
    const char* typ = (const char*)&png[p + 4];
    if(p + 12 + len > pngsize) return false;
    if(std::memcmp(typ, "IDAT", 4) == 0)
      idat.insert(idat.end(), png + p + 8, png + p + 8 + len);
    p += 12 + len;
  }
  unsigned char* out = 0;
  size_t outsize = 0;
  if(lib.zdec(&out, &outsize, idat.empty() ? 0 : idat.data(), idat.size(),
              lib.default_dec) != 0) return false;
  raw.assign(out, out + outsize);
  lib.lib_free(out); /* freed with the same namespace's libc that allocated it */
  return true;
}

static double now_sec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
  const char* base_path = argc > 1 ? argv[1] : "./liblodepng_base.so";
  const char* new_path  = argc > 2 ? argv[2] : "./liblodepng_new.so";
  Lib base = load_lib(base_path);
  Lib nlib = load_lib(new_path);

  ColorCfg cfgs[7] = {
    {CT_RGBA,       8,  1},
    {CT_RGB,        8,  1},
    {CT_GREY_ALPHA, 8,  1},
    {CT_GREY,       8,  1},
    {CT_GREY,       16, 1},
    {CT_GREY,       1,  0}, /* palette_zero off: MINSUM runs on sub-byte data */
    {CT_PALETTE,    8,  0},
  };
  const unsigned NCFGS = sizeof(cfgs) / sizeof(cfgs[0]);
  const unsigned NIMAGES = 200;

  std::printf("=== Part 1: correctness on %u generated images ===\n", NIMAGES);
  unsigned rows_checked = 0;
  size_t total_pixels_corr = 0;
  unsigned filter_hist[5] = {0, 0, 0, 0, 0};

  for(unsigned n = 0; n < NIMAGES; ++n) {
    rng_state = 0x9e3779b97f4a7c15ULL ^ (uint64_t)(n + 1) * 0xd1b54a32d192ed03ULL;
    ColorCfg cfg = cfgs[n % NCFGS];
    int cls = (int)(n % 4);
    unsigned w = 1 + (rng_next() % 31);
    unsigned h = 1 + (rng_next() % 17);

    std::vector<unsigned char> img = gen_image(w, h, cls, cfg.colortype, cfg.bitdepth);

    unsigned char *bpng = 0, *npng = 0;
    size_t bsz = 0, nsz = 0;
    unsigned e1 = encode_with(base, &bpng, &bsz, img.data(), w, h, cfg);
    unsigned e2 = encode_with(nlib, &npng, &nsz, img.data(), w, h, cfg);
    if(e1 || e2) {
      std::printf("FAIL image %u: encoder errors base=%u new=%u\n", n, e1, e2);
      return 1;
    }
    if(bsz != nsz || std::memcmp(bpng, npng, bsz) != 0) {
      std::printf("FAIL image %u (w=%u h=%u cfg=%u cls=%s): PNG bytes differ (%zu vs %zu)\n",
                  n, w, h, n % NCFGS, class_name(cls), bsz, nsz);
      return 1;
    }

    std::vector<unsigned char> braw, nraw;
    if(!inflate_idat(base, bpng, bsz, braw) || !inflate_idat(nlib, npng, nsz, nraw)) {
      std::printf("FAIL image %u: inflate failed\n", n);
      return 1;
    }
    if(braw != nraw) {
      std::printf("FAIL image %u: filtered data differs\n", n);
      return 1;
    }
    /* explicitly check every filter-type byte and count rows, using the actual output color mode */
    size_t linebytes = 0;
    if(!output_linebytes(base, bpng, bsz, w, h, linebytes)) {
      std::printf("FAIL image %u: inspect failed\n", n);
      return 1;
    }
    size_t stride = linebytes + 1;
    if(braw.size() != stride * h) {
      std::printf("FAIL image %u: unexpected filtered size\n", n);
      return 1;
    }
    for(unsigned y = 0; y < h; ++y) {
      unsigned char fb = braw[y * stride];
      unsigned char fn = nraw[y * stride];
      if(fb > 4 || fb != fn) {
        std::printf("FAIL image %u row %u: filter type %u vs %u\n", n, y, fb, fn);
        return 1;
      }
      if(std::memcmp(&braw[y * stride + 1], &nraw[y * stride + 1], linebytes) != 0) {
        std::printf("FAIL image %u row %u: filtered bytes differ\n", n, y);
        return 1;
      }
      ++filter_hist[fb];
      ++rows_checked;
    }
    total_pixels_corr += (size_t)w * h;
    base.lib_free(bpng);
    nlib.lib_free(npng);
  }
  std::printf("PASS: %u images, %u scanlines (%zu pixels)\n", NIMAGES, rows_checked, total_pixels_corr);
  std::printf("      filter type byte and every filtered byte match; PNG files byte-identical\n");
  std::printf("      chosen filter distribution (0..4): %u %u %u %u %u\n\n",
              filter_hist[0], filter_hist[1], filter_hist[2], filter_hist[3], filter_hist[4]);

  std::printf("=== Part 2: encode timing and size, four image classes (512x512) ===\n");
  const unsigned PW = 512, PH = 512;
  struct Bench { int cls; int colortype; unsigned bitdepth; unsigned iters; };
  Bench benches[4] = {
    {CLASS_PHOTO,    CT_RGBA, 8, 40},
    {CLASS_GRADIENT, CT_RGB,  8, 60},
    {CLASS_TEXT,     CT_RGBA, 8, 60},
    {CLASS_FLAT,     CT_RGB,  8, 80},
  };

  std::printf("%-12s %-6s %-12s %-12s %-10s %-12s %-12s\n",
              "class", "cfg", "base time", "new time", "time cut", "base size", "new size");
  double sum_base_t = 0, sum_new_t = 0;
  size_t sum_base_sz = 0, sum_new_sz = 0;

  for(int bi = 0; bi < 4; ++bi) {
    rng_state = 0xabcdef0123456789ULL ^ (uint64_t)(bi + 7) * 0x9e3779b97f4a7c15ULL;
    ColorCfg cfg = {benches[bi].colortype, benches[bi].bitdepth, 1};
    std::vector<unsigned char> img = gen_perf_image(PW, PH, benches[bi].cls,
                                                    benches[bi].colortype);
    unsigned char* png = 0;
    size_t sz = 0;

    unsigned char *bpng = 0, *npng = 0;
    size_t bsz = 0, nsz = 0;
    encode_with(base, &bpng, &bsz, img.data(), PW, PH, cfg);
    encode_with(nlib, &npng, &nsz, img.data(), PW, PH, cfg);
    if(bsz != nsz || std::memcmp(bpng, npng, bsz) != 0) {
      std::printf("FAIL benchmark %d: outputs differ\n", bi);
      return 1;
    }
    base.lib_free(bpng);
    nlib.lib_free(npng);

    double t0 = now_sec();
    for(unsigned k = 0; k < benches[bi].iters; ++k) {
      encode_with(base, &png, &sz, img.data(), PW, PH, cfg);
      base.lib_free(png); png = 0;
    }
    double t1 = now_sec();
    for(unsigned k = 0; k < benches[bi].iters; ++k) {
      encode_with(nlib, &png, &sz, img.data(), PW, PH, cfg);
      nlib.lib_free(png); png = 0;
    }
    double t2 = now_sec();

    double base_t = t1 - t0, new_t = t2 - t1;
    sum_base_t += base_t; sum_new_t += new_t;
    sum_base_sz += bsz; sum_new_sz += nsz;
    std::printf("%-12s %-6s %-12.4f %-12.4f %-9.2f%% %-12zu %-12zu\n",
                class_name(benches[bi].cls),
                benches[bi].colortype == CT_RGBA ? "RGBA8" : "RGB8",
                base_t, new_t, 100.0 * (1.0 - new_t / base_t), bsz, nsz);
  }

  std::printf("\n%-12s %-6s %-12.4f %-12.4f %-9.2f%% %-12zu %-12zu\n",
              "TOTAL", "-", sum_base_t, sum_new_t,
              100.0 * (1.0 - sum_new_t / sum_base_t), sum_base_sz, sum_new_sz);
  double size_delta_pct = 100.0 * ((double)sum_new_sz - (double)sum_base_sz) / (double)sum_base_sz;
  std::printf("size regression: %+.4f%% (acceptance: <= 1%%)\n", size_delta_pct);
  std::printf("time reduction: %.2f%% (acceptance: >= 30%%)\n",
              100.0 * (1.0 - sum_new_t / sum_base_t));
  return 0;
}

/*Differential equivalence test: old (pre-refactor, old/oldpng.*) vs new (refactored, ../lodepng.*)
PNG decoder. Compares error codes, dimensions, decoded pixels and selected decoder state between
the two implementations on identical inputs. Any difference is a refactoring bug by construction,
because the refactor is intended to be behavior preserving (and is verified as such here).*/
#include "../lodepng.h"
#include "old/oldpng.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>

/* ------------------------------------------------------------------ */
/* RNG (splitmix64), deterministic so failures are reproducible        */
/* ------------------------------------------------------------------ */
struct Rng {
  unsigned long long s;
  explicit Rng(unsigned long long seed) : s(seed) {}
  unsigned long long next() {
    unsigned long long z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  unsigned u(unsigned n) { return (unsigned)(next() % n); } /*0..n-1*/
  bool bit() { return (next() & 1) != 0; }
};

/* ------------------------------------------------------------------ */
/* old/new API bridges (the two libs have identical APIs, renamed)     */
/* ------------------------------------------------------------------ */
static unsigned call_decode(unsigned char** out, unsigned* w, unsigned* h, LodePNGState* st,
                            const unsigned char* in, size_t n) {
  return lodepng_decode(out, w, h, st, in, n);
}
static unsigned call_decode(unsigned char** out, unsigned* w, unsigned* h, OldPNGState* st,
                            const unsigned char* in, size_t n) {
  return oldpng_decode(out, w, h, st, in, n);
}
static void call_state_init(LodePNGState* st) { lodepng_state_init(st); }
static void call_state_init(OldPNGState* st) { oldpng_state_init(st); }
static void call_state_cleanup(LodePNGState* st) { lodepng_state_cleanup(st); }
static void call_state_cleanup(OldPNGState* st) { oldpng_state_cleanup(st); }

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static size_t raw_size_bytes(int colortype, int bitdepth, unsigned w, unsigned h) {
  size_t channels;
  switch(colortype) {
    case 0: channels = 1; break; /*LCT_GREY*/
    case 2: channels = 3; break; /*LCT_RGB*/
    case 3: channels = 1; break; /*LCT_PALETTE*/
    case 4: channels = 2; break; /*LCT_GREY_ALPHA*/
    case 6: channels = 4; break; /*LCT_RGBA*/
    default: return 0;
  }
  return ((size_t)w * (size_t)h * channels * (size_t)bitdepth + 7) / 8;
}

static std::vector<unsigned char> make_chunk(const char* type, const std::vector<unsigned char>& data) {
  unsigned char* out = 0;
  size_t outsize = 0;
  unsigned e = lodepng_chunk_create(&out, &outsize, (unsigned)data.size(), type,
                                    data.empty() ? 0 : &data[0]);
  if(e) { fprintf(stderr, "chunk create failed\n"); exit(2); }
  std::vector<unsigned char> v(out, out + outsize);
  free(out);
  return v;
}

/*insert a full chunk (incl. length/type/crc) right after IHDR or right before IEND*/
static void inject_chunk(std::vector<unsigned char>& png, const std::vector<unsigned char>& chunk,
                         bool after_ihdr) {
  size_t pos = after_ihdr ? 33 : png.size() - 12;
  png.insert(png.begin() + pos, chunk.begin(), chunk.end());
}

/*split the first IDAT chunk of a PNG into 2-4 IDAT chunks (tests IDAT accumulation)*/
static void split_idat(std::vector<unsigned char>& png, Rng& rng) {
  size_t pos = 33;
  while(pos + 12 <= png.size()) {
    unsigned len = ((unsigned)png[pos] << 24) | (png[pos+1] << 16) | (png[pos+2] << 8) | png[pos+3];
    if(pos + 12 + len > png.size()) return;
    if(std::memcmp(&png[pos + 4], "IDAT", 4) == 0) {
      std::vector<unsigned char> data(png.begin() + pos + 8, png.begin() + pos + 8 + len);
      int parts = 2 + (int)rng.u(3);
      std::vector<unsigned char> replacement;
      size_t off = 0;
      for(int i = 0; i < parts; ++i) {
        size_t n = (i == parts - 1) ? data.size() - off : rng.u((unsigned)(data.size() - off + 1));
        std::vector<unsigned char> piece(data.begin() + off, data.begin() + off + n);
        std::vector<unsigned char> chunk = make_chunk("IDAT", piece);
        replacement.insert(replacement.end(), chunk.begin(), chunk.end());
        off += n;
      }
      png.erase(png.begin() + pos, png.begin() + pos + 12 + len);
      png.insert(png.begin() + pos, replacement.begin(), replacement.end());
      return;
    }
    pos += 12 + len;
  }
}

/* ------------------------------------------------------------------ */
/* random PNG generation (encoder of the NEW implementation; the       */
/* encoder is untouched by the refactor and both decoders see the      */
/* same bytes, so this is a valid differential setup)                  */
/* ------------------------------------------------------------------ */
static const int kCombos[][2] = {
  {0,1},{0,2},{0,4},{0,8},{0,16},   /*grey*/
  {2,8},{2,16},                    /*RGB*/
  {3,1},{3,2},{3,4},{3,8},         /*palette*/
  {4,8},{4,16},                    /*grey+alpha*/
  {6,8},{6,16}                     /*RGBA*/
};
static const int kNumCombos = (int)(sizeof(kCombos) / sizeof(kCombos[0]));

static void init_random_mode(Rng& rng, LodePNGColorMode* m, int ct, int bd, bool palette_alpha) {
  lodepng_color_mode_init(m);
  m->colortype = (LodePNGColorType)ct;
  m->bitdepth = (unsigned)bd;
  if(ct == LCT_PALETTE) {
    unsigned n = 1u << bd; /*full palette: every packed index is valid*/
    for(unsigned i = 0; i < n; ++i) {
      unsigned char a = 255;
      if(palette_alpha && rng.u(100) < 30) a = (unsigned char)rng.u(256); /*-> tRNS chunk*/
      lodepng_palette_add(m, (unsigned char)rng.u(256), (unsigned char)rng.u(256),
                          (unsigned char)rng.u(256), a);
    }
  }
}

static std::vector<unsigned char> encode_random_png(Rng& rng, LodePNGColorMode* mode_out,
                                                    unsigned* w_out, unsigned* h_out) {
  int ci = (int)rng.u(kNumCombos);
  int ct = kCombos[ci][0], bd = kCombos[ci][1];
  unsigned w = 1 + rng.u(48);
  unsigned h = 1 + rng.u(48);
  if(rng.u(100) < 8) { w = 1 + rng.u(300); h = 1 + rng.u(300); }
  if(rng.u(100) < 10) { w = 1 + rng.u(4); h = 1 + rng.u(4); } /*tiny: interlace edge cases*/

  bool palette_alpha = (ct == LCT_PALETTE) && rng.bit();
  LodePNGColorMode mode;
  init_random_mode(rng, &mode, ct, bd, palette_alpha);

  LodePNGState st;
  lodepng_state_init(&st);
  lodepng_color_mode_copy(&st.info_png.color, &mode);
  lodepng_color_mode_copy(&st.info_raw, &mode);
  st.info_png.interlace_method = rng.bit() ? 1 : 0;

  /*tRNS color key for grey/RGB*/
  if((ct == LCT_GREY || ct == LCT_RGB) && rng.u(100) < 30) {
    unsigned maxv = (bd == 16) ? 65535 : 255;
    st.info_png.color.key_defined = 1;
    st.info_png.color.key_r = rng.u(maxv + 1);
    st.info_png.color.key_g = rng.u(maxv + 1);
    st.info_png.color.key_b = rng.u(maxv + 1);
  }
  /*tEXt via the encoder*/
  if(rng.u(100) < 20) {
    char buf[64];
    std::sprintf(buf, "trial text %u", rng.u(1000000));
    lodepng_add_text(&st.info_png, "Comment", buf);
  }
  /*a few metadata chunks the encoder can emit*/
  if(rng.u(100) < 15) { st.info_png.gama_defined = 1; st.info_png.gama_gamma = 40000 + rng.u(20000); }
  if(rng.u(100) < 10) {
    st.info_png.phys_defined = 1; st.info_png.phys_unit = 1;
    st.info_png.phys_x = 1000 + rng.u(4000); st.info_png.phys_y = 1000 + rng.u(4000);
  }
  if(rng.u(100) < 10) {
    st.info_png.time_defined = 1;
    st.info_png.time.year = 1990 + rng.u(60); st.info_png.time.month = 1 + rng.u(12);
    st.info_png.time.day = 1 + rng.u(28); st.info_png.time.hour = rng.u(24);
    st.info_png.time.minute = rng.u(60); st.info_png.time.second = rng.u(60);
  }
  if(rng.u(100) < 10 && ct != LCT_PALETTE) {
    unsigned maxv = (bd == 16) ? 65535 : 255;
    st.info_png.background_defined = 1;
    st.info_png.background_r = rng.u(maxv + 1);
    st.info_png.background_g = rng.u(maxv + 1);
    st.info_png.background_b = rng.u(maxv + 1);
    if(ct == LCT_GREY || ct == LCT_GREY_ALPHA) { /*grey: channels must agree*/
      st.info_png.background_g = st.info_png.background_r;
      st.info_png.background_b = st.info_png.background_r;
    }
  }

  size_t rawsize = raw_size_bytes(ct, bd, w, h);
  std::vector<unsigned char> raw(rawsize);
  for(size_t i = 0; i < rawsize; ++i) raw[i] = (unsigned char)rng.u(256);

  unsigned char* png = 0;
  size_t pngsize = 0;
  unsigned e = lodepng_encode(&png, &pngsize, raw.empty() ? 0 : &raw[0], w, h, &st);
  lodepng_state_cleanup(&st);
  if(e) { fprintf(stderr, "encode failed: %u %s\n", e, lodepng_error_text(e)); exit(2); }
  std::vector<unsigned char> v(png, png + pngsize);
  free(png);

  if(rng.u(100) < 15) split_idat(v, rng);

  *mode_out = mode; /*caller must lodepng_color_mode_cleanup*/
  *w_out = w;
  *h_out = h;
  return v;
}

/* ------------------------------------------------------------------ */
/* decode + compare                                                    */
/* ------------------------------------------------------------------ */
struct DecodeSpec {
  int colortype;
  int bitdepth;
  int color_convert;
  int read_text_chunks;
  int remember_unknown_chunks;
  int ignore_crc;
  int ignore_end;
};

struct DecodeResult {
  unsigned error = 0;
  unsigned w = 0, h = 0;
  std::vector<unsigned char> pixels;
  int raw_colortype = 0, raw_bitdepth = 0;
  std::vector<unsigned char> raw_palette;
  size_t unknown_sizes[3] = {0, 0, 0};
  std::string text_digest;
};

template<class S>
static DecodeResult decode_spec(const DecodeSpec& spec, const unsigned char* png, size_t pngsize) {
  S st;
  call_state_init(&st);
  st.info_raw.colortype = static_cast<decltype(st.info_raw.colortype)>(spec.colortype);
  st.info_raw.bitdepth = (unsigned)spec.bitdepth;
  st.decoder.color_convert = spec.color_convert;
  st.decoder.read_text_chunks = spec.read_text_chunks;
  st.decoder.remember_unknown_chunks = spec.remember_unknown_chunks;
  st.decoder.ignore_crc = spec.ignore_crc;
  st.decoder.ignore_end = spec.ignore_end;

  DecodeResult r;
  unsigned char* out = 0;
  r.error = call_decode(&out, &r.w, &r.h, &st, png, pngsize);
  r.raw_colortype = (int)st.info_raw.colortype;
  r.raw_bitdepth = (int)st.info_raw.bitdepth;
  if(r.error == 0 && out) {
    size_t n = raw_size_bytes(r.raw_colortype, r.raw_bitdepth, r.w, r.h);
    r.pixels.assign(out, out + n);
  }
  if(out) free(out);
  if(spec.color_convert == 0 && st.info_raw.palette && st.info_raw.palettesize) {
    r.raw_palette.assign(st.info_raw.palette, st.info_raw.palette + 4 * st.info_raw.palettesize);
  }
  for(int i = 0; i < 3; ++i) r.unknown_sizes[i] = st.info_png.unknown_chunks_size[i];
  for(size_t i = 0; i < st.info_png.text_num; ++i) {
    r.text_digest += st.info_png.text_keys[i]; r.text_digest += '\1';
    r.text_digest += st.info_png.text_strings[i]; r.text_digest += '\2';
  }
  for(size_t i = 0; i < st.info_png.itext_num; ++i) {
    r.text_digest += st.info_png.itext_keys[i]; r.text_digest += '\1';
    r.text_digest += st.info_png.itext_langtags[i]; r.text_digest += '\1';
    r.text_digest += st.info_png.itext_transkeys[i]; r.text_digest += '\1';
    r.text_digest += st.info_png.itext_strings[i]; r.text_digest += '\2';
  }
  call_state_cleanup(&st);
  return r;
}

static long g_checked = 0;
static int g_failures = 0;

static void save_file(const char* path, const std::vector<unsigned char>& data) {
  FILE* f = std::fopen(path, "wb");
  if(!f) return;
  if(!data.empty()) std::fwrite(&data[0], 1, data.size(), f);
  std::fclose(f);
}

static void compare_pair(const DecodeResult& o, const DecodeResult& n, const char* context,
                         const std::vector<unsigned char>& png) {
  ++g_checked;
  bool ok = o.error == n.error && o.w == n.w && o.h == n.h && o.pixels == n.pixels
            && o.raw_colortype == n.raw_colortype && o.raw_bitdepth == n.raw_bitdepth
            && o.raw_palette == n.raw_palette && o.text_digest == n.text_digest
            && o.unknown_sizes[0] == n.unknown_sizes[0] && o.unknown_sizes[1] == n.unknown_sizes[1]
            && o.unknown_sizes[2] == n.unknown_sizes[2];
  if(ok) return;
  ++g_failures;
  std::printf("MISMATCH [%s]: error %u vs %u, wh %ux%u vs %ux%u, pixel bytes %zu vs %zu\n",
              context, o.error, n.error, o.w, o.h, n.w, n.h, o.pixels.size(), n.pixels.size());
  if(o.pixels != n.pixels && o.pixels.size() == n.pixels.size()) {
    for(size_t i = 0; i < o.pixels.size(); ++i) {
      if(o.pixels[i] != n.pixels[i]) {
        std::printf("  first pixel diff at byte %zu: %u vs %u\n", i, o.pixels[i], n.pixels[i]);
        break;
      }
    }
  }
  char path[256];
  std::sprintf(path, "../artifacts/mismatch_%d.png", g_failures);
  save_file(path, png);
  std::printf("  input saved to %s\n", path);
  if(g_failures > 20) { std::printf("too many failures, aborting\n"); std::exit(1); }
}

static void run_both(const DecodeSpec& spec, const std::vector<unsigned char>& png,
                     const char* context) {
  DecodeResult o = decode_spec<OldPNGState>(spec, png.empty() ? 0 : &png[0], png.size());
  DecodeResult n = decode_spec<LodePNGState>(spec, png.empty() ? 0 : &png[0], png.size());
  compare_pair(o, n, context, png);
}

static DecodeSpec random_decode_spec(Rng& rng, int src_ct, int src_bd) {
  DecodeSpec s;
  if(rng.u(100) < 35) { s.colortype = src_ct; s.bitdepth = src_bd; }
  else {
    int ci = (int)rng.u(kNumCombos);
    s.colortype = kCombos[ci][0];
    s.bitdepth = kCombos[ci][1];
  }
  s.color_convert = rng.u(100) < 25 ? 0 : 1;
  s.read_text_chunks = rng.bit() ? 1 : 0;
  s.remember_unknown_chunks = rng.bit() ? 1 : 0;
  s.ignore_crc = rng.u(100) < 5 ? 1 : 0;
  s.ignore_end = 0;
  return s;
}

/* ------------------------------------------------------------------ */
/* 1) random differential: 10000 random PNGs                           */
/* ------------------------------------------------------------------ */
static void run_random_differential(int count) {
  Rng rng(0xDEC0DE01ull);
  for(int i = 0; i < count; ++i) {
    LodePNGColorMode mode;
    unsigned w, h;
    std::vector<unsigned char> png = encode_random_png(rng, &mode, &w, &h);
    DecodeSpec spec = random_decode_spec(rng, (int)mode.colortype, (int)mode.bitdepth);
    run_both(spec, png, "random");
    lodepng_color_mode_cleanup(&mode);
  }
  std::printf("random differential: %d PNGs done\n", count);
}

/* ------------------------------------------------------------------ */
/* 2) mutation differential: corrupted PNGs must fail identically      */
/* ------------------------------------------------------------------ */
static void mutate(Rng& rng, std::vector<unsigned char>& png) {
  int kind = (int)rng.u(4);
  if(png.empty()) return;
  if(kind == 0) {
    png[rng.u((unsigned)png.size())] ^= (unsigned char)(1u << rng.u(8));
  } else if(kind == 1) {
    png.resize(rng.u((unsigned)png.size())); /*truncate*/
  } else if(kind == 2) {
    int n = 1 + (int)rng.u(6);
    for(int i = 0; i < n; ++i) png[rng.u((unsigned)png.size())] = (unsigned char)rng.u(256);
  } else {
    /*corrupt a chunk length field somewhere*/
    size_t pos = 33;
    if(png.size() > 37) {
      size_t off = 33 + rng.u((unsigned)(png.size() - 37));
      png[off] = (unsigned char)rng.u(256);
    }
    (void)pos;
  }
}

static void run_mutation_differential(int count) {
  Rng rng(0xBAD5EEDull);
  for(int i = 0; i < count; ++i) {
    LodePNGColorMode mode;
    unsigned w, h;
    std::vector<unsigned char> png = encode_random_png(rng, &mode, &w, &h);
    int nmut = 1 + (int)rng.u(3);
    for(int m = 0; m < nmut; ++m) mutate(rng, png);
    DecodeSpec spec = random_decode_spec(rng, (int)mode.colortype, (int)mode.bitdepth);
    if(rng.u(100) < 10) spec.ignore_end = 1;
    run_both(spec, png, "mutation");
    lodepng_color_mode_cleanup(&mode);
  }
  std::printf("mutation differential: %d corrupted PNGs done\n", count);
}

/* ------------------------------------------------------------------ */
/* 3) targeted chunk tests: every ancillary chunk type, valid and      */
/* malformed, plus unknown-chunk and structural edge cases             */
/* ------------------------------------------------------------------ */
static std::vector<unsigned char> zlib_compress(const std::vector<unsigned char>& in) {
  unsigned char* out = 0;
  size_t outsize = 0;
  unsigned e = lodepng_zlib_compress(&out, &outsize, in.empty() ? 0 : &in[0], in.size(),
                                     &lodepng_default_compress_settings);
  if(e) { fprintf(stderr, "zlib compress failed\n"); exit(2); }
  std::vector<unsigned char> v(out, out + outsize);
  free(out);
  return v;
}

static std::vector<unsigned char> make_ancillary(const char* type, int ct, int bd) {
  std::vector<unsigned char> d;
  std::string t = std::string(type);
  if(t == "tEXt") {
    const char s[] = "Author\0differential test";
    d.assign(s, s + sizeof(s) - 1);
  } else if(t == "zTXt") {
    std::vector<unsigned char> raw;
    const char* s = "Comment\0"; /*key + null + compression method 0*/
    d.assign(s, s + 8);
    d.push_back(0);
    std::vector<unsigned char> text(1, 'H'); text.push_back('i');
    std::vector<unsigned char> comp = zlib_compress(std::vector<unsigned char>(text.begin(), text.end()));
    d.insert(d.end(), comp.begin(), comp.end());
  } else if(t == "iTXt") {
    const char s[] = "Comment\0\0\0en\0Kommentar\0Hallo Welt";
    d.assign(s, s + sizeof(s) - 1);
  } else if(t == "iCCP") {
    const char* s = "prof\0";
    d.assign(s, s + 5);
    d.push_back(0);
    std::vector<unsigned char> comp = zlib_compress(std::vector<unsigned char>(64, 7));
    d.insert(d.end(), comp.begin(), comp.end());
  } else if(t == "eXIf") {
    for(int i = 0; i < 16; ++i) d.push_back((unsigned char)(i * 3));
  } else if(t == "cICP") {
    d.push_back(9); d.push_back(16); d.push_back(0); d.push_back(1);
  } else if(t == "mDCV") {
    for(int i = 0; i < 24; ++i) d.push_back((unsigned char)i);
  } else if(t == "cLLI") {
    for(int i = 0; i < 8; ++i) d.push_back((unsigned char)(255 - i));
  } else if(t == "sBIT") {
    int n = (ct == 0) ? 1 : (ct == 2 || ct == 3) ? 3 : (ct == 4) ? 2 : 4;
    for(int i = 0; i < n; ++i) d.push_back((unsigned char)(bd < 8 ? bd : 8));
  } else if(t == "bKGD") {
    if(ct == 3) d.push_back(0);
    else if(ct == 0 || ct == 4) { d.push_back(0); d.push_back(1); }
    else { for(int i = 0; i < 6; ++i) d.push_back((unsigned char)i); }
  } else if(t == "gAMA") {
    d.push_back(0); d.push_back(0); d.push_back(0xB1); d.push_back(0x8F);
  } else if(t == "cHRM") {
    for(int i = 0; i < 32; ++i) d.push_back((unsigned char)i);
  } else if(t == "sRGB") {
    d.push_back(0);
  } else if(t == "pHYs") {
    d.push_back(0); d.push_back(0); d.push_back(0x0B); d.push_back(0x13);
    d.push_back(0); d.push_back(0); d.push_back(0x0B); d.push_back(0x13);
    d.push_back(1);
  } else if(t == "tIME") {
    d.push_back(0x07); d.push_back(0xEA); d.push_back(1); d.push_back(2);
    d.push_back(3); d.push_back(4); d.push_back(5);
  } else if(t == "vpAg" || t == "ABCD" || t == "abCd" || t == "ab{d") {
    d.push_back(1); d.push_back(2); d.push_back(3);
  }
  return make_chunk(type, d);
}

static void run_targeted_chunk_tests(void) {
  Rng rng(0xC0FFEEull);
  static const char* kTypes[] = {
    "tEXt", "zTXt", "iTXt", "iCCP", "eXIf", "cICP", "mDCV", "cLLI",
    "sBIT", "bKGD", "gAMA", "cHRM", "sRGB", "pHYs", "tIME",
    "vpAg", /*unknown ancillary: remembered*/
    "ABCD", /*unknown critical: error 69 unless ignore_critical*/
    "abCd", /*invalid: reserved bit set -> 122*/
    "ab{d"  /*invalid: bad character in type name -> 121*/
  };
  static const int kBase[][2] = {{2,8},{3,8},{0,16},{6,8}};
  for(size_t t = 0; t < sizeof(kTypes) / sizeof(kTypes[0]); ++t) {
    for(size_t b = 0; b < sizeof(kBase) / sizeof(kBase[0]); ++b) {
      int ct = kBase[b][0], bd = kBase[b][1];
      LodePNGColorMode mode;
      init_random_mode(rng, &mode, ct, bd, true);
      LodePNGState st;
      lodepng_state_init(&st);
      lodepng_color_mode_copy(&st.info_png.color, &mode);
      lodepng_color_mode_copy(&st.info_raw, &mode);
      size_t rawsize = raw_size_bytes(ct, bd, 16, 16);
      std::vector<unsigned char> raw(rawsize);
      for(size_t i = 0; i < rawsize; ++i) raw[i] = (unsigned char)rng.u(256);
      unsigned char* png = 0;
      size_t pngsize = 0;
      unsigned e = lodepng_encode(&png, &pngsize, &raw[0], 16, 16, &st);
      lodepng_state_cleanup(&st);
      if(e) { fprintf(stderr, "encode failed\n"); exit(2); }
      std::vector<unsigned char> v(png, png + pngsize);
      free(png);

      std::vector<unsigned char> chunk = make_ancillary(kTypes[t], ct, bd);
      bool after_ihdr = (t % 2 == 0); /*exercise both unknown_chunks slots*/

      /*variant 1: valid chunk, all readers enabled*/
      {
        std::vector<unsigned char> p = v;
        inject_chunk(p, chunk, after_ihdr);
        DecodeSpec spec = {6, 8, 1, 1, 1, 0, 0};
        run_both(spec, p, kTypes[t]);
      }
      /*variant 2: corrupted CRC (flip a data byte inside the chunk)*/
      {
        std::vector<unsigned char> p = v;
        std::vector<unsigned char> bad = chunk;
        if(bad.size() > 12) bad[9] ^= 0xFF;
        inject_chunk(p, bad, after_ihdr);
        DecodeSpec spec = {6, 8, 1, 1, 1, 0, 0};
        run_both(spec, p, "crc");
        /*same but ignoring CRC*/
        spec.ignore_crc = 1;
        run_both(spec, p, "crc-ignored");
      }
      /*variant 3: ignore_critical for the unknown-critical chunk*/
      if(std::string(kTypes[t]) == "ABCD") {
        std::vector<unsigned char> p = v;
        inject_chunk(p, chunk, after_ihdr);
        /*decode with ignore_critical through the state API*/
        DecodeResult o, n;
        {
          OldPNGState os; oldpng_state_init(&os);
          os.info_raw.colortype = OLDLCT_RGBA; os.info_raw.bitdepth = 8;
          os.decoder.ignore_critical = 1;
          os.decoder.remember_unknown_chunks = 1;
          unsigned char* out = 0;
          o.error = oldpng_decode(&out, &o.w, &o.h, &os, &p[0], p.size());
          if(o.error == 0 && out) o.pixels.assign(out, out + raw_size_bytes(6, 8, o.w, o.h));
          if(out) free(out);
          o.raw_colortype = (int)os.info_raw.colortype;
          o.raw_bitdepth = (int)os.info_raw.bitdepth;
          for(int i = 0; i < 3; ++i) o.unknown_sizes[i] = os.info_png.unknown_chunks_size[i];
          oldpng_state_cleanup(&os);
        }
        {
          LodePNGState ns; lodepng_state_init(&ns);
          ns.info_raw.colortype = LCT_RGBA; ns.info_raw.bitdepth = 8;
          ns.decoder.ignore_critical = 1;
          ns.decoder.remember_unknown_chunks = 1;
          unsigned char* out = 0;
          n.error = lodepng_decode(&out, &n.w, &n.h, &ns, &p[0], p.size());
          if(n.error == 0 && out) n.pixels.assign(out, out + raw_size_bytes(6, 8, n.w, n.h));
          if(out) free(out);
          n.raw_colortype = (int)ns.info_raw.colortype;
          n.raw_bitdepth = (int)ns.info_raw.bitdepth;
          for(int i = 0; i < 3; ++i) n.unknown_sizes[i] = ns.info_png.unknown_chunks_size[i];
          lodepng_state_cleanup(&ns);
        }
        compare_pair(o, n, "ignore_critical", p);
      }
      lodepng_color_mode_cleanup(&mode);
    }
  }
  /*structural: truncated IEND (error 30) and ignore_end*/
  {
    LodePNGColorMode mode;
    init_random_mode(rng, &mode, 6, 8, false);
    LodePNGState st;
    lodepng_state_init(&st);
    lodepng_color_mode_copy(&st.info_png.color, &mode);
    lodepng_color_mode_copy(&st.info_raw, &mode);
    std::vector<unsigned char> raw(16 * 16 * 4, 200);
    unsigned char* png = 0;
    size_t pngsize = 0;
    lodepng_encode(&png, &pngsize, &raw[0], 16, 16, &st);
    lodepng_state_cleanup(&st);
    std::vector<unsigned char> v(png, png + pngsize);
    free(png);
    v.resize(v.size() - 12); /*drop IEND*/
    DecodeSpec spec = {6, 8, 1, 1, 1, 0, 0};
    run_both(spec, v, "no-IEND");
    spec.ignore_end = 1;
    run_both(spec, v, "no-IEND-ignore-end");
    lodepng_color_mode_cleanup(&mode);
  }
  std::printf("targeted chunk tests done\n");
}

/* ------------------------------------------------------------------ */
/* 4) eight sample files on disk: byte-exact output comparison         */
/* ------------------------------------------------------------------ */
static std::vector<unsigned char> pattern_raw(int ct, int bd, unsigned w, unsigned h, int variant) {
  /*per-pixel sample values, then packed to lodepng raw format*/
  unsigned channels = (ct == 0 || ct == 3) ? 1 : (ct == 2) ? 3 : (ct == 4) ? 2 : 4;
  unsigned maxv = (bd == 16) ? 65535 : (bd == 8) ? 255 : (1u << bd) - 1;
  std::vector<unsigned> vals;
  for(unsigned y = 0; y < h; ++y) {
    for(unsigned x = 0; x < w; ++x) {
      for(unsigned c = 0; c < channels; ++c) {
        unsigned v = (x * (3 + variant) + y * 5 + c * 40 + x * y / 7 + variant * 11) & maxv;
        vals.push_back(v);
      }
    }
  }
  if(bd == 8 || bd == 16) {
    std::vector<unsigned char> out;
    for(size_t i = 0; i < vals.size(); ++i) {
      if(bd == 16) out.push_back((unsigned char)(vals[i] >> 8));
      out.push_back((unsigned char)(vals[i] & 255));
    }
    return out;
  }
  /*sub-8-bit: pack MSB first*/
  std::vector<unsigned char> out((vals.size() * bd + 7) / 8, 0);
  for(size_t i = 0; i < vals.size(); ++i) {
    for(int b = 0; b < bd; ++b) {
      unsigned bit = (vals[i] >> (bd - 1 - b)) & 1u;
      size_t p = i * bd + b;
      out[p / 8] |= (unsigned char)(bit << (7 - (p % 8)));
    }
  }
  return out;
}

static void run_sample_tests(void) {
  struct Sample { int ct, bd, interlace, trns; unsigned w, h; const char* name; };
  static const Sample kSamples[] = {
    {0, 1, 0, 0, 37, 23, "sample1_grey1"},
    {3, 2, 0, 1, 64, 48, "sample2_palette2_trns"},
    {3, 4, 1, 1, 31, 17, "sample3_palette4_interlace"},
    {4, 8, 0, 0, 50, 50, "sample4_greya8"},
    {2, 8, 0, 1, 80, 60, "sample5_rgb8_trnskey"},
    {2, 16, 1, 0, 45, 33, "sample6_rgb16_interlace"},
    {6, 16, 0, 0, 40, 40, "sample7_rgba16"},
    {6, 8, 1, 0, 128, 96, "sample8_rgba8_interlace"}
  };
  int idx = 0;
  for(size_t s = 0; s < sizeof(kSamples) / sizeof(kSamples[0]); ++s) {
    const Sample& sp = kSamples[s];
    ++idx;
    LodePNGState st;
    lodepng_state_init(&st);
    st.info_png.color.colortype = (LodePNGColorType)sp.ct;
    st.info_png.color.bitdepth = (unsigned)sp.bd;
    st.info_raw.colortype = (LodePNGColorType)sp.ct;
    st.info_raw.bitdepth = (unsigned)sp.bd;
    st.info_png.interlace_method = sp.interlace;
    if(sp.ct == LCT_PALETTE) {
      unsigned n = 1u << sp.bd;
      for(unsigned i = 0; i < n; ++i) {
        unsigned char a = (sp.trns && i % 3 == 0) ? (unsigned char)(i * 40) : 255;
        lodepng_palette_add(&st.info_png.color, (unsigned char)(i * 37), (unsigned char)(255 - i * 13),
                            (unsigned char)(i * 91), a);
        lodepng_palette_add(&st.info_raw, (unsigned char)(i * 37), (unsigned char)(255 - i * 13),
                            (unsigned char)(i * 91), a);
      }
    }
    if(sp.trns && (sp.ct == LCT_GREY || sp.ct == LCT_RGB)) {
      st.info_png.color.key_defined = 1;
      st.info_png.color.key_r = 100; st.info_png.color.key_g = 150; st.info_png.color.key_b = 200;
    }
    std::vector<unsigned char> raw = pattern_raw(sp.ct, sp.bd, sp.w, sp.h, idx);
    unsigned char* png = 0;
    size_t pngsize = 0;
    unsigned e = lodepng_encode(&png, &pngsize, &raw[0], sp.w, sp.h, &st);
    lodepng_state_cleanup(&st);
    if(e) { fprintf(stderr, "sample encode failed: %u\n", e); exit(2); }
    std::vector<unsigned char> v(png, png + pngsize);
    free(png);

    char path[256];
    std::sprintf(path, "../artifacts/samples/%s.png", sp.name);
    save_file(path, v);

    /*byte-exact comparison on two decode targets: RGBA8 and native (color_convert=0)*/
    DecodeSpec spec_rgba = {6, 8, 1, 1, 1, 0, 0};
    DecodeSpec spec_native = {sp.ct, sp.bd, 0, 1, 1, 0, 0};
    run_both(spec_rgba, v, sp.name);
    run_both(spec_native, v, sp.name);
    std::printf("sample %d %-28s %5zu bytes  w=%-3u h=%-3u  byte-compare ok\n",
                idx, sp.name, v.size(), sp.w, sp.h);
  }
}

int main(void) {
  run_random_differential(10000);
  run_mutation_differential(20000);
  run_targeted_chunk_tests();
  run_sample_tests();
  std::printf("----\n%ld decode pairs compared, %d mismatches\n", g_checked, g_failures);
  if(g_failures) {
    std::printf("RESULT: FAIL\n");
    return 1;
  }
  std::printf("RESULT: ALL IDENTICAL\n");
  return 0;
}

/*
sBIT extension test / demo (not part of the repository build; compile manually:
  g++ -I . -O2 -o sbit_test sbit_test.cpp lodepng.cpp

Scenario: a scanner produces samples with fewer significant bits than the PNG
bit depth (e.g. 10-bit data stored in a 16-bit PNG). This program creates four
small sample groups (8-bit and 16-bit, each with and without an sBIT chunk),
and for each group reports: read-back values, rescaling error, round-trip byte
consistency and timing. It also checks that significant-bits > bit depth is
rejected, and that default decoding is pixel-by-pixel unchanged.
*/

#include "lodepng.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* sBIT scaling extension: implemented in lodepng.cpp, intentionally not
   declared in lodepng.h so the public interface is unchanged. */
unsigned lodepng_sbit_effective(const LodePNGInfo* info, const LodePNGColorMode* mode, unsigned channel);
unsigned lodepng_sbit_scale_to_full_range(unsigned char* image, unsigned w, unsigned h,
                                          const LodePNGColorMode* mode, const LodePNGInfo* info);

static int failures = 0;
#define CHECK(cond) do { \
  if(!(cond)) { ++failures; std::printf("  FAIL: %s (line %d)\n", #cond, __LINE__); } \
} while(0)

static double now_us() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
}

/* Deterministic scan-like samples in [0, 2^sbits - 1]. */
static std::vector<unsigned char> make_samples(unsigned w, unsigned h, unsigned bitdepth, unsigned sbits) {
  unsigned smax = (1u << sbits) - 1u;
  std::vector<unsigned char> img((size_t)w * h * (bitdepth / 8));
  for(unsigned y = 0; y < h; ++y) {
    for(unsigned x = 0; x < w; ++x) {
      unsigned v = (x * 7u + y * 13u + ((x * y) ^ (x << 3)) * 3u) & smax;
      size_t i = (size_t)y * w + x;
      if(bitdepth == 16) { img[2 * i] = (unsigned char)(v >> 8); img[2 * i + 1] = (unsigned char)(v & 255u); }
      else img[i] = (unsigned char)v;
    }
  }
  return img;
}

static unsigned get_sample(const std::vector<unsigned char>& img, size_t i, unsigned bitdepth) {
  if(bitdepth == 16) return 256u * img[2 * i] + img[2 * i + 1];
  return img[i];
}

static void print_samples(const char* label, const std::vector<unsigned char>& img, unsigned bitdepth, size_t n) {
  std::printf("%s", label);
  for(size_t i = 0; i < n; ++i) std::printf(" %u", get_sample(img, i, bitdepth));
  std::printf("\n");
}

/* Encode grey image; if sbits != 0 an sBIT chunk with that value is written. */
static unsigned encode_png(std::vector<unsigned char>& png, const std::vector<unsigned char>& img,
                           unsigned w, unsigned h, unsigned bitdepth, unsigned sbits) {
  LodePNGState state;
  lodepng_state_init(&state);
  state.encoder.auto_convert = 0;
  lodepng_color_mode_init(&state.info_raw);
  state.info_raw.colortype = LCT_GREY;
  state.info_raw.bitdepth = bitdepth;
  state.info_png.color.colortype = LCT_GREY;
  state.info_png.color.bitdepth = bitdepth;
  if(sbits) {
    state.info_png.sbit_defined = 1;
    state.info_png.sbit_r = state.info_png.sbit_g = state.info_png.sbit_b = sbits;
  }
  size_t size = 0;
  unsigned char* out = 0;
  unsigned error = lodepng_encode(&out, &size, img.data(), w, h, &state);
  if(!error) png.assign(out, out + size);
  free(out);
  lodepng_state_cleanup(&state);
  return error;
}

static unsigned decode_png(std::vector<unsigned char>& img, unsigned* w, unsigned* h,
                           LodePNGState* state, const std::vector<unsigned char>& png) {
  unsigned char* out = 0;
  unsigned error = lodepng_decode(&out, w, h, state, png.data(), png.size());
  if(!error) img.assign(out, out + (size_t)(*w) * (*h) * (state->info_raw.bitdepth / 8));
  free(out);
  return error;
}

static bool png_has_chunk(const std::vector<unsigned char>& png, const char* type) {
  for(size_t i = 8; i + 8 <= png.size(); ) {
    size_t len = 256u * 256u * 256u * png[i] + 256u * 256u * png[i + 1] + 256u * png[i + 2] + png[i + 3];
    if(std::memcmp(&png[i + 4], type, 4) == 0) return true;
    i += 12 + len;
  }
  return false;
}

static void run_group(const char* name, unsigned bitdepth, unsigned sbits /*0 = no sBIT chunk*/) {
  const unsigned w = 256, h = 192;
  const unsigned data_sbits = sbits ? sbits : (bitdepth == 16 ? 10 : 6); /*simulated scanner bits*/
  std::printf("== %s: %u-bit grey, %s (data has %u significant bits) ==\n", name, bitdepth,
              sbits ? "with sBIT" : "without sBIT", data_sbits);

  std::vector<unsigned char> raw = make_samples(w, h, bitdepth, data_sbits);
  std::vector<unsigned char> png;
  CHECK(encode_png(png, raw, w, h, bitdepth, sbits) == 0);
  CHECK(png_has_chunk(png, "sBIT") == (sbits != 0));
  std::printf("  png size: %lu bytes\n", (unsigned long)png.size());

  /* decode (color conversion off: buffer keeps the PNG color mode) */
  std::vector<unsigned char> dec;
  unsigned w2 = 0, h2 = 0;
  LodePNGState state;
  lodepng_state_init(&state);
  state.decoder.color_convert = 0;
  double t0 = now_us();
  const int reps = 50;
  unsigned error = 0;
  for(int r = 0; r < reps; ++r) error = decode_png(dec, &w2, &h2, &state, png);
  double t_dec = (now_us() - t0) / reps;
  CHECK(error == 0);
  CHECK(w2 == w && h2 == h);
  /* default decoding must be pixel-by-pixel identical to what was stored */
  CHECK(dec == raw);
  std::printf("  decode ok, sbit_defined=%u sbit_r=%u\n", state.info_png.sbit_defined, state.info_png.sbit_r);
  print_samples("  read-back :", dec, bitdepth, 8);

  /* effective significant bits: from the chunk, or inferred from the bit depth */
  unsigned eff = lodepng_sbit_effective(&state.info_png, &state.info_raw, 0);
  std::printf("  effective significant bits: %u (%s)\n", eff, sbits ? "from sBIT chunk" : "inferred from bit depth");
  CHECK(eff == (sbits ? sbits : bitdepth));

  /* rescale samples to the full range */
  std::vector<unsigned char> scaled = dec;
  t0 = now_us();
  for(int r = 0; r < reps; ++r) {
    scaled = dec;
    error = lodepng_sbit_scale_to_full_range(scaled.data(), w2, h2, &state.info_raw, &state.info_png);
  }
  double t_scale = (now_us() - t0) / reps;
  CHECK(error == 0);
  print_samples("  scaled    :", scaled, bitdepth, 8);

  /* rescaling error vs the ideal real-valued mapping v * max_out / max_in */
  unsigned maxv = (1u << bitdepth) - 1u, smax = (1u << eff) - 1u;
  double maxerr = 0, sumerr = 0;
  size_t n = (size_t)w2 * h2;
  for(size_t i = 0; i < n; ++i) {
    double ideal = get_sample(dec, i, bitdepth) * (double)maxv / smax;
    double err = std::fabs(get_sample(scaled, i, bitdepth) - ideal);
    if(err > maxerr) maxerr = err;
    sumerr += err;
  }
  std::printf("  rescale error: max %.4f, mean %.6f (full-range lsb = 1/%u)\n", maxerr, sumerr / n, maxv);
  CHECK(maxerr <= 0.5);
  if(!sbits) CHECK(scaled == dec); /*inferred sbit == bitdepth: no-op*/

  /* round trip: decode -> re-encode -> identical PNG bytes -> decode -> identical pixels */
  std::vector<unsigned char> png2, dec2;
  t0 = now_us();
  for(int r = 0; r < reps; ++r) CHECK(encode_png(png2, dec, w2, h2, bitdepth, sbits) == 0);
  double t_enc = (now_us() - t0) / reps;
  CHECK(png2 == png);
  unsigned w3, h3;
  LodePNGState state2;
  lodepng_state_init(&state2);
  state2.decoder.color_convert = 0;
  CHECK(decode_png(dec2, &w3, &h3, &state2, png2) == 0);
  CHECK(dec2 == dec);
  std::printf("  round trip: png bytes %s, pixels %s\n",
              png2 == png ? "identical" : "DIFFER", dec2 == dec ? "identical" : "DIFFER");
  std::printf("  timing: decode %.1f us, rescale %.1f us, encode %.1f us\n", t_dec, t_scale, t_enc);

  lodepng_state_cleanup(&state);
  lodepng_state_cleanup(&state2);
  std::printf("\n");
}

static void run_error_cases(const std::vector<unsigned char>& png8_sbit) {
  std::printf("== error cases: significant bits > bit depth must be rejected ==\n");
  std::vector<unsigned char> img8 = make_samples(16, 16, 8, 6);
  std::vector<unsigned char> img16 = make_samples(16, 16, 16, 10);
  std::vector<unsigned char> png;
  unsigned e1 = encode_png(png, img8, 16, 16, 8, 12);   /*12 significant bits in an 8-bit PNG*/
  unsigned e2 = encode_png(png, img16, 16, 16, 16, 17); /*17 significant bits in a 16-bit PNG*/
  std::printf("  encode sbit=12 in  8-bit -> error %u (%s)\n", e1, lodepng_error_text(e1));
  std::printf("  encode sbit=17 in 16-bit -> error %u (%s)\n", e2, lodepng_error_text(e2));
  CHECK(e1 == 115 && e2 == 115);

  /* corrupt the sBIT value of a valid 8-bit file to 12 and fix its CRC */
  std::vector<unsigned char> bad = png8_sbit;
  size_t pos = 0;
  for(size_t i = 8; i + 8 <= bad.size(); ) {
    size_t len = 256u * 256u * 256u * bad[i] + 256u * 256u * bad[i + 1] + 256u * bad[i + 2] + bad[i + 3];
    if(std::memcmp(&bad[i + 4], "sBIT", 4) == 0) { pos = i; break; }
    i += 12 + len;
  }
  CHECK(pos != 0);
  bad[pos + 8] = 12; /*sBIT data byte: 12 > 8*/
  unsigned crc = lodepng_crc32(&bad[pos + 4], 4 + 1);
  bad[pos + 9] = (unsigned char)((crc >> 24) & 255u);
  bad[pos + 10] = (unsigned char)((crc >> 16) & 255u);
  bad[pos + 11] = (unsigned char)((crc >> 8) & 255u);
  bad[pos + 12] = (unsigned char)(crc & 255u);
  std::vector<unsigned char> dec;
  unsigned w, h;
  LodePNGState state;
  lodepng_state_init(&state);
  state.decoder.color_convert = 0;
  unsigned e3 = decode_png(dec, &w, &h, &state, bad);
  std::printf("  decode sbit=12 in  8-bit -> error %u (%s)\n", e3, lodepng_error_text(e3));
  CHECK(e3 == 115);

  /* scaler API rejects it too */
  LodePNGInfo info;
  lodepng_info_init(&info);
  info.sbit_defined = 1;
  info.sbit_r = info.sbit_g = info.sbit_b = 12;
  LodePNGColorMode mode;
  lodepng_color_mode_init(&mode);
  mode.colortype = LCT_GREY;
  mode.bitdepth = 8;
  unsigned e4 = lodepng_sbit_scale_to_full_range(img8.data(), 16, 16, &mode, &info);
  std::printf("  rescale sbit=12 in  8-bit -> error %u (%s)\n", e4, lodepng_error_text(e4));
  CHECK(e4 == 115);
  lodepng_info_cleanup(&info);
  lodepng_state_cleanup(&state);
  std::printf("\n");
}

int main() {
  std::printf("sBIT extension test\n\n");
  std::vector<unsigned char> png8_sbit;
  {
    std::vector<unsigned char> img8 = make_samples(256, 192, 8, 6);
    CHECK(encode_png(png8_sbit, img8, 256, 192, 8, 6) == 0);
  }
  run_group("group 1", 16, 10); /*16-bit with sBIT=10 (the scanner case)*/
  run_group("group 2", 16, 0);  /*16-bit without sBIT*/
  run_group("group 3", 8, 6);   /*8-bit with sBIT=6*/
  run_group("group 4", 8, 0);   /*8-bit without sBIT*/
  run_error_cases(png8_sbit);
  std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}

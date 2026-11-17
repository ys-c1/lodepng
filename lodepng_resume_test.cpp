/*
lodepng_resume_test.cpp

Test for the resumable PNG decode sessions (decode state snapshots) implemented in
lodepng.cpp. This file is an added test; it does not modify the library's public
interface: the resumable decode API is defined in lodepng.cpp and declared here.

Covers:
- four image classes: small, large, interlaced (Adam7) and alpha, plus a sub-byte
  (4-bit palette) image to cover the padding-bit path
- pixel-exact equality between one-shot lodepng_decode_memory and resumed decodes
  (including a run that serializes and restores at every checkpoint)
- snapshot sizes (configurable via window size) and checkpoint interval configuration
- resume timing versus one-shot decode timing
- corrupted / mismatched snapshots must produce errors, never a wrong image

Build and run with run_resume_test.sh (does not use the Makefile).
*/

#include "lodepng.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

/*declarations of the resumable decode API (defined in lodepng.cpp, not in lodepng.h)*/

typedef struct LodePNGDecodeSession LodePNGDecodeSession;
LodePNGDecodeSession* lodepng_session_begin(const unsigned char* in, size_t insize,
                                            unsigned interval_rows, size_t window_size,
                                            unsigned* error);
unsigned lodepng_session_step(LodePNGDecodeSession* session);
int lodepng_session_is_paused(const LodePNGDecodeSession* session);
int lodepng_session_is_finished(const LodePNGDecodeSession* session);
unsigned lodepng_session_snapshot(const LodePNGDecodeSession* session,
                                  unsigned char** out, size_t* outsize);
LodePNGDecodeSession* lodepng_session_restore(const unsigned char* in, size_t insize,
                                              const unsigned char* snapshot, size_t snapshot_size,
                                              const unsigned char* partial, size_t partial_size,
                                              unsigned* error);
const unsigned char* lodepng_session_partial(const LodePNGDecodeSession* session, size_t* size);
unsigned lodepng_session_output(LodePNGDecodeSession* session,
                                unsigned char** out, unsigned* w, unsigned* h);
void lodepng_session_destroy(LodePNGDecodeSession* session);

static int g_failures = 0;

#define CHECK(cond, msg) do { \
  if(!(cond)) { \
    std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
    g_failures++; \
  } \
} while(0)

static double now_ms() {
  return 1000.0 * (double)std::clock() / (double)CLOCKS_PER_SEC;
}

/*deterministic test image with smooth areas, texture and noise so that all PNG
filter types occur and the deflate stream contains back references*/
static void gen_image(std::vector<unsigned char>& img, unsigned w, unsigned h,
                      unsigned channels, bool vary_alpha) {
  unsigned seed = 123456789u;
  img.resize((size_t)w * h * channels);
  for(unsigned y = 0; y < h; ++y) {
    for(unsigned x = 0; x < w; ++x) {
      seed = seed * 1103515245u + 12345u;
      unsigned n = (seed >> 16) & 255u;
      size_t i = ((size_t)y * w + x) * channels;
      img[i + 0] = (unsigned char)(((x / 8) * 32 + y / 4 + n / 8) & 255u);
      if(channels >= 3) {
        img[i + 1] = (unsigned char)(((y / 8) * 32 + x / 4 + n / 4) & 255u);
        img[i + 2] = (unsigned char)((((x + y) / 16) * 64 + n / 2) & 255u);
      }
      if(channels == 4) {
        img[i + 3] = (unsigned char)(vary_alpha ? ((x * y / 64 + n) & 255u) : 255u);
      }
    }
  }
}

static bool encode_png(std::vector<unsigned char>& png, const unsigned char* img,
                       unsigned w, unsigned h, LodePNGColorType ct, unsigned bd,
                       bool interlace, bool palette) {
  LodePNGState state;
  lodepng_state_init(&state);
  state.info_raw.colortype = ct;
  state.info_raw.bitdepth = bd;
  state.info_png.color.colortype = ct;
  state.info_png.color.bitdepth = bd;
  state.info_png.interlace_method = interlace ? 1 : 0;
  state.encoder.auto_convert = 0; /*keep exactly the requested color type*/
  if(palette) {
    for(unsigned i = 0; i < 16; ++i) {
      lodepng_palette_add(&state.info_png.color, i * 16, 255 - i * 16, (i * 37) & 255, 255);
      lodepng_palette_add(&state.info_raw, i * 16, 255 - i * 16, (i * 37) & 255, 255);
    }
  }
  unsigned char* buf = 0;
  size_t bufsize = 0;
  unsigned error = lodepng_encode(&buf, &bufsize, img, w, h, &state);
  lodepng_state_cleanup(&state);
  if(error) {
    std::printf("encode error %u: %s\n", error, lodepng_error_text(error));
    return false;
  }
  png.assign(buf, buf + bufsize);
  std::free(buf);
  return true;
}

/*decode with the resumable session API. If restore_each, every checkpoint is
serialized, the session destroyed, and restored from the snapshot + partial output.*/
static bool resume_decode(std::vector<unsigned char>& out, unsigned* w, unsigned* h,
                          const std::vector<unsigned char>& png,
                          unsigned interval, size_t window_size, bool restore_each,
                          unsigned* num_checkpoints, size_t* max_snapshot) {
  unsigned error = 0;
  *num_checkpoints = 0;
  *max_snapshot = 0;
  LodePNGDecodeSession* s = lodepng_session_begin(&png[0], png.size(), interval, window_size, &error);
  if(!s) {
    std::printf("session begin error %u: %s\n", error, lodepng_error_text(error));
    return false;
  }
  for(;;) {
    error = lodepng_session_step(s);
    if(error) {
      std::printf("session step error %u: %s\n", error, lodepng_error_text(error));
      lodepng_session_destroy(s);
      return false;
    }
    if(lodepng_session_is_finished(s)) break;
    if(lodepng_session_is_paused(s)) {
      unsigned char* snap = 0;
      size_t snapsize = 0;
      error = lodepng_session_snapshot(s, &snap, &snapsize);
      if(error) {
        std::printf("snapshot error %u: %s\n", error, lodepng_error_text(error));
        lodepng_session_destroy(s);
        return false;
      }
      (*num_checkpoints)++;
      if(snapsize > *max_snapshot) *max_snapshot = snapsize;
      if(restore_each) {
        size_t psize = 0;
        const unsigned char* partial = lodepng_session_partial(s, &psize);
        std::vector<unsigned char> pcopy(partial, partial + psize);
        lodepng_session_destroy(s);
        s = lodepng_session_restore(&png[0], png.size(), snap, snapsize,
                                    psize ? &pcopy[0] : 0, psize, &error);
        if(!s) {
          std::printf("restore error %u: %s\n", error, lodepng_error_text(error));
          std::free(snap);
          return false;
        }
      }
      std::free(snap);
    }
  }
  unsigned char* o = 0;
  error = lodepng_session_output(s, &o, w, h);
  lodepng_session_destroy(s);
  if(error) {
    std::printf("session output error %u: %s\n", error, lodepng_error_text(error));
    return false;
  }
  out.assign(o, o + (size_t)(*w) * (*h) * 4);
  std::free(o);
  return true;
}

struct TestCase {
  std::string name;
  std::vector<unsigned char> png;
  unsigned interval;
  size_t window_size;
};

static void run_case(const TestCase& tc) {
  std::printf("=== %s (png size %lu bytes, interval %u rows, window %lu) ===\n",
              tc.name.c_str(), (unsigned long)tc.png.size(), tc.interval,
              (unsigned long)tc.window_size);

  /*one-shot reference decode with the unmodified original API*/
  double t0 = now_ms();
  unsigned char* ref = 0;
  unsigned w = 0, h = 0;
  unsigned error = lodepng_decode_memory(&ref, &w, &h, &tc.png[0], tc.png.size(), LCT_RGBA, 8);
  double t1 = now_ms();
  CHECK(error == 0, "one-shot decode failed");
  if(error) {
    std::printf("  decode error %u: %s\n", error, lodepng_error_text(error));
    return;
  }
  std::vector<unsigned char> refv(ref, ref + (size_t)w * h * 4);
  std::free(ref);
  double oneshot_ms = t1 - t0;

  /*resumable decode, serializing and restoring at every checkpoint*/
  std::vector<unsigned char> out;
  unsigned w2 = 0, h2 = 0, checkpoints = 0;
  size_t max_snap = 0;
  t0 = now_ms();
  bool ok = resume_decode(out, &w2, &h2, tc.png, tc.interval, tc.window_size,
                          true, &checkpoints, &max_snap);
  t1 = now_ms();
  double resume_total_ms = t1 - t0;
  CHECK(ok, "resumable decode with restore at every checkpoint failed");
  CHECK(w2 == w && h2 == h, "dimension mismatch");
  CHECK(ok && out == refv, "pixels differ: resumed (restore at each checkpoint) vs one-shot");
  bool identical = (out == refv);

  /*resumable decode without restore (in-session checkpoints only)*/
  std::vector<unsigned char> out2;
  unsigned checkpoints2 = 0;
  size_t max_snap2 = 0;
  ok = resume_decode(out2, &w2, &h2, tc.png, tc.interval, tc.window_size,
                     false, &checkpoints2, &max_snap2);
  CHECK(ok, "resumable decode without restore failed");
  CHECK(ok && out2 == refv, "pixels differ: resumed (in-session) vs one-shot");

  /*mid-resume timing: decode to the middle checkpoint, then measure finishing from the snapshot*/
  double mid_resume_ms = -1.0;
  size_t mid_rows = 0;
  {
    unsigned err2 = 0;
    LodePNGDecodeSession* s = lodepng_session_begin(&tc.png[0], tc.png.size(),
                                                    tc.interval, tc.window_size, &err2);
    CHECK(s != 0, "begin for mid-resume failed");
    if(s) {
      /*advance to roughly half of the checkpoints*/
      unsigned target_cp = checkpoints / 2;
      unsigned cp_count = 0;
      unsigned char* snap = 0;
      size_t snapsize = 0;
      std::vector<unsigned char> pcopy;
      for(;;) {
        err2 = lodepng_session_step(s);
        CHECK(err2 == 0, "step during mid-resume failed");
        if(lodepng_session_is_finished(s)) break;
        if(lodepng_session_is_paused(s)) {
          cp_count++;
          if(cp_count > target_cp) {
            err2 = lodepng_session_snapshot(s, &snap, &snapsize);
            CHECK(err2 == 0, "mid snapshot failed");
            size_t psize = 0;
            const unsigned char* partial = lodepng_session_partial(s, &psize);
            pcopy.assign(partial, partial + psize);
            break;
          }
        }
      }
      lodepng_session_destroy(s);
      if(snap) {
        double t2 = now_ms();
        LodePNGDecodeSession* s2 = lodepng_session_restore(&tc.png[0], tc.png.size(),
                                                           snap, snapsize,
                                                           pcopy.empty() ? 0 : &pcopy[0],
                                                           pcopy.size(), &err2);
        CHECK(s2 != 0, "mid-resume restore failed");
        if(s2) {
          for(;;) {
            err2 = lodepng_session_step(s2);
            CHECK(err2 == 0, "step after mid-resume failed");
            if(lodepng_session_is_finished(s2)) break;
            CHECK(lodepng_session_is_paused(s2), "neither finished nor paused");
          }
          unsigned char* o = 0;
          unsigned w3 = 0, h3 = 0;
          err2 = lodepng_session_output(s2, &o, &w3, &h3);
          CHECK(err2 == 0, "output after mid-resume failed");
          if(!err2) {
            std::vector<unsigned char> out3(o, o + (size_t)w3 * h3 * 4);
            CHECK(out3 == refv, "pixels differ: mid-resume vs one-shot");
            std::free(o);
          }
          lodepng_session_destroy(s2);
        }
        mid_resume_ms = now_ms() - t2;
        std::free(snap);
      }
    }
  }

  std::printf("  size %ux%u, raw RGBA %lu bytes\n", w, h, (unsigned long)((size_t)w * h * 4));
  std::printf("  checkpoints: %u, snapshot bytes (max): %lu\n",
              checkpoints, (unsigned long)max_snap);
  std::printf("  pixel-identical to one-shot: %s\n", identical ? "YES" : "NO");
  std::printf("  one-shot decode: %.2f ms, resumable total (restore each checkpoint): %.2f ms",
              oneshot_ms, resume_total_ms);
  if(mid_resume_ms >= 0) std::printf(", resume from mid snapshot: %.2f ms", mid_resume_ms);
  std::printf("\n\n");
  (void)mid_rows;
}

/*take a valid snapshot of the first checkpoint of the small image for corruption tests*/
static bool make_snapshot(const std::vector<unsigned char>& png, unsigned interval,
                          std::vector<unsigned char>* snap, std::vector<unsigned char>* partial) {
  unsigned error = 0;
  LodePNGDecodeSession* s = lodepng_session_begin(&png[0], png.size(), interval, 0, &error);
  if(!s) return false;
  for(;;) {
    error = lodepng_session_step(s);
    if(error || lodepng_session_is_finished(s)) break;
    if(lodepng_session_is_paused(s)) {
      unsigned char* sn = 0;
      size_t snsize = 0;
      error = lodepng_session_snapshot(s, &sn, &snsize);
      if(error) break;
      snap->assign(sn, sn + snsize);
      std::free(sn);
      size_t psize = 0;
      const unsigned char* p = lodepng_session_partial(s, &psize);
      partial->assign(p, p + psize);
      break;
    }
  }
  lodepng_session_destroy(s);
  return !snap->empty();
}

static void expect_restore_result(const char* name, const std::vector<unsigned char>& png,
                                  const std::vector<unsigned char>& snap,
                                  const std::vector<unsigned char>& partial,
                                  bool expect_ok, unsigned expect_code) {
  unsigned error = 0;
  LodePNGDecodeSession* s = lodepng_session_restore(&png[0], png.size(),
                                                    snap.empty() ? 0 : &snap[0], snap.size(),
                                                    partial.empty() ? 0 : &partial[0],
                                                    partial.size(), &error);
  if(expect_ok) {
    CHECK(s != 0 && error == 0, name);
  } else {
    if(s != 0 || error == 0) {
      std::printf("FAIL: %s: corrupted snapshot was accepted\n", name);
      g_failures++;
    } else if(expect_code && error != expect_code) {
      std::printf("FAIL: %s: expected error %u, got %u (%s)\n", name, expect_code, error,
                  lodepng_error_text(error));
      g_failures++;
    } else {
      std::printf("  ok: %s -> error %u (%s)\n", name, error, lodepng_error_text(error));
    }
  }
  if(s) lodepng_session_destroy(s);
}

static void run_corruption_tests(const std::vector<unsigned char>& png,
                                 const std::vector<unsigned char>& other_png) {
  std::printf("=== corrupted snapshot handling ===\n");
  std::vector<unsigned char> snap, partial;
  bool ok = make_snapshot(png, 7, &snap, &partial);
  CHECK(ok, "could not create a snapshot for corruption tests");
  if(!ok) return;
  std::printf("  snapshot size: %lu bytes\n", (unsigned long)snap.size());

  /*control: valid snapshot restores fine*/
  expect_restore_result("valid snapshot restores", png, snap, partial, true, 0);

  /*flip a byte in the header*/
  {
    std::vector<unsigned char> bad = snap;
    bad[20] ^= 0xFF;
    expect_restore_result("flipped header byte", png, bad, partial, false, 124);
  }
  /*flip a byte in the deflate window payload*/
  {
    std::vector<unsigned char> bad = snap;
    bad[snap.size() - 10] ^= 0xFF;
    expect_restore_result("flipped window byte", png, bad, partial, false, 124);
  }
  /*flip a byte in the checksum*/
  {
    std::vector<unsigned char> bad = snap;
    bad[snap.size() - 1] ^= 0xFF;
    expect_restore_result("flipped checksum byte", png, bad, partial, false, 124);
  }
  /*truncated snapshot*/
  {
    std::vector<unsigned char> bad(snap.begin(), snap.begin() + snap.size() / 2);
    expect_restore_result("truncated snapshot", png, bad, partial, false, 124);
  }
  /*snapshot of a different PNG*/
  expect_restore_result("snapshot does not match PNG", other_png, snap, partial, false, 125);
  /*partial output buffer of wrong size*/
  {
    std::vector<unsigned char> badpartial(partial.begin(), partial.end() - 1);
    expect_restore_result("wrong partial buffer size", png, snap, badpartial, false, 124);
  }
  /*corrupt the btype field to an impossible value (fix crc so only the value is wrong)*/
  {
    std::vector<unsigned char> bad = snap;
    bad[52] = 99; /*btype*/
    /*recompute crc32 over all but the last 4 bytes using lodepng's crc*/
    unsigned crc = lodepng_crc32(&bad[0], bad.size() - 4);
    size_t n = bad.size();
    bad[n - 4] = (unsigned char)(crc & 255u);
    bad[n - 3] = (unsigned char)((crc >> 8) & 255u);
    bad[n - 2] = (unsigned char)((crc >> 16) & 255u);
    bad[n - 1] = (unsigned char)((crc >> 24) & 255u);
    expect_restore_result("invalid btype value", png, bad, partial, false, 124);
  }
  std::printf("\n");
}

int main() {
  std::vector<TestCase> cases;

  /*small image: 64x48 RGBA*/
  {
    std::vector<unsigned char> img;
    gen_image(img, 64, 48, 4, true);
    TestCase tc;
    tc.name = "small 64x48 RGBA";
    tc.interval = 7;
    tc.window_size = 32768;
    if(!encode_png(tc.png, &img[0], 64, 48, LCT_RGBA, 8, false, false)) return 1;
    cases.push_back(tc);
  }
  /*large image: 1600x1200 RGB*/
  {
    std::vector<unsigned char> img;
    gen_image(img, 1600, 1200, 3, false);
    TestCase tc;
    tc.name = "large 1600x1200 RGB";
    tc.interval = 50;
    tc.window_size = 32768;
    if(!encode_png(tc.png, &img[0], 1600, 1200, LCT_RGB, 8, false, false)) return 1;
    cases.push_back(tc);
  }
  /*interlaced image: 517x389 RGB Adam7*/
  {
    std::vector<unsigned char> img;
    gen_image(img, 517, 389, 3, false);
    TestCase tc;
    tc.name = "interlaced 517x389 RGB Adam7";
    tc.interval = 30;
    tc.window_size = 32768;
    if(!encode_png(tc.png, &img[0], 517, 389, LCT_RGB, 8, true, false)) return 1;
    cases.push_back(tc);
  }
  /*alpha image: 400x300 RGBA with varied alpha, smaller window (smaller snapshot)*/
  {
    std::vector<unsigned char> img;
    gen_image(img, 400, 300, 4, true);
    TestCase tc;
    tc.name = "alpha 400x300 RGBA (4KB window)";
    tc.interval = 25;
    tc.window_size = 4096;
    if(!encode_png(tc.png, &img[0], 400, 300, LCT_RGBA, 8, false, false)) return 1;
    cases.push_back(tc);
  }
  /*sub-byte image: 101x77 4-bit palette (exercises padding bit removal)*/
  {
    unsigned w = 101, h = 77;
    std::vector<unsigned char> img(((size_t)w * 4 + 7) / 8 * h, 0);
    for(unsigned y = 0; y < h; ++y) {
      for(unsigned x = 0; x < w; ++x) {
        unsigned v = (x + y * 3) & 15u;
        size_t i = (size_t)y * ((w * 4 + 7) / 8) + x / 2;
        if(x % 2 == 0) img[i] |= (unsigned char)(v << 4);
        else img[i] |= (unsigned char)v;
      }
    }
    TestCase tc;
    tc.name = "palette 101x77 4-bit";
    tc.interval = 9;
    tc.window_size = 32768;
    if(!encode_png(tc.png, &img[0], w, h, LCT_PALETTE, 4, false, true)) return 1;
    cases.push_back(tc);
  }

  for(size_t i = 0; i < cases.size(); ++i) run_case(cases[i]);

  /*interval larger than the image: no checkpoints, straight decode*/
  {
    std::vector<unsigned char> out;
    unsigned w = 0, h = 0, checkpoints = 999;
    size_t max_snap = 0;
    bool ok = resume_decode(out, &w, &h, cases[0].png, 100000, 32768, true,
                            &checkpoints, &max_snap);
    CHECK(ok, "huge interval decode failed");
    CHECK(checkpoints == 0, "huge interval should produce no checkpoints");
    unsigned char* ref = 0;
    unsigned rw = 0, rh = 0;
    lodepng_decode_memory(&ref, &rw, &rh, &cases[0].png[0], cases[0].png.size(), LCT_RGBA, 8);
    std::vector<unsigned char> refv(ref, ref + (size_t)rw * rh * 4);
    std::free(ref);
    CHECK(out == refv, "huge interval pixels differ");
    std::printf("=== huge interval (no checkpoints): ok ===\n\n");
  }

  run_corruption_tests(cases[0].png, cases[1].png);

  if(g_failures == 0) {
    std::printf("ALL RESUME TESTS PASSED\n");
    return 0;
  }
  std::printf("%d FAILURES\n", g_failures);
  return 1;
}

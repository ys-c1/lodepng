/*
Timing comparison: non-interruptible one-shot decoding vs. an incremental
decode interrupted and resumed 100 times.

A single 512x512 RGBA8 PNG (dynamic huffman, the common case) is decoded
repeatedly, so the workload is identical across runs; only the interruption
pattern differs. Wall-clock work is measured with clock() (CPU time, no extra
libraries). The output checksums prove the interrupted runs stay pixel-exact.
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <ctime>

#include "lodepng.h"
#include "lodepng_incremental.h"

static double seconds(clock_t a, clock_t b) {
  return (double)(b - a) / CLOCKS_PER_SEC;
}

static std::vector<unsigned char> make_png(unsigned w, unsigned h) {
  std::vector<unsigned char> raw((size_t)w * h * 4);
  unsigned rng = 0x9e3779b9u;
  for(size_t p = 0; p < raw.size(); ++p) {
    unsigned pix = (unsigned)(p / 4), ch = (unsigned)(p % 4);
    unsigned x = pix % w, y = pix / w;
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    unsigned v = (unsigned)((x * 5 + y * 11 + (x ^ y) * 3 + rng) & 255);
    if(ch == 3) v = 255;
    raw[p] = (unsigned char)v;
  }
  lodepng::State st;
  unsigned char* out = 0; size_t outsize = 0;
  st.encoder.auto_convert = 0;
  st.encoder.filter_strategy = LFS_MINSUM;
  st.info_raw.colortype = LCT_RGBA; st.info_raw.bitdepth = 8;
  st.info_png.color.colortype = LCT_RGBA; st.info_png.color.bitdepth = 8;
  st.encoder.zlibsettings.btype = 2;
  lodepng_encode(&out, &outsize, &raw[0], w, h, &st);
  std::vector<unsigned char> png(out, out + outsize);
  std::free(out);
  return png;
}

int main() {
  const unsigned W = 512, H = 512, REPS = 20, NINTERRUPT = 100;
  std::vector<unsigned char> png = make_png(W, H);
  clock_t t0, t1;
  double t_oneshot, t_chunk_nosave, t_saveonly, t_interrupt100;
  unsigned long cs_a = 0, cs_b = 0, cs_c = 0;
  unsigned r;

  std::printf("workload: one %ux%u RGBA8 PNG (%u bytes), %u repetitions, CPU time\n",
              W, H, (unsigned)png.size(), REPS);

  t0 = std::clock();
  for(r = 0; r < REPS; ++r) {
    unsigned char* out=0; unsigned w,h;
    if(lodepng_decode32(&out,&w,&h,&png[0],png.size())==0) {
      for(size_t k=0;k<(size_t)w*h*4;++k) cs_a += out[k];
      std::free(out);
    }
  }
  t1 = std::clock(); t_oneshot = seconds(t0,t1);

  /* incremental, one row per step, no serialization (state-machine overhead) */
  t0 = std::clock();
  for(r = 0; r < REPS; ++r) {
    LodePNGIncremental* d=0; unsigned char* out=0; unsigned w,h;
    if(lodepng_inc_begin(&d,&out,&w,&h,&png[0],png.size())) continue;
    unsigned e;
    do { e=lodepng_inc_step(d,1,0); } while(e==130);
    if(e==0) for(size_t k=0;k<(size_t)w*h*4;++k) cs_b += out[k];
    lodepng_inc_destroy(d); std::free(out);
  }
  t1 = std::clock(); t_chunk_nosave = seconds(t0,t1);

  /* serialization round-trip alone: 100 save/reload cycles at one breakpoint */
  t0 = std::clock();
  for(r = 0; r < REPS; ++r) {
    LodePNGIncremental* d=0; unsigned char* out=0; unsigned w,h,rows;
    if(lodepng_inc_begin(&d,&out,&w,&h,&png[0],png.size())) continue;
    lodepng_inc_step(d, 1, &rows);
    for(unsigned k=0;k<NINTERRUPT;++k) {
      std::vector<unsigned char> blob(lodepng_inc_state_size(d));
      lodepng_inc_save(d,&blob[0]);
      lodepng_inc_destroy(d); d=0;
      lodepng_inc_restore(&d,&blob[0],blob.size(),&png[0],png.size(),out);
    }
    lodepng_inc_destroy(d); std::free(out);
  }
  t1 = std::clock(); t_saveonly = seconds(t0,t1);

  /* full decode with 100 interrupt+resume cycles spread over the rows */
  t0 = std::clock();
  for(r = 0; r < REPS; ++r) {
    LodePNGIncremental* d=0; unsigned char* out=0; unsigned w,h;
    if(lodepng_inc_begin(&d,&out,&w,&h,&png[0],png.size())) continue;
    unsigned rows_per = H / (NINTERRUPT + 1);
    unsigned e = 0, cycles = 0;
    for(;;) {
      unsigned rows=0;
      e = lodepng_inc_step(d, rows_per, &rows);
      if(e != 0 && e != 130) break;
      if(e == 0) break;
      if(cycles < NINTERRUPT) {
        std::vector<unsigned char> blob(lodepng_inc_state_size(d));
        lodepng_inc_save(d,&blob[0]);
        lodepng_inc_destroy(d); d=0;
        e = lodepng_inc_restore(&d,&blob[0],blob.size(),&png[0],png.size(),out);
        if(e) break;
        ++cycles;
      }
    }
    if(e==0) for(size_t k=0;k<(size_t)w*h*4;++k) cs_c += out[k];
    lodepng_inc_destroy(d); std::free(out);
  }
  t1 = std::clock(); t_interrupt100 = seconds(t0,t1);

  std::printf("output checksums (equal => pixel-identical): %lu %lu %lu\n",
              cs_a, cs_b, cs_c);
  std::printf("--------------------------------------------------------------------\n");
  std::printf("total CPU s over %u reps        per-rep ms\n", REPS);
  std::printf("  one-shot, no interrupts        %8.4f s   %7.3f ms/rep\n",
              t_oneshot, t_oneshot * 1000.0 / REPS);
  std::printf("  1 row/step, no save/reload     %8.4f s   %7.3f ms/rep  (%.2fx)\n",
              t_chunk_nosave, t_chunk_nosave * 1000.0 / REPS,
              t_chunk_nosave / t_oneshot);
  std::printf("  100 save/reload, no extra work %8.4f s   %7.3f ms/rep\n",
              t_saveonly, t_saveonly * 1000.0 / REPS);
  std::printf("  100 interrupt+resume decode    %8.4f s   %7.3f ms/rep  (%.2fx)\n",
              t_interrupt100, t_interrupt100 * 1000.0 / REPS,
              t_interrupt100 / t_oneshot);
  std::printf("--------------------------------------------------------------------\n");
  std::printf("  net cost of 100 interruptions  %7.3f ms/image (%.1f us/interrupt)\n",
              (t_interrupt100 - t_oneshot) * 1000.0 / REPS,
              (t_interrupt100 - t_oneshot) * 1e6 / REPS / NINTERRUPT);
  return (cs_a == cs_b && cs_b == cs_c) ? 0 : 2;
}

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lodepng.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
enum {CT_GREY=0,CT_RGB=2,CT_PALETTE=3,CT_GREY_ALPHA=4,CT_RGBA=6};
typedef unsigned(*enc_fn)(unsigned char**,size_t*,const unsigned char*,unsigned,unsigned,LodePNGState*);
typedef void(*si_fn)(LodePNGState*); typedef void(*sc_fn)(LodePNGState*); typedef void(*fr_fn)(void*);
typedef unsigned(*pa_fn)(LodePNGColorMode*,unsigned char,unsigned char,unsigned char,unsigned char);
struct Lib{void*h;enc_fn enc;si_fn ini;sc_fn cln;fr_fn fr;pa_fn pal;};
static Lib load(const char*p){Lib L;std::memset(&L,0,sizeof L);L.h=dlmopen(LM_ID_NEWLM,p,RTLD_NOW|RTLD_LOCAL);
 L.enc=(enc_fn)dlsym(L.h,"_Z14lodepng_encodePPhPmPKhjjP12LodePNGState");
 L.ini=(si_fn)dlsym(L.h,"_Z18lodepng_state_initP12LodePNGState");
 L.cln=(sc_fn)dlsym(L.h,"_Z21lodepng_state_cleanupP12LodePNGState");
 L.fr=(fr_fn)dlsym(L.h,"free");
 L.pal=(pa_fn)dlsym(L.h,"_Z19lodepng_palette_addP16LodePNGColorModehhhh");return L;}
static uint64_t s=12345; static unsigned r(){s^=s<<13;s^=s>>7;s^=s<<17;return s>>16;}
static unsigned chof(int ct){static const unsigned c[7]={1,0,3,1,2,0,4};return c[ct];}
static int one(const Lib&B,const Lib&N,int ct,unsigned bd,unsigned w,unsigned h,unsigned pz){
 unsigned ch=chof(ct); size_t rb=((size_t)w*ch*bd+7)/8;
 std::vector<unsigned char> img(rb*h);
 for(size_t k=0;k<img.size();++k)img[k]=r()&255;
 auto enc=[&](const Lib&L,unsigned char**o,size_t*z){LodePNGState st;L.ini(&st);
  st.info_raw.colortype=(LodePNGColorType)ct;st.info_raw.bitdepth=bd;
  st.encoder.filter_strategy=LFS_MINSUM;st.encoder.filter_palette_zero=pz;st.encoder.auto_convert=0;
  st.info_png.color.colortype=(LodePNGColorType)ct;st.info_png.color.bitdepth=bd;
  if(ct==CT_PALETTE)for(int i=0;i<256;++i)L.pal(&st.info_png.color,i,255-i,(i*7)&255,255);
  unsigned e=L.enc(o,z,img.data(),w,h,&st);L.cln(&st);return e;};
 unsigned char*bo=0,*no=0;size_t bz=0,nz=0;
 unsigned e1=enc(B,&bo,&bz),e2=enc(N,&no,&nz);
 int ok=(e1==e2)&&(bz==nz)&&(std::memcmp(bo,no,bz)==0);
 if(!ok)std::printf("FAIL ct=%d bd=%u w=%u h=%u pz=%u e=%u/%u size=%zu/%zu\n",ct,bd,w,h,pz,e1,e2,bz,nz);
 B.fr(bo);N.fr(no);return ok;}
int main(int,char**av){Lib B=load(av[1]),N=load(av[2]);long n=0,fail=0;
 int cts[]={CT_GREY,CT_RGB,CT_GREY_ALPHA,CT_RGBA}; unsigned bds[]={8,16};
 /* widths straddling SIMD boundaries and narrow lines; heights include 1 */
 unsigned widths[]={1,2,3,4,5,6,7,8,9,10,11,12,15,16,17,23,24,25,31,32,33,63,64,65,127,128,129,255,256,257};
 for(int ci=0;ci<4;++ci)for(unsigned bi=0;bi<2;++bi)
  for(unsigned wi=0;wi<sizeof(widths)/sizeof(widths[0]);++wi)
   for(unsigned h=1;h<=3;++h){++n;if(!one(B,N,cts[ci],bds[bi],widths[wi],h,1))++fail;}
 /* sub-byte grayscale (bytewidth 1) and palette, palette_zero off so MINSUM runs */
 for(unsigned wi=0;wi<sizeof(widths)/sizeof(widths[0]);++wi){
   ++n; if(!one(B,N,CT_GREY,1,widths[wi],2,0))++fail;
   ++n; if(!one(B,N,CT_PALETTE,8,widths[wi],2,0))++fail;
 }
 std::printf("edge cases: %ld tested, %ld failures\n",n,fail);
 return fail?1:0;}

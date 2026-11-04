// JPEG entropy tables and integer IDCT extracted from stb_image v2.30.
/*
------------------------------------------------------------------------------
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------
*/

#pragma once
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>

#define FAST_BITS 9 // Huffman 前缀查表位数
#define STBI__MARKER_none 0xff
#define STBI_ASSERT(value) assert(value)
#define stbi_inline inline
using stbi_uc = unsigned char;
using stbi__uint16 = uint16_t;
using stbi__int16 = int16_t;
using stbi__uint32 = uint32_t;
struct stbi_io_callbacks {
    int (*read)(void*, char*, int);
    void (*skip)(void*, int);
    int (*eof)(void*);
};
struct stbi__context {
    uint32_t img_x, img_y;
    int img_n, img_out_n;
    stbi_io_callbacks io;
    void* io_user_data;
    int read_from_callbacks, buflen;
    stbi_uc buffer_start[16384]; // 流式输入与条带输出共用的 PSRAM 缓冲
    int callback_already_read;
    stbi_uc *img_buffer, *img_buffer_end, *img_buffer_original, *img_buffer_original_end;
};
static int stbi__err(const char*, const char*) { return 0; }

static void stbi__refill_buffer(stbi__context *s)
{
   int n = (s->io.read)(s->io_user_data,(char*)s->buffer_start,s->buflen);
   s->callback_already_read += (int) (s->img_buffer - s->img_buffer_original);
   if (n == 0) {
      // at end of file, treat same as if from memory, but need to handle case
      // where s->img_buffer isn't pointing to safe memory, e.g. 0-byte file
      s->read_from_callbacks = 0;
      s->img_buffer = s->buffer_start;
      s->img_buffer_end = s->buffer_start+1;
      *s->img_buffer = 0;
   } else {
      s->img_buffer = s->buffer_start;
      s->img_buffer_end = s->buffer_start + n;
   }
}

static void stbi__start_callbacks(stbi__context *s, stbi_io_callbacks *c, void *user)
{
   s->io = *c;
   s->io_user_data = user;
   s->buflen = sizeof(s->buffer_start);
   s->read_from_callbacks = 1;
   s->callback_already_read = 0;
   s->img_buffer = s->img_buffer_original = s->buffer_start;
   stbi__refill_buffer(s);
   s->img_buffer_original_end = s->img_buffer_end;
}

stbi_inline static stbi_uc stbi__get8(stbi__context *s)
{
   if (s->img_buffer < s->img_buffer_end)
      return *s->img_buffer++;
   if (s->read_from_callbacks) {
      stbi__refill_buffer(s);
      return *s->img_buffer++;
   }
   return 0;
}

stbi_inline static int stbi__at_eof(stbi__context *s)
{
   if (s->read_from_callbacks && s->img_buffer < s->img_buffer_end) return 0;
   if (s->io.read) {
      if (!(s->io.eof)(s->io_user_data)) return 0;
      // if feof() is true, check if buffer = end
      // special case: we've only got the special 0 character at the end
      if (s->read_from_callbacks == 0) return 1;
   }

   return s->img_buffer >= s->img_buffer_end;
}

static void stbi__skip(stbi__context *s, int n)
{
   if (n == 0) return;  // already there!
   if (n < 0) {
      s->img_buffer = s->img_buffer_end;
      return;
   }
   if (s->io.read) {
      int blen = (int) (s->img_buffer_end - s->img_buffer);
      if (blen < n) {
         s->img_buffer = s->img_buffer_end;
         (s->io.skip)(s->io_user_data, n - blen);
         return;
      }
   }
   s->img_buffer += n;
}

static int stbi__get16be(stbi__context *s)
{
   int z = stbi__get8(s);
   return (z << 8) + stbi__get8(s);
}

static int stbi__addints_valid(int a, int b)
{
   if ((a >= 0) != (b >= 0)) return 1; // a and b have different signs, so no overflow
   if (a < 0 && b < 0) return a >= INT_MIN - b; // same as a + b >= INT_MIN; INT_MIN - b cannot overflow since b < 0.
   return a <= INT_MAX - b;
}

static int stbi__mul2shorts_valid(int a, int b)
{
   if (b == 0 || b == -1) return 1; // multiplication by 0 is always 0; check for -1 so SHRT_MIN/b doesn't overflow
   if ((a >= 0) == (b >= 0)) return a <= SHRT_MAX/b; // product is positive, so similar to mul2sizes_valid
   if (b < 0) return a <= SHRT_MIN / b; // same as a * b >= SHRT_MIN
   return a >= SHRT_MIN / b;
}

struct stbi__huffman {
    uint16_t fast[1 << FAST_BITS]; // 短码的长度及符号索引
    stbi_uc values[256]; // 规范顺序的符号
    uint16_t symbols; // 已加载符号数量
    unsigned maxcode[18]; // 各码长的上界
    int delta[17]; // 规范码到符号索引的偏移
};

typedef struct
{
   stbi__context *s;
   stbi__huffman huff_dc[4];
   stbi__huffman huff_ac[4];
   stbi__uint16 dequant[4][64];
   stbi__int16 fast_ac[4][1 << FAST_BITS];

// sizes for components, interleaved MCUs
   int img_h_max, img_v_max;
   int img_mcu_x, img_mcu_y;
   int img_mcu_w, img_mcu_h;

// definition of jpeg image component
   struct
   {
      int id;
      int h,v;
      int tq;
      int hd,ha;
      int dc_pred;

      int x,y,w2,h2;
      stbi_uc *data;
      void *raw_data, *raw_coeff;
      stbi_uc *linebuf;
      short   *coeff;   // progressive only
      int      coeff_w, coeff_h; // number of 8x8 coefficient blocks
   } img_comp[4];

   stbi__uint32   code_buffer; // jpeg entropy-coded buffer
   int            code_bits;   // number of valid bits
   unsigned char  marker;      // marker seen while filling entropy buffer
   int            nomore;      // flag if we saw a marker so must stop

   int            progressive;
   int            spec_start;
   int            spec_end;
   int            succ_high;
   int            succ_low;
   int            eob_run;
   int            jfif;
   int            app14_color_transform; // Adobe APP14 tag
   int            rgb;

   int scan_n, order[4];
   int restart_interval, todo;

// kernels
   void (*idct_block_kernel)(stbi_uc *out, int out_stride, short data[64]);
   void (*YCbCr_to_RGB_kernel)(stbi_uc *out, const stbi_uc *y, const stbi_uc *pcb, const stbi_uc *pcr, int count, int step);
   stbi_uc *(*resample_row_hv_2_kernel)(stbi_uc *out, stbi_uc *in_near, stbi_uc *in_far, int w, int hs);
} stbi__jpeg;


static int stbi__build_huffman(stbi__huffman* table, int* count) {
    std::fill(std::begin(table->fast), std::end(table->fast), UINT16_MAX);
    unsigned code = 0, symbols = 0;
    for (unsigned length = 1; length <= 16; ++length) {
        const unsigned entries = count[length - 1];
        if (symbols + entries > 256 || code + entries > (1u << length)) return 0;
        table->delta[length] = int(symbols) - int(code);
        table->maxcode[length] = (code + entries) << (16 - length);
        for (unsigned n = 0; n < entries; ++n) {
            if (length <= FAST_BITS) {
                const unsigned first = code << (FAST_BITS - length);
                const unsigned span = 1u << (FAST_BITS - length);
                for (unsigned index = first; index < first + span; ++index)
                    table->fast[index] = uint16_t((length << 8) | symbols);
            }
            ++symbols;
            ++code;
        }
        code <<= 1;
    }
    table->maxcode[17] = UINT32_MAX;
    table->symbols = symbols;
    return symbols != 0;
}

static void stbi__build_fast_ac(stbi__int16* fast, stbi__huffman* table) {
    for (unsigned index = 0; index < (1u << FAST_BITS); ++index) {
        fast[index] = 0;
        const unsigned entry = table->fast[index];
        if (entry == UINT16_MAX) continue;
        const unsigned length = entry >> 8;
        const unsigned symbol = table->values[entry & 255];
        const unsigned magnitude = symbol & 15;
        if (magnitude == 0 || length + magnitude > FAST_BITS) continue;
        int value = ((index << length) & ((1u << FAST_BITS) - 1)) >> (FAST_BITS - magnitude);
        if (value < (1 << (magnitude - 1))) value += 1 - (1 << magnitude);
        if (value >= -128 && value <= 127)
            fast[index] = stbi__int16(value * 256 + (symbol & 0xf0) + length + magnitude);
    }
}

static const stbi__uint32 stbi__bmask[17]={0,1,3,7,15,31,63,127,255,511,1023,2047,4095,8191,16383,32767,65535};

static const stbi_uc stbi__jpeg_dezigzag[64+15] =
{
    0,  1,  8, 16,  9,  2,  3, 10,
   17, 24, 32, 25, 18, 11,  4,  5,
   12, 19, 26, 33, 40, 48, 41, 34,
   27, 20, 13,  6,  7, 14, 21, 28,
   35, 42, 49, 56, 57, 50, 43, 36,
   29, 22, 15, 23, 30, 37, 44, 51,
   58, 59, 52, 45, 38, 31, 39, 46,
   53, 60, 61, 54, 47, 55, 62, 63,
   // let corrupt input sample past end
   63, 63, 63, 63, 63, 63, 63, 63,
   63, 63, 63, 63, 63, 63, 63
};

// take a -128..127 value and stbi__clamp it and convert to 0..255
stbi_inline static stbi_uc stbi__clamp(int x)
{
   // trick to use a single test to catch both cases
   if ((unsigned int) x > 255) {
      if (x < 0) return 0;
      if (x > 255) return 255;
   }
   return (stbi_uc) x;
}

#define stbi__f2f(x)  ((int) (((x) * 4096 + 0.5)))
#define stbi__fsh(x)  ((x) * 4096)

// derived from jidctint -- DCT_ISLOW
#define STBI__IDCT_1D(s0,s1,s2,s3,s4,s5,s6,s7) \
   int t0,t1,t2,t3,p1,p2,p3,p4,p5,x0,x1,x2,x3; \
   p2 = s2;                                    \
   p3 = s6;                                    \
   p1 = (p2+p3) * stbi__f2f(0.5411961f);       \
   t2 = p1 + p3*stbi__f2f(-1.847759065f);      \
   t3 = p1 + p2*stbi__f2f( 0.765366865f);      \
   p2 = s0;                                    \
   p3 = s4;                                    \
   t0 = stbi__fsh(p2+p3);                      \
   t1 = stbi__fsh(p2-p3);                      \
   x0 = t0+t3;                                 \
   x3 = t0-t3;                                 \
   x1 = t1+t2;                                 \
   x2 = t1-t2;                                 \
   t0 = s7;                                    \
   t1 = s5;                                    \
   t2 = s3;                                    \
   t3 = s1;                                    \
   p3 = t0+t2;                                 \
   p4 = t1+t3;                                 \
   p1 = t0+t3;                                 \
   p2 = t1+t2;                                 \
   p5 = (p3+p4)*stbi__f2f( 1.175875602f);      \
   t0 = t0*stbi__f2f( 0.298631336f);           \
   t1 = t1*stbi__f2f( 2.053119869f);           \
   t2 = t2*stbi__f2f( 3.072711026f);           \
   t3 = t3*stbi__f2f( 1.501321110f);           \
   p1 = p5 + p1*stbi__f2f(-0.899976223f);      \
   p2 = p5 + p2*stbi__f2f(-2.562915447f);      \
   p3 = p3*stbi__f2f(-1.961570560f);           \
   p4 = p4*stbi__f2f(-0.390180644f);           \
   t3 += p1+p4;                                \
   t2 += p2+p3;                                \
   t1 += p2+p4;                                \
   t0 += p1+p3;

static void stbi__idct_block(stbi_uc *out, int out_stride, short data[64])
{
   int i,val[64],*v=val;
   stbi_uc *o;
   short *d = data;

   // columns
   for (i=0; i < 8; ++i,++d, ++v) {
      // if all zeroes, shortcut -- this avoids dequantizing 0s and IDCTing
      if (d[ 8]==0 && d[16]==0 && d[24]==0 && d[32]==0
           && d[40]==0 && d[48]==0 && d[56]==0) {
         //    no shortcut                 0     seconds
         //    (1|2|3|4|5|6|7)==0          0     seconds
         //    all separate               -0.047 seconds
         //    1 && 2|3 && 4|5 && 6|7:    -0.047 seconds
         int dcterm = d[0]*4;
         v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dcterm;
      } else {
         STBI__IDCT_1D(d[ 0],d[ 8],d[16],d[24],d[32],d[40],d[48],d[56])
         // constants scaled things up by 1<<12; let's bring them back
         // down, but keep 2 extra bits of precision
         x0 += 512; x1 += 512; x2 += 512; x3 += 512;
         v[ 0] = (x0+t3) >> 10;
         v[56] = (x0-t3) >> 10;
         v[ 8] = (x1+t2) >> 10;
         v[48] = (x1-t2) >> 10;
         v[16] = (x2+t1) >> 10;
         v[40] = (x2-t1) >> 10;
         v[24] = (x3+t0) >> 10;
         v[32] = (x3-t0) >> 10;
      }
   }

   for (i=0, v=val, o=out; i < 8; ++i,v+=8,o+=out_stride) {
      // no fast case since the first 1D IDCT spread components out
      STBI__IDCT_1D(v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7])
      // constants scaled things up by 1<<12, plus we had 1<<2 from first
      // loop, plus horizontal and vertical each scale by sqrt(8) so together
      // we've got an extra 1<<3, so 1<<17 total we need to remove.
      // so we want to round that, which means adding 0.5 * 1<<17,
      // aka 65536. Also, we'll end up with -128 to 127 that we want
      // to encode as 0..255 by adding 128, so we'll add that before the shift
      x0 += 65536 + (128<<17);
      x1 += 65536 + (128<<17);
      x2 += 65536 + (128<<17);
      x3 += 65536 + (128<<17);
      // tried computing the shifts into temps, or'ing the temps to see
      // if any were out of range, but that was slower
      o[0] = stbi__clamp((x0+t3) >> 17);
      o[7] = stbi__clamp((x0-t3) >> 17);
      o[1] = stbi__clamp((x1+t2) >> 17);
      o[6] = stbi__clamp((x1-t2) >> 17);
      o[2] = stbi__clamp((x2+t1) >> 17);
      o[5] = stbi__clamp((x2-t1) >> 17);
      o[3] = stbi__clamp((x3+t0) >> 17);
      o[4] = stbi__clamp((x3-t0) >> 17);
   }
}


static stbi_uc stbi__get_marker(stbi__jpeg *j)
{
   stbi_uc x;
   if (j->marker != STBI__MARKER_none) { x = j->marker; j->marker = STBI__MARKER_none; return x; }
   x = stbi__get8(j->s);
   if (x != 0xff) return STBI__MARKER_none;
   while (x == 0xff)
      x = stbi__get8(j->s); // consume repeated 0xff fill bytes
   return x;
}

static void stbi__jpeg_reset(stbi__jpeg *j)
{
   j->code_bits = 0;
   j->code_buffer = 0;
   j->nomore = 0;
   j->img_comp[0].dc_pred = j->img_comp[1].dc_pred = j->img_comp[2].dc_pred = j->img_comp[3].dc_pred = 0;
   j->marker = STBI__MARKER_none;
   j->todo = j->restart_interval ? j->restart_interval : 0x7fffffff;
   j->eob_run = 0;
   // no more than 1<<31 MCUs if no restart_interal? that's plenty safe,
   // since we don't even allow 1<<30 pixels
}

static int stbi__process_marker(stbi__jpeg *z, int m)
{
   int L;
   switch (m) {
      case STBI__MARKER_none: // no marker found
         return stbi__err("expected marker","Corrupt JPEG");

      case 0xDD: // DRI - specify restart interval
         if (stbi__get16be(z->s) != 4) return stbi__err("bad DRI len","Corrupt JPEG");
         z->restart_interval = stbi__get16be(z->s);
         return 1;

      case 0xDB: // DQT - define quantization table
         L = stbi__get16be(z->s)-2;
         while (L > 0) {
            int q = stbi__get8(z->s);
            int p = q >> 4, sixteen = (p != 0);
            int t = q & 15,i;
            if (p != 0 && p != 1) return stbi__err("bad DQT type","Corrupt JPEG");
            if (t > 3) return stbi__err("bad DQT table","Corrupt JPEG");

            for (i=0; i < 64; ++i)
               z->dequant[t][stbi__jpeg_dezigzag[i]] = (stbi__uint16)(sixteen ? stbi__get16be(z->s) : stbi__get8(z->s));
            L -= (sixteen ? 129 : 65);
         }
         return L==0;

      case 0xC4: // DHT - define huffman table
         L = stbi__get16be(z->s)-2;
         while (L > 0) {
            stbi_uc *v;
            int sizes[16],i,n=0;
            int q = stbi__get8(z->s);
            int tc = q >> 4;
            int th = q & 15;
            if (tc > 1 || th > 3) return stbi__err("bad DHT header","Corrupt JPEG");
            for (i=0; i < 16; ++i) {
               sizes[i] = stbi__get8(z->s);
               n += sizes[i];
            }
            if(n > 256) return stbi__err("bad DHT header","Corrupt JPEG"); // Loop over i < n would write past end of values!
            L -= 17;
            if (tc == 0) {
               if (!stbi__build_huffman(z->huff_dc+th, sizes)) return 0;
               v = z->huff_dc[th].values;
            } else {
               if (!stbi__build_huffman(z->huff_ac+th, sizes)) return 0;
               v = z->huff_ac[th].values;
            }
            for (i=0; i < n; ++i)
               v[i] = stbi__get8(z->s);
            if (tc != 0)
               stbi__build_fast_ac(z->fast_ac[th], z->huff_ac + th);
            L -= n;
         }
         return L==0;
   }

   // check for comment block or APP blocks
   if ((m >= 0xE0 && m <= 0xEF) || m == 0xFE) {
      L = stbi__get16be(z->s);
      if (L < 2) {
         if (m == 0xFE)
            return stbi__err("bad COM len","Corrupt JPEG");
         else
            return stbi__err("bad APP len","Corrupt JPEG");
      }
      L -= 2;

      if (m == 0xE0 && L >= 5) { // JFIF APP0 segment
         static const unsigned char tag[5] = {'J','F','I','F','\0'};
         int ok = 1;
         int i;
         for (i=0; i < 5; ++i)
            if (stbi__get8(z->s) != tag[i])
               ok = 0;
         L -= 5;
         if (ok)
            z->jfif = 1;
      } else if (m == 0xEE && L >= 5) { // Adobe APP14 segment
         static const unsigned char tag[5] = {'A','d','o','b','e'};
         int ok = 1;
         int i;
         for (i=0; i < 5; ++i)
            if (stbi__get8(z->s) != tag[i])
               ok = 0;
         L -= 5;
         if (ok) {
            if (L < 7) return stbi__err("bad APP14 len", "Corrupt JPEG");
            stbi__get16be(z->s); // version
            stbi__get16be(z->s); // flags0
            stbi__get16be(z->s); // flags1
            z->app14_color_transform = stbi__get8(z->s); // color transform
            L -= 7;
         }
      }

      stbi__skip(z->s, L);
      return 1;
   }

   return stbi__err("unknown marker","Corrupt JPEG");
}

static int stbi__process_scan_header(stbi__jpeg *z)
{
   int i;
   int Ls = stbi__get16be(z->s);
   z->scan_n = stbi__get8(z->s);
   if (z->scan_n < 1 || z->scan_n > 4 || z->scan_n > (int) z->s->img_n) return stbi__err("bad SOS component count","Corrupt JPEG");
   if (Ls != 6+2*z->scan_n) return stbi__err("bad SOS len","Corrupt JPEG");
   for (i=0; i < z->scan_n; ++i) {
      int id = stbi__get8(z->s), which;
      int q = stbi__get8(z->s);
      for (which = 0; which < z->s->img_n; ++which)
         if (z->img_comp[which].id == id)
            break;
      if (which == z->s->img_n) return 0; // no match
      z->img_comp[which].hd = q >> 4;   if (z->img_comp[which].hd > 3) return stbi__err("bad DC huff","Corrupt JPEG");
      z->img_comp[which].ha = q & 15;   if (z->img_comp[which].ha > 3) return stbi__err("bad AC huff","Corrupt JPEG");
      z->order[i] = which;
   }

   {
      int aa;
      z->spec_start = stbi__get8(z->s);
      z->spec_end   = stbi__get8(z->s); // should be 63, but might be 0
      aa = stbi__get8(z->s);
      z->succ_high = (aa >> 4);
      z->succ_low  = (aa & 15);
      if (z->progressive) {
         if (z->spec_start > 63 || z->spec_end > 63  || z->spec_start > z->spec_end || z->succ_high > 13 || z->succ_low > 13)
            return stbi__err("bad SOS", "Corrupt JPEG");
      } else {
         if (z->spec_start != 0) return stbi__err("bad SOS","Corrupt JPEG");
         if (z->succ_high != 0 || z->succ_low != 0) return stbi__err("bad SOS","Corrupt JPEG");
         z->spec_end = 63;
      }
   }

   return 1;
}

static stbi_uc stbi__skip_jpeg_junk_at_end(stbi__jpeg *j)
{
   // some JPEGs have junk at end, skip over it but if we find what looks
   // like a valid marker, resume there
   while (!stbi__at_eof(j->s)) {
      stbi_uc x = stbi__get8(j->s);
      while (x == 0xff) { // might be a marker
         if (stbi__at_eof(j->s)) return STBI__MARKER_none;
         x = stbi__get8(j->s);
         if (x != 0x00 && x != 0xff) {
            // not a stuffed zero or lead-in to another marker, looks
            // like an actual marker, return it
            return x;
         }
         // stuffed zero has x=0 now which ends the loop, meaning we go
         // back to regular scan loop.
         // repeated 0xff keeps trying to read the next byte of the marker.
      }
   }
   return STBI__MARKER_none;
}

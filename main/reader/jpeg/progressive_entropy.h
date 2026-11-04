// Entropy routines adapted from stb_image v2.30; strict bit termination is local.
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


void StrictGrow(stbi__jpeg* decoder) {
    auto& state = *static_cast<Progressive*>(decoder->s->io_user_data);
    while (decoder->code_bits <= 24 && !decoder->nomore) {
        if (stbi__at_eof(decoder->s)) { state.context->Fail(ImageDecodeError::kInvalidData); return; }
        unsigned byte = stbi__get8(decoder->s);
        if (byte == 255) {
            unsigned marker;
            do {
                if (stbi__at_eof(decoder->s)) { state.context->Fail(ImageDecodeError::kInvalidData); return; }
                marker = stbi__get8(decoder->s);
            } while (marker == 255);
            if (marker != 0) { decoder->marker = marker; decoder->nomore = 1; return; }
        }
        decoder->code_buffer |= byte << (24 - decoder->code_bits);
        decoder->code_bits += 8;
    }
}

int StrictBits(stbi__jpeg* decoder, int count) {
    if (decoder->code_bits < count) StrictGrow(decoder);
    if (decoder->code_bits < count) {
        static_cast<Progressive*>(decoder->s->io_user_data)->context->Fail(ImageDecodeError::kInvalidData);
        return 0;
    }
    const unsigned value = decoder->code_buffer >> (32 - count);
    decoder->code_buffer <<= count;
    decoder->code_bits -= count;
    return value;
}
int StrictBit(stbi__jpeg* decoder) { return StrictBits(decoder, 1); }
int StrictExtend(stbi__jpeg* decoder, int count) {
    const int value = StrictBits(decoder, count);
    return value < (1 << (count - 1)) ? value + 1 - (1 << count) : value;
}


static int StrictHuffman(stbi__jpeg* decoder, stbi__huffman* table) {
    if (decoder->code_bits < 16) StrictGrow(decoder);
    const unsigned prefix = decoder->code_buffer >> (32 - FAST_BITS);
    const unsigned entry = table->fast[prefix];
    unsigned length, symbol;
    if (entry != UINT16_MAX) {
        length = entry >> 8;
        symbol = entry & 255;
    } else {
        const unsigned high = decoder->code_buffer >> 16;
        for (length = FAST_BITS + 1; length <= 16 && high >= table->maxcode[length]; ++length) {}
        if (length > 16) return -1;
        const int index = ((decoder->code_buffer >> (32 - length)) & stbi__bmask[length]) + table->delta[length];
        if (index < 0 || unsigned(index) >= table->symbols) return -1;
        symbol = index;
    }
    if (length > unsigned(decoder->code_bits)) return -1;
    decoder->code_buffer <<= length;
    decoder->code_bits -= length;
    return table->values[symbol];
}

static int StrictDc(stbi__jpeg *j, short data[64], stbi__huffman *hdc, int b)
{
   int diff,dc;
   int t;
   if (j->spec_end != 0) return stbi__err("can't merge dc and ac", "Corrupt JPEG");

   if (j->code_bits < 16) StrictGrow(j);

   if (j->succ_high == 0) {
      // first scan for DC coefficient, must be first
      memset(data,0,64*sizeof(data[0])); // 0 all the ac values now
      t = StrictHuffman(j, hdc);
      if (t < 0 || t > 11) return stbi__err("can't merge dc and ac", "Corrupt JPEG");
      diff = t ? StrictExtend(j, t) : 0;

      if (!stbi__addints_valid(j->img_comp[b].dc_pred, diff)) return stbi__err("bad delta", "Corrupt JPEG");
      dc = j->img_comp[b].dc_pred + diff;
      j->img_comp[b].dc_pred = dc;
      if (!stbi__mul2shorts_valid(dc, 1 << j->succ_low)) return stbi__err("can't merge dc and ac", "Corrupt JPEG");
      data[0] = (short) (dc * (1 << j->succ_low));
   } else {
      // refinement scan for DC coefficient
      if (StrictBit(j))
         data[0] += (short) (1 << j->succ_low);
   }
   return 1;
}

template<bool Mask = false>
static int StrictAc(stbi__jpeg *j, short data[64], stbi__huffman *hac, stbi__int16 *fac,
                    uint64_t* significance = nullptr, uint64_t spectral_mask = 0)
{
   int k;
   if (j->spec_start == 0) return stbi__err("can't merge dc and ac", "Corrupt JPEG");

   const auto nonzero = [&](unsigned index) {
      if constexpr (Mask) return (*significance & (uint64_t(1) << index)) != 0;
      else return data[index] != 0;
   };
   const auto assign = [&](unsigned index, short value) {
      if constexpr (Mask) {
         if (value != 0) *significance |= uint64_t(1) << index;
         else *significance &= ~(uint64_t(1) << index);
      } else data[index] = value;
   };
   const auto refine = [&](unsigned index, short bit) {
      // 显著性模式仍消费已有非零系数的修正位，不保存符号及幅值。
      if (StrictBit(j)) {
         if constexpr (!Mask) {
            short* value = &data[index];
            if ((*value & bit) == 0) {
               if (*value > 0) *value += bit;
               else *value -= bit;
            }
         }
      }
   };

   if (j->succ_high == 0) {
      int shift = j->succ_low;

      if (j->eob_run) {
         --j->eob_run;
         return 1;
      }

      k = j->spec_start;
      do {
         unsigned int zig;
         int c,r,s;
         if (j->code_bits < 16) StrictGrow(j);
         c = (j->code_buffer >> (32 - FAST_BITS)) & ((1 << FAST_BITS)-1);
         r = fac[c];
         if (r) { // fast-AC path
            k += (r >> 4) & 15; // run
            if (k > j->spec_end) return stbi__err("AC run", "Corrupt JPEG");
            s = r & 15; // combined length
            if (s > j->code_bits) return stbi__err("bad huffman code", "Combined length longer than code bits available");
            j->code_buffer <<= s;
            j->code_bits -= s;
            zig = stbi__jpeg_dezigzag[k++];
            assign(zig, (short) ((r >> 8) * (1 << shift)));
         } else {
            int rs = StrictHuffman(j, hac);
            if (rs < 0) return stbi__err("bad huffman code","Corrupt JPEG");
            s = rs & 15;
            r = rs >> 4;
            if (s == 0) {
               if (r < 15) {
                  j->eob_run = (1 << r);
                  if (r)
                     j->eob_run += StrictBits(j, r);
                  --j->eob_run;
                  break;
               }
               k += 16;
               if (k > j->spec_end + 1) return stbi__err("AC run", "Corrupt JPEG");
            } else {
               k += r;
               if (k > j->spec_end) return stbi__err("AC run", "Corrupt JPEG");
               zig = stbi__jpeg_dezigzag[k++];
               assign(zig, (short) (StrictExtend(j,s) * (1 << shift)));
            }
         }
      } while (k <= j->spec_end);
   } else {
      // refinement scan for these AC coefficients

      short bit = (short) (1 << j->succ_low);

      if (j->eob_run) {
         --j->eob_run;
         if constexpr (Mask) {
            // 该分支不会新增非零系数；所有已有系数的修正位连续出现且均不保留。
            unsigned remaining = __builtin_popcountll(*significance & spectral_mask);
            while (remaining != 0) {
               const unsigned count = std::min(remaining, 16u);
               StrictBits(j, count);
               remaining -= count;
            }
            return 1;
         }
         for (k = j->spec_start; k <= j->spec_end; ++k) {
            const unsigned zig = stbi__jpeg_dezigzag[k];
            if (nonzero(zig)) refine(zig, bit);
         }
      } else {
         k = j->spec_start;
         do {
            int r,s;
            int rs = StrictHuffman(j, hac); // @OPTIMIZE see if we can use the fast path here, advance-by-r is so slow, eh
            if (rs < 0) return stbi__err("bad huffman code","Corrupt JPEG");
            s = rs & 15;
            r = rs >> 4;
            if (s == 0) {
               if (r < 15) {
                  j->eob_run = (1 << r) - 1;
                  if (r)
                     j->eob_run += StrictBits(j, r);
                  r = 64; // force end of block
               } else {
                  // r=15 s=0 should write 16 0s, so we just do
                  // a run of 15 0s and then write s (which is 0),
                  // so we don't have to do anything special here
               }
            } else {
               if (s != 1) return stbi__err("bad huffman code", "Corrupt JPEG");
               // sign bit
               if (StrictBit(j))
                  s = bit;
               else
                  s = -bit;
            }

            // advance by r
            while (k <= j->spec_end) {
               const unsigned zig = stbi__jpeg_dezigzag[k++];
               if (nonzero(zig)) {
                  refine(zig, bit);
               } else {
                  if (r == 0) {
                     assign(zig, (short) s);
                     break;
                  }
                  --r;
               }
            }
         } while (k <= j->spec_end);
      }
   }
   return 1;
}

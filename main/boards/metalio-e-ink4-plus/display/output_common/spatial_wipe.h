#pragma once
#include "epd_internals.h"
#include <stdint.h>
#include <string.h>

// 目标 LUT 与只读波形不重叠；各 band 依次晚一相启动，保留真实灰阶转换。
static inline void epd_spatial_build(uint8_t* __restrict luts,const EpdWaveformPhases* phases,int frame) {
    for(int band=0;band<10;++band) {
        uint8_t* band_lut=luts+band*256;
        const int local=frame-band;
        if(local<0 || local>=phases->phases) {
            memset(band_lut,0,256);
            continue;
        }
        const uint8_t* phase=phases->luts+local*64;
        for(int i=0;i<64;++i) {
            const uint8_t packed=phase[i];
            band_lut[i*4]=(packed>>6)&3;
            band_lut[i*4+1]=(packed>>4)&3;
            band_lut[i*4+2]=(packed>>2)&3;
            band_lut[i*4+3]=packed&3;
        }
        band_lut[255]=0;
    }
}
static inline void epd_spatial_bands(uint8_t* bands,int size,int reverse) {
    for(int i=0;i<size;++i) bands[i]=(reverse ? size-1-i : i)*10/size;
}
enum {
    EPD_SPATIAL_MIXED = 0, // 保留完整的新旧灰阶单像素索引。
    EPD_SPATIAL_OLD = 1, // 两像素索引仅取旧灰阶。
    EPD_SPATIAL_TARGET = 2, // 两像素索引仅取目标灰阶。
    EPD_SPATIAL_ZERO = 3, // 整个分带在当前相保持不驱动。
};

// 必须在原单像素表建立后、通知供数任务前执行；模式复用既有 conversion_lut。
static inline void epd_spatial_prepare_pairs(uint8_t* __restrict luts,uint8_t* __restrict modes) {
    for(int band=0;band<10;++band) {
        uint8_t* lut=luts+band*256;
        int old_only=1,target_only=1;
        uint8_t any=0;
        for(int code=0;code<256;++code) {
            const uint8_t action=lut[code];
            any|=action;
            old_only&=action==lut[code&15];
            target_only&=action==lut[code&240];
            if(!old_only && !target_only) break;
        }
        if(!any) {
            modes[band]=EPD_SPATIAL_ZERO;
            continue;
        }
        if(!old_only && !target_only) {
            modes[band]=EPD_SPATIAL_MIXED;
            continue;
        }
        uint8_t actions[16];
        modes[band]=old_only ? EPD_SPATIAL_OLD : EPD_SPATIAL_TARGET;
        for(int gray=0;gray<16;++gray) actions[gray]=lut[old_only ? gray : gray<<4];
        for(int pair=0;pair<256;++pair) {
            lut[pair]=(uint8_t)((actions[pair&15]<<2)|actions[pair>>4]);
        }
    }
}
// Expand four packed target/old pairs with one aligned PSRAM load. S31 consumes
// the first pixel in the high bits. Force inline into the IRAM/O3 feeder rather
// than outlining this hot loop into a size-optimized flash function.
static inline __attribute__((always_inline)) uint8_t epd_spatial_lookup4(uint32_t in,const uint8_t* lut) {
    return (lut[in&255]<<6)|(lut[(in>>8)&255]<<4)|(lut[(in>>16)&255]<<2)|lut[in>>24];
}
static inline __attribute__((always_inline)) uint8_t epd_spatial_pair4(uint32_t in,const uint8_t* lut) {
    return (uint8_t)((lut[(in&15)|((in>>4)&240)]<<4)|
                     lut[((in>>16)&15)|((in>>20)&240)]);
}
// 输出行与输入、LUT 及 band 表不重叠，允许提前读取后续 PSRAM 输入。
static inline __attribute__((always_inline)) void epd_spatial_span(const uint8_t* input,uint8_t* __restrict output,
                                                               int words,const uint8_t* lut,int mode) {
    if(mode==EPD_SPATIAL_ZERO) {
        memset(output,0,words);
        return;
    }
    const uint32_t* src=(const uint32_t*)input;
    int i=0;
    if(mode!=EPD_SPATIAL_MIXED) {
        const unsigned shift=mode==EPD_SPATIAL_TARGET ? 4 : 0;
        for(;i+4<=words;i+=4) {
            const uint32_t in0=src[i]>>shift;
            const uint32_t in1=src[i+1]>>shift;
            const uint32_t in2=src[i+2]>>shift;
            const uint32_t in3=src[i+3]>>shift;
            output[i]=epd_spatial_pair4(in0,lut);
            output[i+1]=epd_spatial_pair4(in1,lut);
            output[i+2]=epd_spatial_pair4(in2,lut);
            output[i+3]=epd_spatial_pair4(in3,lut);
        }
        for(;i<words;++i) output[i]=epd_spatial_pair4(src[i]>>shift,lut);
        return;
    }
    for(;i+4<=words;i+=4) {
        const uint32_t in0=src[i];
        const uint32_t in1=src[i+1];
        const uint32_t in2=src[i+2];
        const uint32_t in3=src[i+3];
        output[i]=epd_spatial_lookup4(in0,lut);
        output[i+1]=epd_spatial_lookup4(in1,lut);
        output[i+2]=epd_spatial_lookup4(in2,lut);
        output[i+3]=epd_spatial_lookup4(in3,lut);
    }
    for(;i<words;++i) output[i]=epd_spatial_lookup4(src[i],lut);
}
static inline __attribute__((always_inline)) uint8_t epd_spatial_pixel(uint8_t input,const uint8_t* lut,int mode) {
    if(mode==EPD_SPATIAL_ZERO) return 0;
    if(mode==EPD_SPATIAL_MIXED) return lut[input];
    const unsigned gray=mode==EPD_SPATIAL_TARGET ? input>>4 : input&15;
    return lut[gray*17]&3;
}
static inline __attribute__((always_inline)) void epd_spatial_line(const uint8_t* input,uint8_t* __restrict output,int width,
                                                               const uint8_t* luts,const uint8_t* modes,
                                                               const uint8_t* bands,int by_y,int row) {
    if(by_y) {
        const int band=bands[row];
        epd_spatial_span(input,output,width/4,luts+band*256,modes[band]);
        return;
    }
    // Landscape: bulk-convert aligned spans; only the nine band boundaries
    // need individual per-pixel LUT selection. No extra internal buffer.
    int x=0;
    while(x<width) {
        const int band=bands[x];
        int end=x;
        while(end+4<=width && bands[end]==band && bands[end+3]==band) end+=4;
        if(end>x) {
            epd_spatial_span(input+x,output+x/4,(end-x)/4,luts+band*256,modes[band]);
            x=end;
        } else {
            output[x/4]=(epd_spatial_pixel(input[x],luts+bands[x]*256,modes[bands[x]])<<6)|
                (epd_spatial_pixel(input[x+1],luts+bands[x+1]*256,modes[bands[x+1]])<<4)|
                (epd_spatial_pixel(input[x+2],luts+bands[x+2]*256,modes[bands[x+2]])<<2)|
                epd_spatial_pixel(input[x+3],luts+bands[x+3]*256,modes[bands[x+3]]);
            x+=4;
        }
    }
}

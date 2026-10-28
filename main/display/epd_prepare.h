#pragma once
#include <cstddef>
#include <cstdint>

struct EpdChangeAnalysis { int changed; bool binary,gray_changed; };
static inline __attribute__((always_inline)) uint32_t EpdNonzeroNibbles(uint32_t value) {
    return (value|(value>>1)|(value>>2)|(value>>3))&0x11111111u;
}
static inline __attribute__((always_inline)) uint32_t EpdGrayNibbles(uint32_t value) {
    return ((value^(value>>1))|(value^(value>>2))|(value^(value>>3)))&0x11111111u;
}
// Four packed bytes = eight pixels. A marker bit counts once per changed nibble.
// Equal words still need gray inspection until the old/new pair is known gray.
[[maybe_unused]] static __attribute__((noinline,optimize("O3"))) EpdChangeAnalysis EpdAnalyzeChanges(
    const uint8_t* old,const uint8_t* target,size_t bytes) {
    EpdChangeAnalysis result{0,true,false};
    size_t i=0;
    if(((reinterpret_cast<uintptr_t>(old)|reinterpret_cast<uintptr_t>(target))&3)==0) {
        typedef uint32_t AliasWord __attribute__((may_alias));
        const auto* a=reinterpret_cast<const AliasWord*>(old);
        const auto* b=reinterpret_cast<const AliasWord*>(target);
        for(;i+4<=bytes;i+=4) {
            const uint32_t av=a[i/4],bv=b[i/4],delta=av^bv;
            if(!delta && (!result.binary || av==0 || av==0xffffffffu))continue;
            const uint32_t changed=EpdNonzeroNibbles(delta);
            result.changed+=(changed*0x11111111u)>>28;
            if(result.binary || !result.gray_changed) {
                const uint32_t gray=EpdGrayNibbles(av)|EpdGrayNibbles(bv);
                result.binary&=gray==0;
                result.gray_changed|=(gray&changed)!=0;
            }
        }
    }
    // Unaligned callers and the final 1..3 bytes preserve the same contract.
    for(;i<bytes;++i) {
        const unsigned av=old[i],bv=target[i],delta=av^bv;
        const uint32_t changed=EpdNonzeroNibbles(delta);
        result.changed+=bool(delta&15)+bool(delta&240);
        if(result.binary || !result.gray_changed) {
            const uint32_t gray=EpdGrayNibbles(av)|EpdGrayNibbles(bv);
            result.binary&=gray==0;
            result.gray_changed|=(gray&changed)!=0;
        }
    }
    return result;
}

// 推白只反转分带顺序，不旋转目标像素；方向值与 display_orient.h 一致。
static inline int EpdWipeOrient(int orient,int direction) {
    return direction > 0 ? (orient ^ 3) : orient;
}

// Preparation only: these routines do not choose waveforms or schedule scans.
static inline __attribute__((always_inline)) uint8_t EpdRevealPixel(unsigned old,unsigned target,int band) {
    return ((old==15 && target==15 ? 15 : band)<<4)|(old==15 ? 1 : 0)|(target==0 ? 2 : 0);
}
static inline __attribute__((always_inline)) void EpdRevealSpan(uint8_t* out,const uint8_t* old,const uint8_t* target,
                                 int first,int end,int band) {
    if(first>=end)return;
    if(first&1) {
        out[first]=EpdRevealPixel(old[first/2]>>4,target[first/2]>>4,band);++first;
    }
    for(;first+1<end;first+=2) {
        const unsigned a=old[first/2],b=target[first/2];
        out[first]=EpdRevealPixel(a&15,b&15,band);
        out[first+1]=EpdRevealPixel(a>>4,b>>4,band);
    }
    if(first<end)out[first]=EpdRevealPixel(old[first/2]&15,target[first/2]&15,band);
}
[[maybe_unused]] static __attribute__((noinline,optimize("O3"))) void EpdPackBinaryReveal(
    uint8_t* out,const uint8_t* old,const uint8_t* target,int width,int height,int orient) {
    for(int y=0;y<height;++y) {
        auto* dst=out+y*width;const auto* a=old+y*(width/2);const auto* b=target+y*(width/2);
        if(orient==1 || orient==2) {
            for(int band=0;band<10;++band) {
                const int lo=(band*width+9)/10,hi=((band+1)*width+9)/10;
                EpdRevealSpan(dst,a,b,orient==1?lo:width-hi,orient==1?hi:width-lo,band);
            }
        } else {
            const int band=(orient==3?y:height-1-y)*10/height;
            EpdRevealSpan(dst,a,b,0,width,band);
        }
    }
}
[[maybe_unused]] static __attribute__((noinline,optimize("O3"))) void EpdPackDifference(
    uint8_t* out,const uint8_t* old,const uint8_t* target,size_t bytes) {
    for(size_t i=0;i<bytes;++i) {
        const unsigned a=old[i],b=target[i];
        out[2*i]=((b&15)<<4)|(a&15);out[2*i+1]=(b&240)|(a>>4);
    }
}
// Convert the final LVGL L8 pixels, never reconstruct content from UI objects.
// Tile rotated writes for locality. Preserve the other nibble at dirty edges.
[[maybe_unused]] static __attribute__((noinline,optimize("O3"))) void EpdBlitL8(
    uint8_t* fb, const uint8_t* pixels, int stride, int origin_x, int origin_y,
    int x1, int y1, int x2, int y2, int panel_w, int panel_h,
    int base_px, int base_py, int dx_px, int dx_py, int dy_px, int dy_py) {
    for (int ty=y1; ty<=y2; ty+=16) for (int tx=x1; tx<=x2; tx+=16) {
        const int ey=ty+15<y2 ? ty+15 : y2, ex=tx+15<x2 ? tx+15 : x2;
        for (int y=ty; y<=ey; ++y) {
            const auto* row=pixels+size_t(y-origin_y)*stride;
            int px=base_px+(tx-x1)*dx_px+(y-y1)*dy_px;
            int py=base_py+(tx-x1)*dx_py+(y-y1)*dy_py;
            for (int x=tx; x<=ex; ++x,px+=dx_px,py+=dx_py) {
                if (unsigned(px)>=unsigned(panel_w) || unsigned(py)>=unsigned(panel_h)) continue;
                const unsigned gray=(unsigned(row[x-origin_x])*15+127)/255;
                auto& dst=fb[size_t(py)*(panel_w/2)+px/2];
                const unsigned shift=(px&1)*4;
                dst=(dst & ~(15u<<shift)) | (gray<<shift);
            }
        }
    }
}

// Affine coordinates captured once per flush. Tiles keep rotated PSRAM writes local.
[[maybe_unused]] static __attribute__((noinline,optimize("O3"))) void EpdBlitI1Scalar(
    uint8_t* fb,const uint8_t* bits,int stride,int origin_x,int origin_y,
    int x1,int y1,int x2,int y2,int panel_w,int panel_h,
    int base_px,int base_py,int dx_px,int dx_py,int dy_px,int dy_py) {
    for(int ty=y1;ty<=y2;ty+=16)for(int tx=x1;tx<=x2;tx+=16) {
        const int ey=ty+15<y2?ty+15:y2,ex=tx+15<x2?tx+15:x2;
        for(int y=ty;y<=ey;++y) {
            const auto* row=bits+size_t(y-origin_y)*stride;
            int px=base_px+(tx-x1)*dx_px+(y-y1)*dy_px;
            int py=base_py+(tx-x1)*dx_py+(y-y1)*dy_py;
            for(int x=tx;x<=ex;++x,px+=dx_px,py+=dx_py) {
                if(unsigned(px)>=unsigned(panel_w)||unsigned(py)>=unsigned(panel_h))continue;
                const int bit=x-origin_x;
                const unsigned white=(row[bit>>3]>>(7-(bit&7)))&1;
                auto& dst=fb[py*(panel_w/2)+px/2];
                const unsigned shift=(px&1)*4,mask=15u<<shift;
                dst=(dst&~mask)|((0u-white)&mask);
            }
        }
    }
}

static inline __attribute__((always_inline)) unsigned EpdI1White(const uint8_t* row,int bit) {
    return (row[bit>>3]>>(7-(bit&7)))&1;
}
static inline __attribute__((always_inline)) void EpdWriteBinaryPixel(uint8_t* fb,int px,int py,
    int panel_w,int panel_h,unsigned white) {
    if(unsigned(px)>=unsigned(panel_w)||unsigned(py)>=unsigned(panel_h))return;
    auto& dst=fb[py*(panel_w/2)+px/2];
    const unsigned shift=(px&1)*4,mask=15u<<shift;
    dst=(dst&~mask)|((0u-white)&mask);
}
// Pair in the logical axis that maps to physical X: a full byte can be stored
// without reading PSRAM. Odd/clipped edges retain the neighbouring nibble.
[[maybe_unused]] static __attribute__((noinline,optimize("O3"))) void EpdBlitI1(
    uint8_t* fb,const uint8_t* bits,int stride,int origin_x,int origin_y,
    int x1,int y1,int x2,int y2,int panel_w,int panel_h,
    int base_px,int base_py,int dx_px,int dx_py,int dy_px,int dy_py) {
    const bool portrait=dx_px==0 && dy_py==0 && (dx_py==1||dx_py==-1) && (dy_px==1||dy_px==-1);
    const bool landscape=dx_py==0 && dy_px==0 && (dx_px==1||dx_px==-1) && (dy_py==1||dy_py==-1);
    // Device A/B found no gain for tiny landscape dirty areas; retain that path.
    if((!portrait && !landscape) || (landscape && int64_t(x2-x1+1)*(y2-y1+1)<=4096)) {
        EpdBlitI1Scalar(fb,bits,stride,origin_x,origin_y,x1,y1,x2,y2,panel_w,panel_h,
                        base_px,base_py,dx_px,dx_py,dy_px,dy_py);
        return;
    }
    for(int ty=y1;ty<=y2;ty+=16)for(int tx=x1;tx<=x2;tx+=16) {
        const int ey=ty+15<y2?ty+15:y2,ex=tx+15<x2?tx+15:x2;
        if(portrait) {
            for(int y=ty;y<=ey;) {
                const int px=base_px+(y-y1)*dy_px;
                const int neighbour=px+dy_px;
                const bool pair=y<ey && (px&1)==(dy_px<0) && unsigned(px)<unsigned(panel_w) && unsigned(neighbour)<unsigned(panel_w);
                const auto* row=bits+size_t(y-origin_y)*stride;
                const auto* next=pair?row+stride:row;
                const int offset=pair?(px<neighbour?px:neighbour)/2:px/2;
                int py=base_py+(tx-x1)*dx_py;
                for(int x=tx;x<=ex;++x,py+=dx_py) {
                    const unsigned first=EpdI1White(row,x-origin_x);
                    if(pair) {
                        if(unsigned(py)>=unsigned(panel_h))continue;
                        const unsigned second=EpdI1White(next,x-origin_x);
                        fb[py*(panel_w/2)+offset]=dy_px>0 ? first*15+second*240 : first*240+second*15;
                    } else EpdWriteBinaryPixel(fb,px,py,panel_w,panel_h,first);
                }
                y+=pair?2:1;
            }
        } else {
            for(int y=ty;y<=ey;++y) {
                const auto* row=bits+size_t(y-origin_y)*stride;
                const int py=base_py+(y-y1)*dy_py;
                if(unsigned(py)>=unsigned(panel_h))continue;
                int px=base_px+(tx-x1)*dx_px;
                for(int x=tx;x<=ex;) {
                    const int neighbour=px+dx_px;
                    const bool pair=x<ex && (px&1)==(dx_px<0) && unsigned(px)<unsigned(panel_w) && unsigned(neighbour)<unsigned(panel_w);
                    const unsigned first=EpdI1White(row,x-origin_x);
                    if(pair) {
                        const unsigned second=EpdI1White(row,x+1-origin_x);
                        const int offset=(px<neighbour?px:neighbour)/2;
                        fb[py*(panel_w/2)+offset]=dx_px>0 ? first*15+second*240 : first*240+second*15;
                    } else EpdWriteBinaryPixel(fb,px,py,panel_w,panel_h,first);
                    const int step=pair?2:1;x+=step;px+=step*dx_px;
                }
            }
        }
    }
}

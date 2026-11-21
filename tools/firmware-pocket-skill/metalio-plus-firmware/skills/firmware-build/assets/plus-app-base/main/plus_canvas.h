#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace plus_ui {
constexpr int Width=684, Height=1216;
constexpr std::size_t FrameBytes=1216*684/2;
struct Rect {int x,y,w,h;};
inline bool Contains(const Rect& r,int x,int y) {
    return x>=r.x && y>=r.y && x<r.x+r.w && y<r.y+r.h;
}
// Same logical coordinates are consumed by PollTouch and layout hit testing.
// Native even-X LOW nibble, not an LVGL/packed-font byte order.
inline void Pixel(uint8_t* out,int x,int y,uint8_t gray=0) {
    if(!out||x<0||y<0||x>=Width||y>=Height)return;
    const int px=y,py=Width-1-x;auto& b=out[py*608+px/2];
    if(px&1)b=uint8_t((b&15)|((gray&15)<<4));
    else b=uint8_t((b&240)|(gray&15));
}
inline void Clear(uint8_t* out) {std::memset(out,255,FrameBytes);}
inline void Fill(uint8_t* out,const Rect& r,uint8_t gray=0) {
    for(int y=r.y;y<r.y+r.h;++y)for(int x=r.x;x<r.x+r.w;++x)Pixel(out,x,y,gray);
}
inline void Box(uint8_t* out,const Rect& r,int thickness=2) {
    Fill(out,{r.x,r.y,r.w,thickness});Fill(out,{r.x,r.y+r.h-thickness,r.w,thickness});
    Fill(out,{r.x,r.y,thickness,r.h});Fill(out,{r.x+r.w-thickness,r.y,thickness,r.h});
}
// Diagnostic 5x7 ASCII only. User Chinese UI MUST bind a real covered font.
inline const char* Glyph(char c) {
    switch(c) {
    case 'A':return "01110100011000111111100011000110001";
    case 'B':return "11110100011000111110100011000111110";
    case 'C':return "01111100001000010000100001000001111";
    case 'D':return "11110100011000110001100011000111110";
    case 'E':return "11111100001000011110100001000011111";
    case 'F':return "11111100001000011110100001000010000";
    case 'G':return "01111100001000010111100011000101111";
    case 'H':return "10001100011000111111100011000110001";
    case 'I':return "11111001000010000100001000010011111";
    case 'J':return "00111000100001000010100101001001100";
    case 'K':return "10001100101010011000101001001010001";
    case 'L':return "10000100001000010000100001000011111";
    case 'M':return "10001110111010110101100011000110001";
    case 'N':return "10001110011010110011100011000110001";
    case 'O':return "01110100011000110001100011000101110";
    case 'P':return "11110100011000111110100001000010000";
    case 'Q':return "01110100011000110001101011001001101";
    case 'R':return "11110100011000111110101001001010001";
    case 'S':return "01111100001000001110000010000111110";
    case 'T':return "11111001000010000100001000010000100";
    case 'U':return "10001100011000110001100011000101110";
    case 'V':return "10001100011000110001100010101000100";
    case 'W':return "10001100011000110101101011101110001";
    case 'X':return "10001100010101000100010101000110001";
    case 'Y':return "10001100010101000100001000010000100";
    case 'Z':return "11111000010001000100010001000011111";
    case '0':return "01110100011001110101110011000101110";
    case '1':return "00100011000010000100001000010001110";
    case '2':return "01110100010000100010001000100011111";
    case '3':return "11110000010000101110000010000111110";
    case '4':return "00010001100101010010111110001000010";
    case '5':return "11111100001000011110000010000111110";
    case '6':return "01110100001000011110100011000101110";
    case '7':return "11111000010001000100010000100001000";
    case '8':return "01110100011000101110100011000101110";
    case '9':return "01110100011000101111000010000101110";
    default:return "00000000000000000000000000000000000";
    }
}
inline void Label(uint8_t* out,int x,int y,const char* text,int scale=3) {
    for(;*text;++text,x+=6*scale) {
        const char* g=Glyph(*text);
        for(int yy=0;yy<7;++yy)for(int xx=0;xx<5;++xx)
            if(g[yy*5+xx]=='1')Fill(out,{x+xx*scale,y+yy*scale,scale,scale});
    }
}
}

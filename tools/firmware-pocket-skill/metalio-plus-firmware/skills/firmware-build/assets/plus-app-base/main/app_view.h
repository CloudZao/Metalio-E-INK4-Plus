#pragma once
#include "plus_font.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
namespace plus_app {
struct Rect{int x=0,y=0,w=0,h=0;};
inline bool Hit(const Rect& r,int x,int y){return r.w>0&&r.h>0&&x>=r.x&&y>=r.y&&x<r.x+r.w&&y<r.y+r.h;}
struct View{
    int width=684,height=1216;
    std::size_t Stride()const{return (std::size_t(width)+7)/8;}
    std::size_t Bytes()const{return Stride()*std::size_t(height);}
    bool Valid()const{return (width==684&&height==1216)||(width==1216&&height==684);}
};
enum class Page{Home,Help,ExitConfirm,Failure};
struct Model{
    bool integrated=false,wifi_enabled=false,wifi_connected=false;
    int battery=-1;unsigned touches=0;Page page=Page::Home;
    const char* title="能力演示";
    const char* status="请按需求接入业务；当前计数不保存。";
    const char* help="";std::size_t help_offset=0;
};
struct Layout{Rect title,back,help,refresh,wifi,battery,action,reset,previous,next,confirm,cancel;};
inline std::size_t NextUtf8(const char* s,std::size_t n,std::size_t p){
    if(p>=n)return n;
    const unsigned c=static_cast<unsigned char>(s[p]);
    const std::size_t k=c<128?1:(c>=0xC2&&c<=0xDF?2:(c>=0xE0&&c<=0xEF?3:(c>=0xF0&&c<=0xF4?4:1)));
    if(p+k>n)return p+1;
    for(std::size_t i=1;i<k;++i)if((static_cast<unsigned char>(s[p+i])&0xC0)!=0x80)return p+1;
    return p+k;
}
inline int Width(const plus_font::Font& f,const char* s,std::size_t n){
    const auto r=plus_font::MeasureText(f,s,n);return r.status==plus_font::Status::Ok?r.width:0;
}
inline std::string Fit(const plus_font::Font& f,const char* s,int width){
    const auto n=std::strlen(s);if(Width(f,s,n)<=width)return s;
    const int reserve=Width(f,"...",3);std::size_t p=0;
    while(p<n){const auto q=NextUtf8(s,n,p);if(Width(f,s,q)+reserve>width)break;p=q;}
    return std::string(s,p)+(reserve<=width?"...":"");
}
inline Layout MakeLayout(const plus_font::Font& font,const View& v,const Model& m){
    Layout a;int right=v.width-16;
    a.battery={right-76,12,76,52};right-=84;
    if(m.wifi_enabled){a.wifi={right-68,12,68,52};right-=76;}
    a.refresh={right-80,8,80,60};right-=88;a.help={right-80,8,80,60};right-=88;
    const int available=std::max(0,right-16-(m.integrated?164:0));
    const int title_width=std::min(available,Width(font,m.title,std::strlen(m.title))+8);
    a.title={16,8,title_width,60};if(m.integrated)a.back={28+title_width,8,152,60};
    a.action={24,230,v.width-48,96};a.reset={24,350,v.width-48,72};
    a.previous={24,v.height-86,180,62};a.next={v.width-204,v.height-86,180,62};
    a.cancel={24,300,(v.width-72)/2,78};a.confirm={48+(v.width-72)/2,300,(v.width-72)/2,78};
    if(m.page==Page::Failure){a.action={};a.reset={};a.back={};}
    return a;
}
inline void Dot(plus_font::Surface1bpp s,int x,int y,bool black=true){
    if(x<0||y<0||x>=s.width||y>=s.height)return;
    auto& b=s.data[std::size_t(y)*s.stride_bytes+std::size_t(x)/8];const auto mask=uint8_t(0x80u>>(x&7));
    if(black==s.one_is_black)b|=mask;else b&=uint8_t(~mask);
}
inline void Box(plus_font::Surface1bpp s,const Rect& r){
    for(int i=0;i<2;++i){for(int x=r.x;x<r.x+r.w;++x){Dot(s,x,r.y+i);Dot(s,x,r.y+r.h-1-i);}
        for(int y=r.y;y<r.y+r.h;++y){Dot(s,r.x+i,y);Dot(s,r.x+r.w-1-i,y);}}
}
inline void WifiIcon(plus_font::Surface1bpp s,const Rect& r,bool connected){
    const int cx=r.x+r.w/2,cy=r.y+r.h-6;
    for(int dy=-37;dy<=0;++dy)for(int dx=-37;dx<=37;++dx){
        const int d=dx*dx+dy*dy;
        if(dx>=dy&&dx<=-dy&&((d>=1156&&d<=1369)||(d>=484&&d<=625)||(d>=100&&d<=169)))Dot(s,cx+dx,cy+dy);
    }
    for(int dy=-3;dy<=3;++dy)for(int dx=-3;dx<=3;++dx)if(dx*dx+dy*dy<=9)Dot(s,cx+dx,cy+dy);
    if(!connected)for(int i=0;i<46;++i)for(int t=-1;t<=1;++t)Dot(s,cx-23+i,cy-42+i+t);
}
inline plus_font::TextResult Label(const plus_font::Font& f,plus_font::Surface1bpp s,const Rect& r,const char* t,bool border=false){
    if(border)Box(s,r);
    const auto fitted=Fit(f,t,std::max(0,r.w-8));
    const int h=f.metrics().ascent+f.metrics().descent;
    return plus_font::DrawText(f,s,fitted.data(),fitted.size(),r.x+4,r.y+std::max(0,(r.h-h)/2)+f.metrics().ascent);
}
struct PageResult{std::size_t next=0,missing=0,invalid=0;};
// The demo wraps on codepoints using real advances. Readers may add word and
// punctuation rules without changing font metrics or the production renderer.
inline PageResult Paragraph(const plus_font::Font& f,plus_font::Surface1bpp s,const Rect& box,const char* text,std::size_t offset=0){
    PageResult result;const auto n=std::strlen(text);std::size_t p=std::min(offset,n);
    for(int y=box.y;y+f.metrics().ascent+f.metrics().descent<=box.y+box.h;y+=f.metrics().line_height){
        if(p>=n)break;
        const std::size_t start=p;std::size_t end=p;
        while(end<n&&text[end]!='\n'&&text[end]!='\r'){
            const auto next=NextUtf8(text,n,end);if(end>start&&Width(f,text+start,next-start)>box.w)break;end=next;
        }
        const auto r=plus_font::DrawText(f,s,text+start,end-start,box.x,y+f.metrics().ascent);
        result.missing+=r.missing_glyphs;result.invalid+=r.invalid_utf8;p=end;
        if(p<n&&text[p]=='\r'){++p;if(p<n&&text[p]=='\n')++p;}else if(p<n&&text[p]=='\n')++p;
    }
    result.next=p;return result;
}
inline PageResult Render(const plus_font::Font& font,uint8_t* pixels,std::size_t bytes,const View& v,const Model& m,Layout* output=nullptr){
    if(!pixels||!v.Valid()||bytes<v.Bytes())return {};
    std::memset(pixels,0,v.Bytes());plus_font::Surface1bpp s{pixels,bytes,v.width,v.height,v.Stride(),true};
    const Layout a=MakeLayout(font,v,m);if(output)*output=a;
    PageResult result;
    auto label=[&](const Rect& r,const char* text,bool border=false){
        const auto t=Label(font,s,r,text,border);result.missing+=t.missing_glyphs;result.invalid+=t.invalid_utf8;
    };
    auto paragraph=[&](const Rect& r,const char* text,std::size_t offset=0){
        const auto p=Paragraph(font,s,r,text,offset);result.missing+=p.missing;result.invalid+=p.invalid;result.next=p.next;
    };
    label(a.title,m.title);if(m.integrated&&m.page!=Page::Failure)label(a.back,"返回口袋",true);
    label(a.help,"帮助",true);label(a.refresh,"研墨",true);
    if(m.wifi_enabled)WifiIcon(s,a.wifi,m.wifi_connected);
    Box(s,{a.battery.x,a.battery.y+8,a.battery.w-6,36});
    for(int y=a.battery.y+18;y<a.battery.y+34;++y)for(int x=a.battery.x+a.battery.w-6;x<a.battery.x+a.battery.w;++x)Dot(s,x,y);
    char battery[12];if(m.battery<0)std::snprintf(battery,sizeof battery,"?");else std::snprintf(battery,sizeof battery,"%d",std::min(100,m.battery));
    label({a.battery.x+2,a.battery.y,a.battery.w-12,a.battery.h},battery);
    for(int x=16;x<v.width-16;++x)Dot(s,x,80);
    if(m.page==Page::Failure){
        paragraph({24,110,v.width-48,300},m.status);
        paragraph({24,440,v.width-48,200},"未通过启动检查。请根据提示检查后重新启动。\n不要因这条提示清空存档或格式化存储卡。");
        return result;
    }
    if(m.page==Page::Help){
        const int top=m.help_offset==0?280:100;
        if(m.help_offset==0)paragraph({24,100,v.width-48,170},"短按锁屏；长按五秒深休或唤醒。\n十分钟无操作自动深休。\n研墨不清业务。以下为完整开源许可：");
        paragraph({24,top,v.width-48,v.height-top-108},m.help,m.help_offset);
        label(a.previous,"上一页",true);label(a.next,"下一页",true);return result;}
    if(m.page==Page::ExitConfirm){
        paragraph({24,110,v.width-48,160},"保存当前业务并返回固件口袋？\n保存失败时保留当前页面。");
        label(a.cancel,"取消",true);label(a.confirm,"保存并返回",true);return result;}
    paragraph({24,105,v.width-48,112},m.status);char count[80];std::snprintf(count,sizeof count,"触摸计数：%u",m.touches);
    label(a.action,count,true);label(a.reset,"清零演示计数",true);
    paragraph({24,460,v.width-48,v.height-484},"短按锁屏/恢复，长按五秒深休与唤醒。\n十分钟无操作自动深休；帮助可查许可。\n计数不保存，正式业务需接入存档。");
    return result;
}
inline bool ToNative4(const uint8_t* mono,std::size_t bytes,const View& v,uint8_t* out,std::size_t out_bytes){
    if(!v.Valid()||!mono||!out||bytes<v.Bytes()||out_bytes!=415872)return false;
    std::memset(out,255,out_bytes);
    for(int y=0;y<v.height;++y)for(int x=0;x<v.width;++x){
        const bool ink=(mono[std::size_t(y)*v.Stride()+std::size_t(x)/8]&(0x80u>>(x&7)))!=0;
        const int px=v.width==684?y:x,py=v.width==684?683-x:y;
        auto& b=out[std::size_t(py)*608+std::size_t(px)/2];const uint8_t gray=ink?0:15;
        if(px&1)b=uint8_t((b&15)|(gray<<4));else b=uint8_t((b&240)|gray);
    }return true;
}
}

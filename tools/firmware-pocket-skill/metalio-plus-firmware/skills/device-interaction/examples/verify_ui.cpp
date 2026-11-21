#include "app_view.h"
#include "app_lifecycle.h"
#include "font.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>
#include <limits>
namespace {
void Write(const std::string& path,const void* data,std::size_t bytes){
    std::ofstream out(path,std::ios::binary);out.write(static_cast<const char*>(data),static_cast<std::streamsize>(bytes));assert(out.good());
}
bool Overlap(const plus_app::Rect& a,const plus_app::Rect& b){
    return a.w>0&&b.w>0&&a.h>0&&b.h>0&&a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h;
}
void Lifecycle(){
    using namespace plus_app;
    IdleSleep idle(1);assert(!idle.Due(600000000));assert(idle.Due(600000001));
    idle.BeginUserLongTask();assert(!idle.Due(90000000000ULL));
    idle.BeginUserLongTask();idle.EndUserLongTask(90000000000ULL);assert(!idle.Due(99900000000ULL));
    idle.EndUserLongTask(99900000000ULL);assert(!idle.Due(99900000001ULL));assert(idle.Due(100500000000ULL));
    idle.Requested();assert(!idle.Due(100500000001ULL));idle.Refused(100500000001ULL);
    assert(!idle.Due(100500000002ULL));assert(idle.Due(100560000001ULL));
    idle.Reset(7776000000000ULL);assert(!idle.Due(7776000000001ULL));
    idle.Activity(7776300000000ULL);assert(!idle.Due(7776800000000ULL));assert(idle.Due(7776900000000ULL));
    FrameFlow f;assert(f.CanSubmit(false,false));assert(f.WantsFull());assert(!f.SceneVisible());
    f.Submitted();f.Changed();assert(!f.CanSubmit(false,false));assert(!f.Observe(0,true,true));
    assert(f.Observe(1,true,false));assert(f.SceneVisible());assert(f.CanSubmit(false,false));assert(!f.WantsFull());
    f.Submitted();f.NewScene();assert(!f.SceneVisible());f.Observe(2,true,false);assert(!f.SceneVisible());
    f.Submitted();f.Observe(3,true,false);assert(f.SceneVisible());
    f.ManualFull();assert(f.WantsFull());f.Submitted();f.Observe(3,false,false);
    assert(f.WantsFull()&&f.CanSubmit(false,false));f.Submitted();f.Observe(3,false,false);
    assert(f.Failed()&&!f.CanSubmit(false,false));f.ManualFull();assert(f.CanSubmit(false,false));
    assert(!f.CanSubmit(true,false));assert(!f.CanSubmit(false,true));
}
}
int main(int argc,char** argv){
    if(argc!=2&&argc!=3)return 2;
    Lifecycle();plus_font::Font font;assert(font.Open(plus_font_data,plus_font_data_size));
    assert(std::strstr(plus_font_data_license,"SIL OPEN FONT LICENSE"));
    assert(std::strstr(plus_font_data_license,"TERMINATION"));
    unsigned scenes=0,pixels_checked=0,license_pages=0;
    for(int orientation=0;orientation<2;++orientation){
        plus_app::View v;if(orientation){v.width=1216;v.height=684;}
        for(int integrated=0;integrated<2;++integrated)for(int wifi=0;wifi<2;++wifi){
            plus_app::Model m;m.integrated=integrated!=0;m.wifi_enabled=wifi!=0;m.battery=100;m.help=plus_font_data_license;
            const auto a=plus_app::MakeLayout(font,v,m);
            const plus_app::Rect top[]={a.title,a.back,a.help,a.refresh,a.wifi,a.battery};
            for(std::size_t i=0;i<6;++i){assert(top[i].x>=0&&top[i].y>=0&&top[i].x+top[i].w<=v.width);
                for(std::size_t j=i+1;j<6;++j)assert(!Overlap(top[i],top[j]));}
            assert((a.back.w>0)==m.integrated);assert((a.wifi.w>0)==m.wifi_enabled);
            assert(plus_app::Fit(font,m.title,a.title.w-8)==m.title);
            std::vector<uint8_t> mono(v.Bytes()+16,0xA5),native(415872+16,0xA5);
            plus_app::Layout actual;
            const auto rendered=plus_app::Render(font,mono.data(),v.Bytes(),v,m,&actual);
            assert(!rendered.missing&&!rendered.invalid);
            assert(plus_app::ToNative4(mono.data(),v.Bytes(),v,native.data(),415872));
            for(std::size_t i=v.Bytes();i<mono.size();++i)assert(mono[i]==0xA5);
            for(std::size_t i=415872;i<native.size();++i)assert(native[i]==0xA5);
            for(int y=0;y<v.height;++y)for(int x=0;x<v.width;++x){
                const bool black=(mono[std::size_t(y)*v.Stride()+std::size_t(x)/8]&(0x80u>>(x&7)))!=0;
                const int px=orientation?x:y,py=orientation?y:683-x;
                const auto packed=native[std::size_t(py)*608+std::size_t(px)/2];
                assert(((packed>>(px&1?4:0))&15)==(black?0:15));++pixels_checked;
            }
            assert(plus_app::Hit(actual.action,actual.action.x,actual.action.y));
            assert(!plus_app::Hit(actual.action,actual.action.x+actual.action.w,actual.action.y));
            if(!orientation){
                const std::string p=std::string(argv[1])+(integrated?"/pocket":"/standalone")+(wifi?"-wifi":"");
                Write(p+".gray4",native.data(),415872);
                const std::string header="P4\n684 1216\n";
                std::ofstream pbm(p+".pbm",std::ios::binary);pbm<<header;
                pbm.write(reinterpret_cast<const char*>(mono.data()),static_cast<std::streamsize>(v.Bytes()));assert(pbm.good());
            }
            m.page=plus_app::Page::Help;m.help_offset=0;
            while(m.help_offset<std::strlen(m.help)){
                const auto page=plus_app::Render(font,mono.data(),v.Bytes(),v,m);
                assert(page.next>m.help_offset&&!page.missing&&!page.invalid);m.help_offset=page.next;++license_pages;
                assert(license_pages<500);
            }
            m.page=plus_app::Page::ExitConfirm;plus_app::Render(font,mono.data(),v.Bytes(),v,m);
            m.page=plus_app::Page::Failure;m.status="协议或设备绑定不兼容，请核对接入要求。";
            const auto failure=plus_app::Render(font,mono.data(),v.Bytes(),v,m,&actual);
            assert(!failure.missing&&!failure.invalid);
            assert(actual.action.w==0&&actual.reset.w==0&&actual.back.w==0);
            assert(actual.help.w>0&&actual.refresh.w>0);
            ++scenes;
        }
    }
    // An optional full generated-license UTF-8 file tests the actual app text.
    // Otherwise exercise >150KB to prove there is no fixed 64-page truncation.
    std::string large_license;
    if(argc==3){std::ifstream input(argv[2],std::ios::binary);assert(input.good());
        large_license.assign(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());}
    else while(large_license.size()<150000)large_license+=plus_font_data_license;
    plus_app::View v;plus_app::Model m;m.page=plus_app::Page::Help;m.help=large_license.c_str();
    std::vector<uint8_t> mono(v.Bytes());unsigned long_pages=0;
    while(m.help_offset<large_license.size()){
        const auto p=plus_app::Render(font,mono.data(),mono.size(),v,m);
        assert(p.next>m.help_offset&&!p.missing&&!p.invalid);m.help_offset=p.next;++long_pages;
        assert(long_pages<10000);
    }
    assert(m.help_offset==large_license.size());
    std::cout<<"PASS production renderer: layouts="<<scenes<<" pixel comparisons="<<pixels_checked
             <<" license pages="<<license_pages<<" long-license bytes="<<large_license.size()<<" pages="<<long_pages
             <<"; idle and frame-state assertions passed; hardware NOT tested\n";
}

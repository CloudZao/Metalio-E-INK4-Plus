#include "app_util.h"
#include <limits.h>
static uint32_t value;
static unsigned base;
static void* values[4];
static void* tabs[4];
static void* keys[16];
static void* hint;
static const unsigned bases[]={2,8,10,16};
static void BaseText(uint32_t v,unsigned b) {
    char digits[33];unsigned n=0;const char* alphabet="0123456789ABCDEF";
    do {digits[n++]=alphabet[v%b];v/=b;}while(v);
    while(n){char part[2]={digits[--n],0};Add(part);}
}
static void BinaryText(uint32_t v) {
    for(int bit=31;bit>=0;--bit) {
        char digit[2]={(char)('0'+((v>>bit)&1u)),0};Add(digit);
        if(bit==16) Add("\n"); else if(bit && bit%8==0) Add(" ");
    }
}
static void Refresh(void) {
    for(int i=0;i<4;++i) {
        ResetText();if(i==0)BinaryText(value);else BaseText(value,bases[i]);app->set_text(values[i],text);
        AppStyle(tabs[i],30,XZ_APP_ALIGN_CENTER,1,base==bases[i]);
    }
    for(unsigned i=0;i<16;++i) {AppStyle(keys[i],36,XZ_APP_ALIGN_CENTER,1,0);AppEnabled(keys[i],i<base);}
    app->set_text(hint,zh?"选择输入进制，结果同步换算":"Choose an input base; results update live");
}
static void Click(void* context) {
    const unsigned key=(unsigned)(uintptr_t)context;
    if(key>=32 && key<36) base=bases[key-32];
    else if(key==16)value=0;
    else if(key==17)value/=base;
    else if(key<16) {
        if(key>=base){app->set_text(hint,zh?"当前进制不支持此数字":"Digit not valid in this base");return;}
        if(value>(UINT32_MAX-key)/base){app->set_text(hint,zh?"超出 32 位范围，数值未改变":"32-bit limit exceeded; value unchanged");return;}
        value=value*base+key;
    } else return;
    Refresh();
}
int app_main(int argc,char** argv) {
    if(!AppInit(argc,argv) || app->struct_size<offsetof(xz_app_api_t,set_enabled)+sizeof(app->set_enabled) || !app->set_enabled)return -1;
    value=0;base=10;
    const int pad=24,gap=12,width=app->width-pad*2,w=(width-gap*3)/4;
    AppStyle(Label(pad,18,width-300,48,zh?"进制换算":"Base converter"),30,XZ_APP_ALIGN_LEFT,0,0);
    AppStyle(Label(app->width-pad-280,22,280,40,zh?"32 位无符号":"Unsigned 32-bit"),25,XZ_APP_ALIGN_RIGHT,0,0);
    static const char* const names[]={"BIN","OCT","DEC","HEX"};
    for(int i=0;i<4;++i) {tabs[i]=Button(pad+i*(w+gap),80,w,60,names[i],Click,32+i);if(!tabs[i])return -1;}
    AppStyle(Label(pad,164,width,326,""),30,XZ_APP_ALIGN_LEFT,1,0);
    static const int y[]={184,276,344,412};
    for(int i=0;i<4;++i) {
        AppStyle(Label(pad+18,y[i]+8,84,38,names[i]),25,XZ_APP_ALIGN_LEFT,0,0);
        values[i]=Label(pad+112,y[i],width-132,i==0?72:52,"");AppStyle(values[i],i==0?25:36,XZ_APP_ALIGN_RIGHT,0,0);if(!values[i])return -1;
    }
    hint=Label(pad,514,width,44,"");AppStyle(hint,25,XZ_APP_ALIGN_CENTER,0,0);
    void* clear=Button(pad,572,(width-gap)/2,60,zh?"清空":"Clear",Click,16);AppStyle(clear,30,XZ_APP_ALIGN_CENTER,2,0);
    void* back=Button(pad+(width+gap)/2,572,(width-gap)/2,60,zh?"退格":"Back",Click,17);AppStyle(back,30,XZ_APP_ALIGN_CENTER,2,0);
    const int top=654,h=(app->height-top-38-gap*3)/4;
    for(unsigned i=0;i<16;++i) {char digit[2]={"0123456789ABCDEF"[i],0};keys[i]=Button(pad+(int)(i%4)*(w+gap),top+(int)(i/4)*(h+gap),w,h,digit,Click,i);if(!keys[i])return -1;}
    AppStyle(Label(pad,app->height-30,width,30,zh?"最大值：4 294 967 295":"Maximum: 4 294 967 295"),25,XZ_APP_ALIGN_CENTER,0,0);
    if(!hint || !clear || !back)return -1;
    Refresh();return 0;
}

#include "app_util.h"
#include "decimal.h"
typedef struct {const char* name; double scale; double offset;} Unit;
static const Unit units[][6]={
    {{"m",1,0},{"cm",.01,0},{"mm",.001,0},{"km",1000,0},{"in",.0254,0},{"ft",.3048,0}},
    {{"kg",1,0},{"g",.001,0},{"lb",.45359237,0},{"oz",.028349523125,0}},
    {{"°C",1,0},{"°F",5.0/9,-32.0*5/9},{"K",1,-273.15}},
    {{"m²",1,0},{"cm²",.0001,0},{"km²",1000000,0},{"ha",10000,0}}
};
static const unsigned counts[]={6,4,3,4};
static unsigned category,from,to,negative,input_len;
static char input[18];
static void* input_label;
static void* result_label;
static void* hint;
static void* tabs[4];
static void* sources[6];
static void* targets[6];
static int Convert(double value,double* result) {
    double standard=value*units[category][from].scale+units[category][from].offset;
    if(category==2) {
        if(standard < -273.15-1e-10)return 0;
        if(standard < -273.15)standard=-273.15;
    }
    *result=(standard-units[category][to].offset)/units[category][to].scale;
    return *result>-1e100 && *result<1e100;
}
static void Refresh(void) {
    for(unsigned i=0;i<4;++i) AppStyle(tabs[i],30,XZ_APP_ALIGN_CENTER,1,i==category);
    for(unsigned i=0;i<6;++i) {
        const int valid=i<counts[category];
        app->set_text(sources[i],valid?units[category][i].name:"");app->set_text(targets[i],valid?units[category][i].name:"");
        AppStyle(sources[i],30,XZ_APP_ALIGN_CENTER,valid,valid && i==from);AppStyle(targets[i],30,XZ_APP_ALIGN_CENTER,valid,valid && i==to);
        AppEnabled(sources[i],valid);AppEnabled(targets[i],valid);
    }
    ResetText();if(negative)Add("-");Add(input);app->set_text(input_label,text);
    const double value=AppParseNumber(input)*(negative?-1:1);
    double result;
    const int valid=Convert(value,&result);
    char output[32];
    if(valid) {AppFormatNumber(result,output);app->set_text(result_label,output);}
    else app->set_text(result_label,zh?"无效温度":"Invalid temperature");
    AppStyle(result_label,valid?36:30,XZ_APP_ALIGN_RIGHT,0,0);
    app->set_text(hint,valid?(zh?"选择来源与目标单位，实时换算":"Choose source and target units"):(zh?"温度不能低于绝对零度":"Temperature cannot be below absolute zero"));
}
static void Click(void* context) {
    const unsigned key=(unsigned)(uintptr_t)context;
    if(key>=200 && key<204){category=key-200;from=0;to=1;}
    else if(key>=300 && key<306){if(key-300>=counts[category])return;from=key-300;}
    else if(key>=400 && key<406){if(key-400>=counts[category])return;to=key-400;}
    else if(key==500){const unsigned old=from;from=to;to=old;}
    else if(key=='<'){if(input_len>1)input[--input_len]=0;else{input[0]='0';input[1]=0;negative=0;}}
    else if(key=='N')negative=!negative;
    else if(key=='C'){input[0]='0';input[1]=0;input_len=1;negative=0;}
    else if((key>='0' && key<='9') || key=='.') {
        if(key=='.'){for(unsigned i=0;i<input_len;++i)if(input[i]=='.')return;}
        else {
            unsigned digits=0;for(unsigned i=0;i<input_len;++i)if(input[i]!='.')++digits;
            if(input[0]=='0')--digits;
            if(digits>=12){app->set_text(hint,zh?"最多输入 12 位数字":"Up to 12 input digits");return;}
            if(input_len==1 && input[0]=='0')input_len=0;
        }
        input[input_len++]=(char)key;input[input_len]=0;
    } else return;
    Refresh();
}
int app_main(int argc,char** argv) {
    if(!AppInit(argc,argv) || app->struct_size<offsetof(xz_app_api_t,set_enabled)+sizeof(app->set_enabled) || !app->set_enabled)return -1;
    category=from=negative=0;to=1;input[0]='0';input[1]=0;input_len=1;
    const int pad=24,gap=12,width=app->width-pad*2,w=(width-gap*3)/4,uw=(width-36-8*5)/6;
    AppStyle(Label(pad,18,width,48,zh?"单位换算":"Unit converter"),30,XZ_APP_ALIGN_LEFT,0,0);
    static const char* const cn[]={"长度","质量","温度","面积"};
    static const char* const en[]={"Length","Mass","Temp","Area"};
    for(int i=0;i<4;++i){tabs[i]=Button(pad+i*(w+gap),80,w,60,zh?cn[i]:en[i],Click,200+i);if(!tabs[i])return -1;}
    AppStyle(Label(pad,164,width,156,""),30,XZ_APP_ALIGN_LEFT,1,0);
    AppStyle(Label(pad,340,width,156,""),30,XZ_APP_ALIGN_LEFT,1,0);
    AppStyle(Label(pad+18,182,128,40,zh?"输入":"Input"),25,XZ_APP_ALIGN_LEFT,0,0);
    AppStyle(Label(pad+18,358,128,40,zh?"结果":"Result"),25,XZ_APP_ALIGN_LEFT,0,0);
    input_label=Label(pad+156,180,width-174,60,"");AppStyle(input_label,36,XZ_APP_ALIGN_RIGHT,0,0);
    result_label=Label(pad+156,356,width-174,60,"");AppStyle(result_label,36,XZ_APP_ALIGN_RIGHT,0,0);
    for(int i=0;i<6;++i){sources[i]=Button(pad+18+i*(uw+8),248,uw,52,"",Click,300+i);targets[i]=Button(pad+18+i*(uw+8),424,uw,52,"",Click,400+i);if(!sources[i] || !targets[i])return -1;}
    hint=Label(pad,520,width,66,"");AppStyle(hint,25,XZ_APP_ALIGN_CENTER,0,0);
    const int top=600,h=(app->height-top-48-gap*3)/4;
    static const char digits[]={'7','8','9','<','4','5','6','N','1','2','3','C','.','0'};
    for(unsigned i=0;i<14;++i) {
        const char key=digits[i];char symbol[2]={key,0};const char* label=symbol;
        if(key=='<')label=zh?"退格":"Back";else if(key=='N')label="±";else if(key=='C')label="AC";
        void* button=Button(pad+(int)(i%4)*(w+gap),top+(int)(i/4)*(h+gap),key=='0'?w*2+gap:w,h,label,Click,key);
        if(!button)return -1;AppStyle(button,key=='<'?30:36,XZ_APP_ALIGN_CENTER,i%4==3?2:1,0);
    }
    void* swap=Button(pad+3*(w+gap),top+3*(h+gap),w,h,zh?"交换单位":"Swap",Click,500);AppStyle(swap,30,XZ_APP_ALIGN_CENTER,2,0);
    AppStyle(Label(pad,app->height-30,width,30,zh?"12 位有效数字":"12-digit precision"),25,XZ_APP_ALIGN_CENTER,0,0);
    if(!input_label || !result_label || !hint || !swap)return -1;
    Refresh();return 0;
}

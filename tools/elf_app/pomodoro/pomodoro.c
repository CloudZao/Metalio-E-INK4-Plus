#include "app_api.h"
#include "pomodoro_engine.h"

/** @brief 为独立 ELF 的结构体复制提供本地实现 */
void* memcpy(void* destination,const void* source,size_t length){
    unsigned char* out=(unsigned char*)destination;const unsigned char* in=(const unsigned char*)source;
    for(size_t i=0;i<length;++i)out[i]=in[i];
    return destination;
}

/** @brief 为独立 ELF 的结构体初始化提供本地实现 */
void* memset(void* destination,int value,size_t length){
    unsigned char* out=(unsigned char*)destination;
    for(size_t i=0;i<length;++i)out[i]=(unsigned char)value;
    return destination;
}

static const xz_app_api_t* api;
static Pomodoro timer;
static int zh,save_failed;
static uint64_t last_save,reset_until;
static void* stage;
static void* hint;
static void* start_button;
static void* reset_button;
static void* bar;
static void* footer;
static void* modes[3];
static void* segments[4][7];
static int previous_digit[4]={-1,-1,-1,-1};
static uint32_t shown_seconds=UINT32_MAX;
static int shown_phase=-1,shown_running=-1,shown_percent=-1,shown_error=-1;
static char output[192];
static unsigned used;

static void Empty(void){used=0;output[0]=0;}
static void Add(const char* value){while(*value && used+1<sizeof(output))output[used++]=*value++;output[used]=0;}
static void Number(uint32_t value){char b[11];unsigned n=0;do{b[n++]=(char)('0'+value%10);value/=10;}while(value);while(n){char c[2]={b[--n],0};Add(c);}}
static void Style(void* widget,unsigned size,unsigned align,unsigned radius,unsigned border,unsigned black){
    const xz_app_style_t value={(uint16_t)size,(uint8_t)align,(uint8_t)radius,(uint8_t)border,(uint8_t)black};
    if(widget)api->set_style(widget,&value);
}
static void* Label(int x,int y,int w,int h,const char* value,unsigned size,unsigned align){
    void* widget=api->label(api->root,x,y,w,h,value);Style(widget,size,align,0,0,0);return widget;
}
static void Click(void* context);
static void* Button(int x,int y,int w,int h,const char* value,unsigned key,unsigned black){
    void* widget=api->button(api->root,x,y,w,h,value,Click,(void*)(uintptr_t)key);Style(widget,25,XZ_APP_ALIGN_CENTER,12,1,black);return widget;
}
static void Notice(const char* cn,const char* en){api->notify(api->host_context,zh?cn:en);}
static uint32_t Day(void){xz_app_time_t date;if(api->get_local_time(&date)!=XZ_APP_OK)return 0;return (uint32_t)date.year*10000u+(uint32_t)date.month*100u+date.day;}
static void Save(void){
    PomoRecord record=timer.record;record.checksum=PomoHash(&record);
    const int result=api->storage_write(api->host_context,"state",&record,sizeof(record));
    if(result!=XZ_APP_OK && !save_failed)Notice("进度未保存，请检查 SD 卡","Save failed. Check SD card.");
    save_failed=result!=XZ_APP_OK;last_save=api->monotonic_ms();
}
static void Awake(void){
    if(api->set_keep_awake(api->host_context,timer.running)!=XZ_APP_OK && timer.running){timer.running=0;Notice("无法保持计时，请重新打开应用","Timer unavailable. Reopen the app.");}
}
static void Digits(uint32_t seconds){
    static const unsigned char masks[10]={0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f};
    const unsigned minutes=seconds/60;
    const unsigned value[4]={minutes/10,minutes%10,seconds/10%6,seconds%10};
    for(unsigned i=0;i<4;++i){
        const unsigned old=previous_digit[i]<0?0:masks[previous_digit[i]],current=masks[value[i]];
        for(unsigned j=0;j<7;++j)if(previous_digit[i]<0 || ((old^current)&(1u<<j)))Style(segments[i][j],0,XZ_APP_ALIGN_LEFT,4,0,(current>>j)&1u);
        previous_digit[i]=(int)value[i];
    }
}
static void Render(int force){
    const uint32_t seconds=(timer.record.remaining_ms+999u)/1000u;
    const int full=force || (int)timer.record.phase!=shown_phase || timer.running!=shown_running || save_failed!=shown_error;
    if(!full && seconds==shown_seconds)return;
    shown_seconds=seconds;shown_phase=(int)timer.record.phase;shown_running=timer.running;shown_error=save_failed;
    Digits(seconds);
    const int percent=(int)((timer.record.duration_ms-timer.record.remaining_ms)*100u/timer.record.duration_ms);
    if(percent!=shown_percent){api->set_progress(bar,(unsigned)percent);shown_percent=percent;}
    if(!full)return;
    const char* cn[]={"专注时间","短休息","长休息"};const char* en[]={"FOCUS","SHORT BREAK","LONG BREAK"};
    api->set_text(stage,zh?cn[timer.record.phase]:en[timer.record.phase]);
    for(unsigned i=0;i<3;++i)Style(modes[i],25,XZ_APP_ALIGN_CENTER,12,1,i==timer.record.phase);
    const int partial=timer.record.remaining_ms<timer.record.duration_ms;
    api->set_text(start_button,timer.running?(zh?"暂停":"Pause"):(partial?(zh?"继续":"Resume"):(zh?"开始":"Start")));
    api->set_text(reset_button,reset_until?(zh?"确认重置":"Confirm reset"):(zh?"重置":"Reset"));
    Empty();
    if(save_failed)Add(zh?"进度未保存，请检查 SD 卡":"Save failed. Check SD card.");
    else if(!timer.running)Add(partial?(zh?"已暂停，准备好再继续":"Paused. Continue when ready."):(zh?"准备好，就开始吧":"Ready when you are."));
    else{Add(zh?"第 ":"Round ");Number(timer.record.phase==POMO_FOCUS?(timer.record.cycle_done<timer.record.cycle_length?timer.record.cycle_done+1:timer.record.cycle_length):(timer.record.cycle_done?timer.record.cycle_done:1));Add(" / ");Number(timer.record.cycle_length);}
    api->set_text(hint,output);
    Empty();Number(timer.record.minutes[0]);Add(" / ");Number(timer.record.minutes[1]);Add(" / ");Number(timer.record.minutes[2]);Add(zh?" 分钟 · 每 ":" min · Long break every ");Number(timer.record.cycle_length);Add(zh?" 轮长休息":" rounds");api->set_text(footer,output);
}
static void Advance(void){
    const unsigned previous=timer.record.phase;
    if(PomoAdvance(&timer,api->monotonic_ms(),Day())){
        Awake();Save();
        if(previous==POMO_FOCUS)Notice("专注完成，休息一下吧","Focus complete. Take a break.");
        else Notice("休息结束，可以开始下一轮","Break complete. Ready for the next session.");
    }
}
static void Tick(void* context){
    (void)context;Advance();const uint64_t now=api->monotonic_ms();
    const int disarm=reset_until && now>=reset_until;if(disarm)reset_until=0;
    if(timer.running && now-last_save>=60000u)Save();Render(disarm);
}
static void Exit(void* context){(void)context;PomoPause(&timer,api->monotonic_ms(),Day());Save();}
static void Click(void* context){
    const unsigned key=(unsigned)(uintptr_t)context;Advance();const uint64_t now=api->monotonic_ms();
    if(key<=2){
        if(timer.running){Notice("请先暂停，再切换阶段","Pause before switching stages.");return;}
        PomoSelect(&timer,key);
    }else if(key==3){if(timer.running)PomoPause(&timer,now,Day());else PomoStart(&timer,now);}
    else if(key==5){
        if(!reset_until){reset_until=now+4000u;Render(1);return;}
        PomoPause(&timer,now,Day());PomoSelect(&timer,timer.record.phase);
    }else return;
    reset_until=0;Awake();Save();Render(1);
}
static int Clock(int x,int y,int dw){
    const int thick=12,gap=12,height=dw*17/10,half=height/2;
    for(unsigned digit=0;digit<4;++digit){
        const int dx=x+(int)digit*(dw+gap)+(digit>=2?32:0);
        const int xs[7]={thick,dw-thick,dw-thick,thick,0,0,thick};
        const int ys[7]={0,thick,half+thick,height-thick,half+thick,thick,half-thick/2};
        const int ws[7]={dw-2*thick,thick,thick,dw-2*thick,thick,thick,dw-2*thick};
        const int hs[7]={thick,half-thick*3/2,half-thick*2,thick,half-thick*2,half-thick*3/2,thick};
        for(unsigned j=0;j<7;++j){segments[digit][j]=api->label(api->root,dx+xs[j],y+ys[j],ws[j],hs[j],"");if(!segments[digit][j])return 0;}
    }
    for(int dot=0;dot<2;++dot){void* part=api->label(api->root,x+2*(dw+gap)+8,y+height/3+dot*height/3,12,12,"");if(!part)return 0;Style(part,0,0,4,0,1);}
    return 1;
}
int app_main(int argc,char** argv){
    if(argc!=1 || !argv || !argv[0])return -1;api=(const xz_app_api_t*)argv[0];
    const uint32_t required=XZ_APP_CAP_TIME|XZ_APP_CAP_TIMER|XZ_APP_CAP_STORAGE|XZ_APP_CAP_WIDGETS|XZ_APP_CAP_EXIT|XZ_APP_CAP_NOTICE|XZ_APP_CAP_KEEP_AWAKE;
    if(api->abi_version!=XZ_APP_ABI_VERSION || !XZ_APP_HAS_FIELD(api,set_keep_awake) || (api->capabilities&required)!=required || !api->host_context || api->width<600 || api->height<900 ||
        !api->label || !api->button || !api->set_text || !api->set_style || !api->get_local_time || !api->monotonic_ms || !api->timer_start || !api->storage_read || !api->storage_write || !api->progress || !api->set_progress || !api->set_on_exit || !api->notify || !api->set_keep_awake)return -1;
    zh=api->language && api->language[0]=='z';PomoInit(&timer);PomoRecord saved;
    const int count=api->storage_read(api->host_context,"state",&saved,sizeof(saved));
    const int bad=count>=0 && (count!=(int)sizeof(saved) || !PomoRestore(&timer,&saved));PomoDay(&timer,Day());
    const int pad=24,width=api->width-pad*2,panel_y=242,panel_h=api->height-470;
    if(!Label(pad,24,width,48,zh?"番茄时钟":"Pomodoro",30,XZ_APP_ALIGN_CENTER) || !Label(pad,88,width,40,zh?"把注意力留给这一刻":"One thing at a time.",25,XZ_APP_ALIGN_CENTER))return -1;
    const char* cn[]={"专注","短休息","长休息"};const char* en[]={"Focus","Short break","Long break"};
    for(unsigned i=0;i<3;++i){modes[i]=Button(pad+(int)i*((width-24)/3+12),158,(width-24)/3,58,zh?cn[i]:en[i],i,0);if(!modes[i])return -1;}
    void* panel=Label(pad,panel_y,width,panel_h,"",30,XZ_APP_ALIGN_CENTER);Style(panel,30,XZ_APP_ALIGN_CENTER,20,1,0);
    stage=Label(pad+20,panel_y+54,width-40,44,"",30,XZ_APP_ALIGN_CENTER);
    const int dw=(width-100)/4<130?(width-100)/4:130,clock_width=4*dw+3*12+32;
    const int clock_y=panel_y+(panel_h-dw*17/10)/2;
    if(!panel || !stage || !Clock((api->width-clock_width)/2,clock_y,dw))return -1;
    hint=Label(pad+24,panel_y+panel_h-106,width-48,48,"",25,XZ_APP_ALIGN_CENTER);
    bar=api->progress(api->root,pad+width/8,panel_y+panel_h-48,width*3/4,6);
    const int main_width=(width-12)*2/3;
    start_button=Button(pad,panel_y+panel_h+24,main_width,76,"",3,1);Style(start_button,30,XZ_APP_ALIGN_CENTER,14,1,1);
    reset_button=Button(pad+main_width+12,panel_y+panel_h+24,width-main_width-12,76,"",5,0);
    footer=Label(pad,api->height-98,width,80,"",25,XZ_APP_ALIGN_CENTER);
    if(!hint || !bar || !start_button || !reset_button || !footer || api->set_on_exit(api->host_context,Exit,0)!=XZ_APP_OK)return -1;
    if(!api->timer_start(api->host_context,1000,1,Tick,0))return -1;
    last_save=api->monotonic_ms();Render(1);
    if(bad)Notice("存档无效，已恢复默认计时","Invalid saved state. Timer reset.");
    else if(count<0 && count!=XZ_APP_ERR_NOT_FOUND)Notice("无法读取进度，本次从默认计时开始","Saved progress unavailable. Using defaults.");
    return 0;
}

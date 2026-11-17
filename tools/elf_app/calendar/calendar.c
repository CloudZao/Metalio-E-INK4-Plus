#include "app_util.h"
static int year, month, selected_day;
static int today_year, today_month, today_day, has_today;
static void* heading;
static void* month_info;
static void* cells[42];
static void* selected_date;
static void* selected_info;
static void* hint;
static void* navigation[4];
static int Leap(int y) { return y%4==0 && (y%100!=0 || y%400==0); }
static int Days(int y,int m) { static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31}; return days[m-1]+(m==2 && Leap(y)); }
static int Weekday(int y,int m) { static const int offsets[]={0,3,2,5,0,3,5,1,4,6,2,4}; y-=m<3; return (y+y/4-y/100+y/400+offsets[m-1]+1)%7; }
static int ReadToday(void) {
    if (app->struct_size<offsetof(xz_app_api_t,get_local_date)+sizeof(app->get_local_date) || !app->get_local_date) return 0;
    int y,m,d;
    if (!app->get_local_date(&y,&m,&d) || y<1600 || y>2399 || m<1 || m>12 || d<1 || d>Days(y,m)) return 0;
    today_year=y; today_month=m; today_day=d;
    return 1;
}
static void TwoDigits(unsigned n) { if(n<10) Add("0"); Number(n); }
static void Refresh(void) {
    ResetText(); Number(year); Add(" / "); TwoDigits(month); app->set_text(heading,text);
    ResetText(); Number(Days(year,month)); Add(zh?" 天 · ":" days · "); Add(Leap(year)?(zh?"闰年":"Leap year"):(zh?"平年":"Common year")); app->set_text(month_info,text);
    const int start=(Weekday(year,month)+6)%7;
    for(int i=0;i<42;++i) {
        const int day=i-start+1, valid=day>=1 && day<=Days(year,month);
        const int today=valid && has_today && year==today_year && month==today_month && day==today_day;
        ResetText(); if(valid) Number(day); app->set_text(cells[i],text);
        AppStyle(cells[i],36,XZ_APP_ALIGN_CENTER,today?2:0,valid && day==selected_day);
        AppEnabled(cells[i],valid);
    }
    ResetText(); Number(year); Add("-"); TwoDigits(month); Add("-"); TwoDigits(selected_day); app->set_text(selected_date,text);
    static const char* const cn[]={"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
    static const char* const en[]={"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    ResetText();
    if(has_today && year==today_year && month==today_month && selected_day==today_day) Add(zh?"今天 · ":"Today · ");
    Add(zh?cn[(Weekday(year,month)+selected_day-1)%7]:en[(Weekday(year,month)+selected_day-1)%7]);
    app->set_text(selected_info,text);
    app->set_text(hint,has_today?"1600 - 2399":(zh?"时间未同步，可手动浏览":"Time not set; browse manually"));
    AppEnabled(navigation[0],year>1600); AppEnabled(navigation[1],year>1600 || month>1);
    AppEnabled(navigation[2],year<2399 || month<12); AppEnabled(navigation[3],year<2399);
}
static void Click(void* context) {
    const unsigned key=(unsigned)(uintptr_t)context;
    if(key>=32 && key<74) {
        const int day=(int)key-32-(Weekday(year,month)+6)%7+1;
        if(day<1 || day>Days(year,month)) return;
        selected_day=day;
    } else if(key==4) {
        has_today=ReadToday();
        if(has_today) {year=today_year;month=today_month;selected_day=today_day;}
    } else if(key<4) {
        int y=year,m=month;
        if(key==0) --y;
        else if(key==3) ++y;
        else if(key==1) {if(--m==0){m=12;--y;}}
        else {if(++m==13){m=1;++y;}}
        if(y<1600 || y>2399) {app->set_text(hint,zh?"已到日历范围边界":"Calendar limit reached");return;}
        year=y;month=m;if(selected_day>Days(year,month)) selected_day=Days(year,month);
    } else return;
    Refresh();
}
int app_main(int argc,char** argv) {
    if(!AppInit(argc,argv) || app->struct_size<offsetof(xz_app_api_t,set_enabled)+sizeof(app->set_enabled) || !app->set_enabled) return -1;
    has_today=ReadToday();year=has_today?today_year:2000;month=has_today?today_month:1;selected_day=has_today?today_day:1;
    const int pad=24,gap=8,width=app->width-pad*2,cw=(width-gap*6)/7;
    AppStyle(Label(pad,18,width-200,48,zh?"万年历":"Calendar"),30,XZ_APP_ALIGN_LEFT,0,0);
    AppStyle(Label(app->width-pad-180,22,180,40,zh?"公历":"Gregorian"),25,XZ_APP_ALIGN_RIGHT,0,0);
    AppStyle(Label(pad,94,width,134,""),30,XZ_APP_ALIGN_CENTER,1,0);
    heading=Label(pad+16,112,width-32,56,""); AppStyle(heading,36,XZ_APP_ALIGN_CENTER,0,0);
    month_info=Label(pad+16,176,width-32,38,""); AppStyle(month_info,25,XZ_APP_ALIGN_CENTER,0,0);
    static const char* const cn[]={"上一年","上一月","下一月","下一年"};
    static const char* const en[]={"Prev year","Prev month","Next month","Next year"};
    for(int i=0;i<4;++i) {navigation[i]=Button(pad+i*((width-36)/4+12),250,(width-36)/4,60,zh?cn[i]:en[i],Click,i);AppStyle(navigation[i],25,XZ_APP_ALIGN_CENTER,1,0);if(!navigation[i])return -1;}
    static const char* const days_cn[]={"一","二","三","四","五","六","日"};
    static const char* const days_en[]={"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
    for(int i=0;i<7;++i) AppStyle(Label(pad+i*(cw+gap),330,cw,34,zh?days_cn[i]:days_en[i]),25,XZ_APP_ALIGN_CENTER,0,0);
    const int ch=(app->height-650)/6<76?(app->height-650)/6:76;
    for(int i=0;i<42;++i) {cells[i]=Button(pad+i%7*(cw+gap),378+i/7*(ch+gap),cw,ch,"",Click,32+i);if(!cells[i])return -1;}
    const int detail_y=app->height-230;
    AppStyle(Label(pad,detail_y,width,112,""),30,XZ_APP_ALIGN_LEFT,1,0);
    selected_date=Label(pad+18,detail_y+14,width-36,46,""); AppStyle(selected_date,36,XZ_APP_ALIGN_CENTER,0,0);
    selected_info=Label(pad+18,detail_y+66,width-36,34,""); AppStyle(selected_info,25,XZ_APP_ALIGN_CENTER,0,0);
    hint=Label(pad,app->height-76,width-220,64,""); AppStyle(hint,25,XZ_APP_ALIGN_LEFT,0,0);
    void* today=Button(app->width-pad-200,app->height-82,200,60,zh?"回到今天":"Today",Click,4); AppStyle(today,30,XZ_APP_ALIGN_CENTER,2,0);
    if(!heading || !month_info || !selected_date || !selected_info || !hint || !today)return -1;
    Refresh();return 0;
}

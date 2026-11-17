#pragma once
#include <stdint.h>
#include <stddef.h>

enum { POMO_FOCUS, POMO_SHORT, POMO_LONG };
typedef struct {
    uint32_t magic; // 存档格式标记
    uint32_t version; // 存档版本
    uint32_t minutes[3]; // 各阶段时长
    uint32_t cycle_length; // 长休息前的完整专注次数
    uint32_t auto_break; // 是否自动开始休息
    uint32_t phase; // 当前阶段
    uint32_t cycle_done; // 本组已完成专注次数
    uint32_t remaining_ms; // 暂停或快照时的剩余时间
    uint32_t duration_ms; // 本轮开始时的总时长
    uint32_t day; // 统计日期，零表示未知
    uint32_t daily_sessions; // 当日完成次数
    uint32_t daily_minutes; // 当日已完成专注分钟数
    uint32_t total_sessions; // 累计完成次数
    uint32_t total_minutes; // 累计已完成专注分钟数
    uint32_t checksum; // 前述字段的 FNV-1a 校验
} PomoRecord;
typedef struct {
    PomoRecord record; // 可持久化的设置与进度
    uint64_t anchor_ms; // 本次计算的单调时间基准
    int running; // 是否正在前台计时，不持久化
} Pomodoro;

static uint32_t PomoHash(const PomoRecord* record) {
    const uint8_t* bytes=(const uint8_t*)record;
    uint32_t hash=2166136261u;
    for (size_t i=0;i<offsetof(PomoRecord,checksum);++i) hash=(hash^bytes[i])*16777619u;
    return hash;
}
static void PomoSelect(Pomodoro* timer,unsigned phase) {
    timer->record.phase=phase;
    timer->record.duration_ms=timer->record.minutes[phase]*60000u;
    timer->record.remaining_ms=timer->record.duration_ms;
    timer->running=0;
}
static void PomoInit(Pomodoro* timer) {
    *timer=(Pomodoro){0};
    timer->record.magic=0x504F4D4Fu;
    timer->record.version=1;
    timer->record.minutes[0]=25;timer->record.minutes[1]=5;timer->record.minutes[2]=15;
    timer->record.cycle_length=4;timer->record.auto_break=1;
    PomoSelect(timer,POMO_FOCUS);
}
static int PomoRestore(Pomodoro* timer,const PomoRecord* record) {
    if (record->magic!=0x504F4D4Fu || record->version!=1 || record->checksum!=PomoHash(record) ||
        record->minutes[0]<1 || record->minutes[0]>99 || record->minutes[1]<1 || record->minutes[1]>30 ||
        record->minutes[2]<5 || record->minutes[2]>60 || record->cycle_length<2 || record->cycle_length>8 ||
        record->auto_break>1 || record->phase>2 || record->cycle_done>record->cycle_length ||
        record->duration_ms<60000 || record->duration_ms>5940000 || !record->remaining_ms ||
        record->remaining_ms>record->duration_ms || record->daily_sessions>record->total_sessions ||
        record->daily_minutes>record->total_minutes) return 0;
    timer->record=*record;timer->running=0;timer->anchor_ms=0;
    return 1;
}
static void PomoDay(Pomodoro* timer,uint32_t day) {
    if (day && day!=timer->record.day) {
        timer->record.day=day;timer->record.daily_sessions=0;timer->record.daily_minutes=0;
    }
}
static uint32_t PomoAdd(uint32_t value,uint32_t amount) { return UINT32_MAX-value<amount?UINT32_MAX:value+amount; }
static int PomoAdvance(Pomodoro* timer,uint64_t now,uint32_t day) {
    PomoDay(timer,day);
    if (!timer->running) return 0;
    if (now<timer->anchor_ms) {timer->anchor_ms=now;return 0;}
    const uint64_t elapsed=now-timer->anchor_ms;
    timer->anchor_ms=now;
    if (elapsed<timer->record.remaining_ms) {timer->record.remaining_ms-=(uint32_t)elapsed;return 0;}
    const unsigned previous=timer->record.phase;
    if (previous==POMO_FOCUS) {
        const uint32_t minutes=timer->record.duration_ms/60000u;
        timer->record.total_sessions=PomoAdd(timer->record.total_sessions,1);
        timer->record.total_minutes=PomoAdd(timer->record.total_minutes,minutes);
        if (day) {
            timer->record.daily_sessions=PomoAdd(timer->record.daily_sessions,1);
            timer->record.daily_minutes=PomoAdd(timer->record.daily_minutes,minutes);
        }
        if (timer->record.cycle_done<timer->record.cycle_length) ++timer->record.cycle_done;
        PomoSelect(timer,timer->record.cycle_done>=timer->record.cycle_length?POMO_LONG:POMO_SHORT);
        timer->running=timer->record.auto_break!=0;
    } else {
        if (previous==POMO_LONG) timer->record.cycle_done=0;
        PomoSelect(timer,POMO_FOCUS);
    }
    return 1;
}
static void PomoStart(Pomodoro* timer,uint64_t now) {timer->anchor_ms=now;timer->running=1;}
static int PomoPause(Pomodoro* timer,uint64_t now,uint32_t day) {
    const int finished=PomoAdvance(timer,now,day);timer->running=0;return finished;
}

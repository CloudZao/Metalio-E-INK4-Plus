#pragma once
#include <cstdint>
namespace plus_app {
// Application-task only; all times use the same monotonic microsecond clock.
class IdleSleep {
public:
    static constexpr uint64_t kTimeoutUs=600000000ULL;
    explicit IdleSleep(uint64_t now=0):last_(now){}
    void Activity(uint64_t now){if(now>last_)last_=now;}
    void Reset(uint64_t now){last_=now;retry_after_=0;requested_=false;}
    void BeginUserLongTask(){++long_tasks_;}
    void EndUserLongTask(uint64_t now){if(long_tasks_){--long_tasks_;if(!long_tasks_)Reset(now);}}
    bool Due(uint64_t now)const{return !long_tasks_&&!requested_&&now>=last_&&now-last_>=kTimeoutUs&&now>=retry_after_;}
    void Requested(){requested_=true;}
    void Refused(uint64_t now){requested_=false;retry_after_=now+60000000ULL;}
    unsigned LongTasks()const{return long_tasks_;}
private:
    uint64_t last_=0,retry_after_=0;
    unsigned long_tasks_=0;
    bool requested_=false;
};
// Preserve accepted inputs; coalesce only DISPLAY work. HAL owns one snapshot.
class FrameFlow {
public:
    void Changed(){dirty_=true;}
    void NewScene(){++scene_;Changed();}
    void ManualFull(){full_=true;failed_=false;retry_=0;Changed();}
    bool CanSubmit(bool blocked,bool busy)const{return dirty_&&!in_flight_&&!blocked&&!busy&&!failed_;}
    bool WantsFull()const{return full_;}
    uint32_t NextRevision()const{return revision_+1;}
    void Submitted(){++revision_;submitted_scene_=scene_;in_flight_=true;dirty_=false;full_=false;}
    bool Observe(uint32_t displayed,bool healthy,bool busy){
        if(!in_flight_)return false;
        if(displayed>=revision_){in_flight_=false;shown_scene_=submitted_scene_;retry_=0;return true;}
        if(!healthy&&!busy){in_flight_=false;if(retry_++==0){dirty_=true;full_=true;}else failed_=true;}
        return false;
    }
    bool SceneVisible()const{return shown_scene_==scene_;}
    bool Failed()const{return failed_;}
    uint32_t Revision()const{return revision_;}
    uint32_t ShownScene()const{return shown_scene_;}
private:
    uint32_t revision_=0,scene_=1,shown_scene_=0,submitted_scene_=0;
    unsigned retry_=0;
    bool dirty_=true,full_=true,in_flight_=false,failed_=false;
};
}

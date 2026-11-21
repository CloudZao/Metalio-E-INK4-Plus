// CAPABILITY_DEMO_ONLY: implement the user's business before final delivery.
#include "plus_hal.h"
#include "plus_pocket_bridge.h"
#include "pocket_build.h"
#include "app_view.h"
#include "app_lifecycle.h"
#include "generated/body/font.h"
#include "generated/license_generated.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <vector>
namespace {
bool Restore(const pocket::RunContext*,std::string* error) {
    if(POCKET_STORAGE_MODE==0)return true;
    if(error)*error="BUSINESS_RESTORE_NOT_IMPLEMENTED";
    return false;
}
bool Save(plus_pocket::SaveReason,const pocket::RunContext*,std::string* error) {
    if(POCKET_STORAGE_MODE==0)return true;
    if(error)*error="BUSINESS_DURABLE_SAVE_NOT_IMPLEMENTED";
    return false;
}
uint64_t Now(){return static_cast<uint64_t>(esp_timer_get_time());}
}
extern "C" void app_main() {
    plus_hal::EarlyWakeGate();
    const auto nvs=nvs_flash_init();
    if(nvs!=ESP_OK){ESP_LOGE("PlusApp","NVS_INIT_FAILED %s",esp_err_to_name(nvs));return;}
    std::string error;
    if(!plus_hal::Init(&error)){ESP_LOGE("PlusApp","HAL_INIT_FAILED %s",error.c_str());return;}
    plus_font::Font font;
    if(!font.Open(plus_font_data,plus_font_data_size)){ESP_LOGE("PlusApp","FONT_INVALID");return;}
    bool initialized=plus_pocket::Initialize({Restore,Save},&error);
    if(!initialized)ESP_LOGE("PlusApp","INITIALIZATION_FAILED %s",error.c_str());
    plus_app::View view;
    auto* mono=static_cast<uint8_t*>(heap_caps_malloc(view.Bytes(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    auto* native=static_cast<uint8_t*>(heap_caps_malloc(415872,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!mono||!native){heap_caps_free(mono);heap_caps_free(native);ESP_LOGE("PlusApp","FRAME_ALLOC_FAILED");return;}
    plus_app::Model model;
    model.title=POCKET_DISPLAY_NAME;model.integrated=initialized&&plus_pocket::Integrated();model.help=plus_licenses::Text();
    if(!initialized){model.page=plus_app::Page::Failure;model.status=plus_pocket::ErrorMessage(plus_pocket::LastError());}
    plus_app::FrameFlow frames;plus_app::IdleSleep idle(Now());
    plus_app::Layout layout;std::vector<std::size_t> offsets{0};std::size_t help_page=0;
    std::size_t help_next=0;bool startup=false;uint32_t standby=plus_hal::StandbyRevision();
    uint32_t visible_scene=0;uint64_t gauge_at=0;
    auto change_scene=[&](){frames.NewScene();plus_hal::InvalidateTouches();};
    for(;;) {
        const uint64_t now=Now();idle.Activity(plus_hal::LastUserActivityUs());
        const auto current_standby=plus_hal::StandbyRevision();
        if(current_standby!=standby){standby=current_standby;idle.Reset(now);}
        frames.Observe(plus_hal::DisplayedRevision(),plus_hal::DisplayHealthy(),plus_hal::DisplayBusy());
        if(frames.ShownScene()!=visible_scene){visible_scene=frames.ShownScene();plus_hal::InvalidateTouches();}
        if(initialized&&!startup&&visible_scene){
            if(!plus_pocket::ConfirmStartup(frames.Revision(),&error)){
                ESP_LOGE("PlusApp","STARTUP_NOT_CONFIRMED %s",error.c_str());
                initialized=false;model.integrated=false;model.page=plus_app::Page::Failure;
                model.status=plus_pocket::ErrorMessage(plus_pocket::LastError());change_scene();
            }else{
                startup=true;ESP_LOGI("PlusApp","CAPABILITY_DEMO_READY mode=%s",model.integrated?"pocket":"standalone");
            }
        }
        const bool manual_sleep=plus_hal::PollPowerOff();
        if(!initialized&&(manual_sleep||idle.Due(now))){
            // No user edits are admitted before startup confirmation. Preserve
            // the failed restore/control records: never fabricate a save here.
            // A real application must stop any workers it started before this
            // point. This probe starts no business workers.
            idle.Requested();
            if(!plus_hal::PowerOff()){
                ESP_LOGE("PlusApp","FAULT_SLEEP_REFUSED");
                idle.Refused(Now());
            }
        }else if(initialized&&(manual_sleep||(startup&&idle.Due(now)))) {
            const uint64_t activity_before_save=plus_hal::LastUserActivityUs();
            idle.Requested();
            // Stop real business workers before Save: use TaskFence when needed.
            // This stateless demo has no background business workers.
            error.clear();
            const bool saved=plus_pocket::Save(&error);
            if(!manual_sleep&&plus_hal::LastUserActivityUs()>activity_before_save){
                idle.Reset(Now()); // New input during save cancels automatic sleep.
            }else{
                if(saved&&!plus_hal::PowerOff())error="DEEP_SLEEP_REFUSED";
                ESP_LOGE("PlusApp","SLEEP_NOT_COMPLETED %s",error.c_str());
                model.status="保存或深休失败，数据未丢弃，请检查存储。";
                frames.Changed();idle.Refused(Now());
            }
        }
        if(!plus_hal::IsStandby()) {
            const bool ai=plus_hal::PollAiClick();const int volume=plus_hal::PollVolumeDelta();
            if(ai||volume!=0)idle.Activity(now);
            int x=0,y=0;
            // Drain accepted input without waiting for physical frame completion.
            while(plus_hal::PollTouch(&x,&y)) {
                idle.Activity(now);
                if((initialized&&!startup)||!frames.SceneVisible())continue;
                if(plus_app::Hit(layout.refresh,x,y)){frames.ManualFull();continue;}
                if(plus_app::Hit(layout.help,x,y)){
                    model.page=model.page==plus_app::Page::Help?(initialized?plus_app::Page::Home:plus_app::Page::Failure):plus_app::Page::Help;
                    help_page=0;model.help_offset=0;change_scene();continue;
                }
                if(model.integrated&&plus_app::Hit(layout.back,x,y)){
                    model.page=plus_app::Page::ExitConfirm;change_scene();continue;
                }
                if(model.page==plus_app::Page::Help){
                    if(plus_app::Hit(layout.previous,x,y)&&help_page){model.help_offset=offsets[--help_page];change_scene();}
                    else if(plus_app::Hit(layout.next,x,y)&&help_next<std::strlen(model.help)){
                        model.help_offset=help_next;++help_page;
                        if(help_page==offsets.size())offsets.push_back(help_next);else offsets[help_page]=help_next;
                        change_scene();
                    }
                } else if(model.page==plus_app::Page::ExitConfirm){
                    if(plus_app::Hit(layout.cancel,x,y)){model.page=plus_app::Page::Home;change_scene();}
                    else if(plus_app::Hit(layout.confirm,x,y)){
                        if(!plus_pocket::ReturnToPocket(&error)){
                            ESP_LOGE("PlusApp","RETURN_FAILED %s",error.c_str());
                            model.status="保存返回失败，当前内容保留。";model.page=plus_app::Page::Home;change_scene();
                        }
                    }
                } else if(initialized&&plus_pocket::CanEdit()){
                    if(plus_app::Hit(layout.action,x,y)){++model.touches;frames.Changed();}
                    else if(plus_app::Hit(layout.reset,x,y)){model.touches=0;frames.Changed();}
                }
            }
            if(now>=gauge_at){
                const int battery=plus_hal::BatteryPercent();
                if(battery!=model.battery){model.battery=battery;frames.Changed();}
                gauge_at=now+60000000ULL;
            }
        }
        if(frames.CanSubmit(plus_hal::IsStandby(),plus_hal::DisplayBusy())){
            const auto rendered=plus_app::Render(font,mono,view.Bytes(),view,model,&layout);help_next=rendered.next;
            if(rendered.missing||rendered.invalid)ESP_LOGW("PlusApp","TEXT_DIAGNOSTIC missing=%u invalid=%u",unsigned(rendered.missing),unsigned(rendered.invalid));
            if(!plus_app::ToNative4(mono,view.Bytes(),view,native,415872))break;
            if(plus_hal::QueueGray4(native,frames.WantsFull(),frames.NextRevision(),&error))frames.Submitted();
            else ESP_LOGW("PlusApp","FRAME_NOT_ADMITTED %s",error.c_str());
        }
        vTaskDelay(pdMS_TO_TICKS(10)>0?pdMS_TO_TICKS(10):1);
    }
    heap_caps_free(mono);heap_caps_free(native);
}

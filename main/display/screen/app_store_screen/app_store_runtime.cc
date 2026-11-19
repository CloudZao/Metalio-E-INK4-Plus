#include "app_store_screen.h"
#include "app_runtime_services.h"
#include "app_store/app_api.h"
#include "app_store/app_store_icons.h"
#include "epd_refresh.h"
#include "assets/lang_config.h"
#include "assets/lang_app_store.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"
#include <memory>
#include <new>
#include <ctime>
#include <cstring>
#include <esp_elf.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
struct Runtime {
    AppStore::Item item; // 本次运行的不可变安装信息
    esp_elf_t elf {}; // ELF 运行资源
    bool initialized = false; // 是否需要释放加载器
    std::atomic<bool> ready {false}; // 工作任务发布加载结果
    bool loaded = false; // 重定位是否完成
    std::string error; // 加载错误
    xz_app_api_t api {}; // 生命周期与页面相同的宿主接口
    AppRuntimeServices::State services; // 随页面清理的基础服务
    ~Runtime() { if(initialized) esp_elf_deinit(&elf); }
};
struct Page {
    std::shared_ptr<Runtime> runtime; // 页面与加载任务共享运行资源
    lv_timer_t* timer = nullptr; // 等待异步加载完成
    lv_obj_t* content = nullptr; // 应用可绘制区域
    lv_obj_t* loading = nullptr; // 应用内容就绪前的居中加载层
    lv_obj_t* status = nullptr; // 加载状态
    bool closing = false; // 已离页时禁止执行 ELF 入口
    uint32_t loading_sequence = 0; // 加载页创建前最后提交的物理帧
};
struct ButtonContext {
    xz_app_click_t callback; // ELF 内按钮回调
    void* context; // 应用自持回调数据
};
AppStore::Item s_pending;
std::weak_ptr<Runtime> s_active_runtime;

bool RuntimeKey(const char* key) {
    if (StandbyScreen::IsResuming()) return true;
    if (std::strcmp(key, "vk_home") != 0 && std::strcmp(key, "vk_prev") != 0) return false;
    auto runtime = s_active_runtime.lock();
    if (!runtime || !runtime->services.open || runtime->services.closing || !runtime->services.on_back) return false;
    return runtime->services.on_back(runtime->services.back_context) != 0;
}

void Style(lv_obj_t* obj,int x,int y,int width,int height) {
    lv_obj_set_pos(obj,x,y);
    lv_obj_set_size(obj,width,height);
    lv_obj_set_style_bg_color(obj,lv_color_white(),0);
    lv_obj_set_style_text_color(obj,lv_color_black(),0);
    lv_obj_set_style_border_color(obj,lv_color_black(),0);
    lv_obj_set_style_shadow_width(obj,0,0);
    lv_obj_set_style_radius(obj,0,0);
    lv_obj_remove_flag(obj,LV_OBJ_FLAG_SCROLLABLE);
}
void* Label(void* parent,int x,int y,int width,int height,const char* text) {
    auto* label=lv_label_create(static_cast<lv_obj_t*>(parent));
    Style(label,x,y,width,height);
    lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);
    lv_label_set_text(label,text ? text : "");
    return label;
}
void ButtonEvent(lv_event_t* e) {
    auto* data=static_cast<ButtonContext*>(lv_event_get_user_data(e));
    if(lv_event_get_code(e)==LV_EVENT_DELETE) delete data;
    else if(lv_event_get_code(e)==LV_EVENT_CLICKED && data->callback &&
            !StandbyScreen::IsActive() && !StandbyScreen::IsResuming()) data->callback(data->context);
}
void* Button(void* parent,int x,int y,int width,int height,const char* text,xz_app_click_t click,void* context) {
    auto* data=new(std::nothrow) ButtonContext{click,context};
    if(!data) return nullptr;
    auto* button=lv_button_create(static_cast<lv_obj_t*>(parent));
    lv_obj_remove_style_all(button);
    lv_obj_set_style_bg_opa(button,LV_OPA_COVER,0);
    Style(button,x,y,width,height);
    lv_obj_set_style_border_width(button,2,0);
    auto* label=lv_label_create(button);
    lv_obj_set_width(label,width-12);
    lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);
    lv_label_set_text(label,text ? text : "");
    lv_obj_center(label);
    lv_obj_add_event_cb(button,ButtonEvent,LV_EVENT_ALL,data);
    HapticAttachClick(button);
    return button;
}
void SetText(void* widget,const char* text) {
    if (!widget) return;
    auto* object=static_cast<lv_obj_t*>(widget);
    if (lv_obj_check_type(object,&lv_button_class)) object=lv_obj_get_child(object,0);
    if (object && lv_obj_check_type(object,&lv_label_class)) lv_label_set_text(object,text ? text : "");
    else if (object && lv_obj_check_type(object,&lv_textarea_class)) lv_textarea_set_text(object,text ? text : "");
}
void WidgetStyle(void* widget, const xz_app_style_t* style) {
    if (!widget || !style) return;
    auto* object=static_cast<lv_obj_t*>(widget);
    const bool button=lv_obj_check_type(object,&lv_button_class);
    auto* label=button ? lv_obj_get_child(object,0) : object;
    if (!label) return;
    if (style->font_size) {
        const auto* font=fontpack_lv_font_get(style->font_size,2);
        if (font) lv_obj_set_style_text_font(label,font,0);
    }
    const auto align=style->text_align==XZ_APP_ALIGN_RIGHT ? LV_TEXT_ALIGN_RIGHT :
        style->text_align==XZ_APP_ALIGN_CENTER ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT;
    lv_obj_set_style_text_align(label,align,0);
    lv_obj_set_style_border_width(object,style->border_width,0);
    lv_obj_set_style_radius(object,style->radius,0);
    lv_obj_set_style_bg_color(object,style->primary ? lv_color_black() : lv_color_white(),0);
    lv_obj_set_style_bg_opa(object,button || style->border_width || style->primary ? LV_OPA_COVER : LV_OPA_TRANSP,0);
    lv_obj_set_style_text_color(label,style->primary ? lv_color_white() : lv_color_black(),0);
}
void WidgetEnabled(void* widget, int enabled) {
    if (!widget) return;
    auto* object=static_cast<lv_obj_t*>(widget);
    lv_obj_set_style_opa(object,LV_OPA_50,LV_STATE_DISABLED);
    if (enabled) lv_obj_remove_state(object,LV_STATE_DISABLED);
    else lv_obj_add_state(object,LV_STATE_DISABLED);
}
int LocalDate(int* year, int* month, int* day) {
    if (!year || !month || !day) return 0;
    const time_t now=std::time(nullptr);
    struct tm date {};
    if (!localtime_r(&now,&date) || date.tm_year<120) return 0;
    *year=date.tm_year+1900;
    *month=date.tm_mon+1;
    *day=date.tm_mday;
    return 1;
}
uint32_t ReadAsset(void* context, uint32_t offset, void* buffer, uint32_t length) {
    return AppStore::ReadAsset(static_cast<Runtime*>(context)->item, offset, buffer, length);
}
void Load(void* context) {
    std::unique_ptr<std::shared_ptr<Runtime>> holder(static_cast<std::shared_ptr<Runtime>*>(context));
    auto runtime=*holder;
    runtime->initialized = esp_elf_init(&runtime->elf) == 0;
    runtime->loaded = runtime->initialized && AppStore::Load(runtime->item, runtime->elf, runtime->error);
    if (!runtime->initialized) { ESP_LOGE("AppRuntime", "operation=launch stage=elf_init result=failed"); runtime->error = Lang::Strings::STORE_MEMORY_ERROR; }
    if (runtime->loaded) runtime->api.asset_size = AppStore::AssetSize(runtime->item);
    runtime->ready.store(true);
    holder.reset();
    runtime.reset();
    vTaskDelete(nullptr);
}
lv_obj_t* CreateRuntime();
void Tick(lv_timer_t* timer) {
    auto* page=static_cast<Page*>(lv_timer_get_user_data(timer));
    auto& runtime=*page->runtime;
    if(page->closing || !runtime.ready.load()) return;
    if (static_cast<int32_t>(EpdGetStats().completed - page->loading_sequence) <= 0) return;
    lv_timer_delete(timer);
    page->timer=nullptr;
    if(!runtime.loaded) {lv_label_set_text(page->status,runtime.error.c_str());return;}
    lv_obj_clean(page->content);
    const auto asset_size = runtime.api.asset_size;
    runtime.api={XZ_APP_ABI_VERSION,sizeof(xz_app_api_t),page->content,
        lv_obj_get_width(page->content),lv_obj_get_height(page->content),Lang::Current(),Label,Button,SetText,&runtime,asset_size,ReadAsset,WidgetStyle,WidgetEnabled,LocalDate};
    AppRuntimeServices::Bind(runtime.services,runtime.api,page->content,runtime.item);
    s_active_runtime = page->runtime;
    char* args[]={reinterpret_cast<char*>(&runtime.api),nullptr};
    const int result=esp_elf_request(&runtime.elf,0,1,args);
    if(result!=0) {
        ESP_LOGE("AppRuntime", "operation=launch stage=entry result=failed code=%d",result);
        AppRuntimeServices::Close(runtime.services);
        lv_obj_clean(page->content);
        s_active_runtime.reset();
        lv_label_set_text(page->status,result == XZ_APP_ERR_UNSUPPORTED ?
            Lang::Strings::STORE_INCOMPATIBLE : Lang::Strings::STORE_RUN_ERROR);
        return;
    }
    ESP_LOGI("AppRuntime", "operation=launch result=success");
    lv_obj_remove_flag(page->content,LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete(page->loading);
    page->loading=nullptr;
    page->status=nullptr;
}
void Unload(lv_event_t* event) {
    auto* page=static_cast<Page*>(lv_event_get_user_data(event));
    page->closing=true;
    if(page->timer) { lv_timer_delete(page->timer); page->timer=nullptr; }
    AppRuntimeServices::Close(page->runtime->services);
}
void Delete(lv_event_t* e) {
    auto* page=static_cast<Page*>(lv_event_get_user_data(e));
    if(page->timer) lv_timer_delete(page->timer);
    AppRuntimeServices::Close(page->runtime->services);
    lv_obj_clean(page->content);
    delete page;
}
lv_obj_t* CreateRuntime() {
    auto* page=new Page;
    page->runtime=std::make_shared<Runtime>();
    page->runtime->item=s_pending;
    ScreenSetIsHome(false);
    auto* root=lv_obj_create(nullptr);
    lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(root,0,0);
    lv_obj_set_style_bg_color(root,lv_color_white(),0);
    lv_obj_set_style_text_color(root,lv_color_black(),0);
    lv_obj_set_style_text_font(root,fontpack_lv_font_ui(),0);
    auto bar=ScreenCreateStatusBar(root);
    page->content=lv_obj_create(root);
    Style(page->content,0,bar.height,LV_HOR_RES,LV_VER_RES-bar.height);
    lv_obj_set_style_border_width(page->content,0,0);
    lv_obj_set_style_pad_all(page->content,0,0);
    lv_obj_add_flag(page->content,LV_OBJ_FLAG_HIDDEN);
    page->loading=lv_obj_create(root);
    Style(page->loading,0,bar.height,LV_HOR_RES,LV_VER_RES-bar.height);
    lv_obj_set_style_border_width(page->loading,0,0);
    lv_obj_set_style_pad_all(page->loading,0,0);
    auto* group=lv_obj_create(page->loading);
    lv_obj_remove_style_all(group);
    lv_obj_remove_flag(group,static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_size(group,LV_HOR_RES-80,LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(group,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(group,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(group,0,0);
    auto* icon=lv_image_create(group);
    lv_image_set_src(icon,AppStoreIcons::Source(page->runtime->item));
    lv_obj_set_size(icon,128,128);
    lv_image_set_inner_align(icon,LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(icon,false);
    lv_obj_remove_flag(icon,LV_OBJ_FLAG_CLICKABLE);
    auto* name=static_cast<lv_obj_t*>(Label(group,0,0,LV_PCT(100),LV_SIZE_CONTENT,AppStore::Name(page->runtime->item)));
    lv_obj_set_style_margin_top(name,16,0);
    lv_obj_set_style_text_align(name,LV_TEXT_ALIGN_CENTER,0);
    page->status=static_cast<lv_obj_t*>(Label(group,0,0,LV_PCT(100),LV_SIZE_CONTENT,Lang::Strings::STORE_LOADING));
    lv_obj_set_style_margin_top(page->status,40,0);
    lv_obj_set_style_text_align(page->status,LV_TEXT_ALIGN_CENTER,0);
    const auto* status_font=fontpack_lv_font_get(25,2);
    if(status_font) lv_obj_set_style_text_font(page->status,status_font,0);
    lv_obj_center(group);
    page->loading_sequence=EpdGetStats().submitted;
    page->timer=lv_timer_create(Tick,100,page);
    lv_obj_add_event_cb(root,Unload,LV_EVENT_SCREEN_UNLOADED,page);
    lv_obj_add_event_cb(root,Delete,LV_EVENT_DELETE,page);
    VkKeyScreenDesc keys;
    keys.factory=CreateRuntime;
    keys.on_key=RuntimeKey;
    VkKey_AttachScreen(root,"AppRuntime",keys);
    auto* holder=new(std::nothrow) std::shared_ptr<Runtime>(page->runtime);
    // 加载器会同步 PSRAM 代码缓存，任务栈必须位于内部 RAM。
    if(!holder || xTaskCreatePinnedToCore(Load,"app_load",12*1024,holder,2,nullptr,0)!=pdPASS) {
        ESP_LOGE("AppRuntime", "operation=launch stage=create_task result=failed stack_bytes=12288");
        delete holder;
        page->runtime->error=Lang::Strings::STORE_MEMORY_ERROR;
        page->runtime->ready.store(true);
    }
    return root;
}
}

void AppStoreScreen::Launch(const AppStore::Item& item) {
    s_pending=item;
    ScreenNavigateTo(CreateRuntime);
}

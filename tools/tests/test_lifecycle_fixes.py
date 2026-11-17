"""Native fault injection against the actual WiFi/render lifecycle functions.

Windows: python tools/tests/test_lifecycle_fixes.py
Uses the installed MSVC compiler; no device or SDK runtime is required.
Stubs model API outcomes, not DMA timing or hardware interrupt behavior.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
VC = Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207")
UM = Path("C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/um/x64")


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


COMMON = r'''
extern "C" __declspec(dllimport) void __stdcall ExitProcess(unsigned int);
#define CHECK(x) do { if (!(x)) ExitProcess(__LINE__); } while (0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffffu
typedef void* SemaphoreHandle_t;
typedef int StaticSemaphore_t;
static void* mutex_handle = (void*)99;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*) { return mutex_handle; }
static int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
'''

wifi = (ROOT / "main/display/screen/settings_screen/settings_test/settings_test_wifi.cc").read_text(encoding="utf-8")
wifi_functions = wifi[wifi.index("class WifiLifecycleLock"):wifi.index("void CloseWifiPopup()")]
wifi_test = COMMON + r'''
typedef int esp_err_t;
typedef int wifi_mode_t;
#define WIFI_MODE_NULL 0
#define WIFI_MODE_STA 1
#define ESP_OK 0
#define ESP_ERR_WIFI_NOT_INIT 10
#define ESP_ERR_INVALID_STATE 11
struct wifi_init_config_t { bool nvs_enable; };
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
struct Ui { bool wifi_stack_owned; void* wifi_netif; } ui;
struct Gen { unsigned value; unsigned load() { return value; } } s_scan_generation;
static unsigned s_worker_generation;
static bool live = true, initialized, foreign;
static int mode, failure, init_calls, stops, deinits, destroys, creates;
static bool deinit_failed;
static bool user_enabled = true;
struct WifiStation {
 static WifiStation& GetInstance() { static WifiStation instance; return instance; }
 bool IsUserEnabled() { return user_enabled; }
};
static Ui& SettingsTest_Ui() { return ui; }
static bool SettingsTest_IsLive() { return live; }
static int xSemaphoreTake(SemaphoreHandle_t, unsigned) { return 1; }
static int esp_wifi_get_mode(wifi_mode_t* m) { *m=mode; return initialized ? ESP_OK : ESP_ERR_WIFI_NOT_INIT; }
static int esp_netif_init() { return 0; }
static int esp_event_loop_create_default() { return 0; }
static void* esp_netif_get_handle_from_ifkey(const char*) { return foreign ? (void*)2 : nullptr; }
static void* esp_netif_create_default_wifi_sta() { CHECK(!foreign); ++creates; return (void*)1; }
static void esp_netif_destroy_default_wifi(void* p) { CHECK(p==(void*)1); ++destroys; }
static int esp_wifi_init(wifi_init_config_t*) { ++init_calls; if(failure==1)return 20; initialized=true; return 0; }
static int esp_wifi_set_mode(int m) { if(failure==2)return 20; mode=m; return 0; }
static int esp_wifi_start() { return failure==3 ? 20 : 0; }
static int esp_wifi_stop() { ++stops; return 0; }
static int esp_wifi_scan_stop() { return 0; }
static int esp_wifi_deinit() { ++deinits; if(deinit_failed)return 20; initialized=false; mode=0; return 0; }
''' + wifi_functions + r'''
static void reset() {
 ui={}; live=true; initialized=false; foreign=false; mode=0; failure=0; user_enabled=true;
 init_calls=stops=deinits=destroys=creates=0; deinit_failed=false;
 s_worker_generation=0; s_scan_generation.value=0;
}
int main() {
 reset(); user_enabled=false; CHECK(!WifiEnsureForScan()); CHECK(init_calls==0 && creates==0);
 // Each initialization failure must release the newly-created STA, including
 // init OOM before driver ownership. A later attempt must succeed.
 for(int stage=1; stage<=3; ++stage) {
  reset(); failure=stage;
  CHECK(!WifiEnsureForScan());
  CHECK(!ui.wifi_stack_owned && !ui.wifi_netif && destroys==1);
  CHECK(deinits==(stage==1 ? 0 : 1));
  failure=0; CHECK(WifiEnsureForScan()); CHECK(ui.wifi_stack_owned);
  WifiTeardownIfOwned(); CHECK(destroys==2 && !initialized && !ui.wifi_netif);
  WifiTeardownIfOwned(); CHECK(destroys==2);
 }
 reset(); initialized=true; mode=1; CHECK(WifiEnsureForScan());
 WifiTeardownIfOwned(); CHECK(initialized && deinits==0 && destroys==0 && creates==0);
 reset(); foreign=true; CHECK(!WifiEnsureForScan()); CHECK(creates==0 && destroys==0);
 reset(); initialized=true; CHECK(!WifiEnsureForScan()); CHECK(init_calls==0 && deinits==0);
 reset(); live=false; CHECK(!WifiEnsureForScan()); CHECK(creates==0);
 reset(); s_scan_generation.value=1; CHECK(!WifiEnsureForScan()); CHECK(creates==0);
 reset(); CHECK(WifiEnsureForScan()); deinit_failed=true; WifiTeardownIfOwned();
 CHECK(ui.wifi_stack_owned && ui.wifi_netif && destroys==0);
 deinit_failed=false; WifiTeardownIfOwned(); CHECK(!ui.wifi_netif && destroys==1);
 return 0;
}
'''

render = (ROOT / "main/boards/metalio-e-ink4-plus/display/output_lcd/render_lcd.c").read_text(encoding="utf-8")
render_functions = "\n".join(function(render, sig) for sig in (
    "static void scan_lock(", "static void cancel_scan(",
    "static bool start_scan(", "static void notify_feeders(", "static bool wait_feeders(", "void lcd_do_update("))
render_test = COMMON + r'''
typedef long long int64_t;
typedef unsigned char uint8_t;
#define NULL 0
#define NUM_RENDER_THREADS 2
#define INT32_MAX 2147483647
#define EPD_DRAW_EMPTY_LINE_QUEUE 1024
#define EPD_MIN_PHASE_US 12000
#define EPD_PHASE_HZ 80
#define memory_order_release 0
#define memory_order_acquire 0
#define __ATOMIC_RELAXED 0
#define atomic_store_explicit(p,v,o) (*(p)=(v))
#define atomic_load_explicit(p,o) (*(p))
#define __atomic_store_n(p,v,o) (*(p)=(v))
#define assert(x) CHECK(x)
typedef void (*frame_done_func_t)(void*);
typedef bool (*line_cb_func_t)(void*,void*);
struct RenderContext_t {
 bool feed_cancelled; int cycle_frames,lines_total,error,current_frame;
 int fault_reason,fault_phase,fault_line,fault_thread,fault_ready,lines_consumed;
 int fault_write[2],fault_read[2],line_queues[2];
 void* feed_tasks[2]; void* feed_done_smphr[2]; void* frame_done;
 void* feed_space[2]; void* prefill_done; void* spatial_luts; void* conversion_lut;
 bool feed_waiting[2]; uint8_t line_ready[1], line_threads[1]; int scan_frame;
};
static RenderContext_t* current;
static StaticSemaphore_t s_scan_lock_storage;
static SemaphoreHandle_t s_scan_lock;
static int last_phase_average,last_phase_minimum,last_phase_maximum,s_prefill_lines=192;
static int scenario, takes[2], resets, stopped, started;
static bool acknowledged[2], expect_abort, callback_enabled;
static int64_t now;
__declspec(noreturn) static void abort() {
 CHECK(expect_abort && stopped && !resets && !callback_enabled);
 ExitProcess(0);
}
static int xSemaphoreTake(void* sem,unsigned ticks) {
 if(sem==mutex_handle) return 1;
 if(sem==current->prefill_done) return ticks ? 1 : 0;
 if(sem==current->feed_space[0] || sem==current->feed_space[1]) return 0;
 if(sem==current->frame_done) return (scenario==1 && ticks) ? 0 : 1;
 int i = sem==current->feed_done_smphr[0] ? 0 : 1;
 ++takes[i];
 if((scenario==2 || scenario==3) && i==0 && takes[i]==1) return 0;
 if(scenario==4 && i==1 && takes[i]==1) return 0;
 if(scenario==3 && i==0) return 0;
 acknowledged[i]=true; return 1;
}
static void epd_lcd_line_source_cb(line_cb_func_t,void*) {}
static void epd_lcd_frame_done_cb(frame_done_func_t cb,void*) { callback_enabled=cb!=0; }
static void epd_lcd_abort_frame() { ++stopped; }
static void epd_lcd_start_frame() { ++started; }
static bool retrieve_line_isr(void*,void*) { return false; }
static void handle_lcd_frame_done(void*) {}
static void prepare_context_for_next_frame(RenderContext_t*) {}
static int64_t esp_timer_get_time() { return now+=100; }
static void epd_set_mode(int) {}
static int xPortGetCoreID() { return 0; }
static void xTaskNotifyGive(void*) {}
static void pace_phase(int64_t) {}
static void vTaskSuspendAll() {}
static void xTaskResumeAll() {}
static void* memset(void* buffer, int value, decltype(sizeof(0)) length) {
 auto* data=static_cast<unsigned char*>(buffer);
 for(decltype(length) i=0;i<length;++i) data[i]=static_cast<unsigned char>(value);
 return buffer;
}
static void epd_spatial_prepare_pairs(void*,void*) {}
static int64_t epd_lcd_get_spv_start_us() { return now; }
static int epd_lcd_get_scan_time_us() { return 100; }
static void epd_lcd_get_stream_stats(int*,int*) {}
static void lq_reset(int*) { CHECK(acknowledged[0] && acknowledged[1] && stopped); ++resets; }
''' + render_functions + r'''
int main() {
 RenderContext_t ctx={}; current=&ctx; ctx.cycle_frames=1;
 ctx.feed_done_smphr[0]=(void*)1; ctx.feed_done_smphr[1]=(void*)2; ctx.frame_done=(void*)3;
 ctx.prefill_done=(void*)4; ctx.feed_space[0]=(void*)5; ctx.feed_space[1]=(void*)6;
 const int scenarios[]={0,1,2,4};
 for(int test=0;test<4;++test) {
  scenario=scenarios[test];
  ctx.error=0; ctx.current_frame=0; takes[0]=takes[1]=0;
  acknowledged[0]=acknowledged[1]=false; resets=stopped=0;
  lcd_do_update(&ctx);
  CHECK(acknowledged[0] && acknowledged[1]);
  CHECK(scenario ? (resets==2 && ctx.feed_cancelled) : (resets==0 && !ctx.error));
 }
 // A producer which reaches start after cancellation cannot restart scanning.
 started=0; CHECK(!start_scan(&ctx)); CHECK(!started);
 ctx.feed_cancelled=false; CHECK(start_scan(&ctx)); CHECK(started==1);
 // A permanently stuck feeder must not return to the caller/free live LUTs.
 scenario=3; takes[0]=takes[1]=0; resets=stopped=0; ctx.error=0; expect_abort=true;
 lcd_do_update(&ctx); return 99;
}
'''

with tempfile.TemporaryDirectory(prefix="metalio-lifecycle-") as output:
    OUT = Path(output)
    for name, text in (("wifi", wifi_test), ("render", render_test)):
        source = OUT / (name + ".cpp")
        source.write_text(text, encoding="utf-8")
        obj = OUT / (name + ".obj")
        exe = OUT / (name + ".exe")
        subprocess.run([str(VC / "bin/Hostx64/x64/cl.exe"), "/nologo", "/utf-8", "/GS-", "/Zl",
                        "/Zc:threadSafeInit-", "/c", str(source), "/Fo" + str(obj)], check=True)
        subprocess.run([str(VC / "bin/Hostx64/x64/link.exe"), "/nologo", "/entry:main",
                        "/subsystem:console", "/nodefaultlib", str(obj), str(UM / "kernel32.lib"),
                        "/out:" + str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        print(name + " lifecycle fault injection: PASS")

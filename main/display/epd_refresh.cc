#include "epd_refresh.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "epd_flash_guard.h"
#include "epd_prepare.h"
#include "display_orient.h"
#include "epd_page_transition.h"
#include "epd_reader_aa.h"
#include "config.h"
#include "display.h"
#include "epd_board_metalio_eink4_plus.h"
#include "epd_highlevel.h"
#include "epdiy.h"
#include "frontlight.h"
#include "font_loader.h"
#include "metalio_epd.h"
#include "metalio_touch.h"
#include "power_hw.h"
#include "sd_log_writer.h"
#include "settings.h"
#include "my_waveform.h"
extern "C" {
#include "output_lcd/lcd_driver.h"
}
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#if defined(CONFIG_IDF_TARGET_ESP32S31)
extern "C" bool epd_pixel_order_selftest(void);
#endif

namespace {
constexpr const char* TAG = "EpdRefresh";
enum class PageDraw { None, Binary, Gray };
struct GrayRefresh {
    bool differential = false;
    bool reader_aa = false;
};
uint8_t *render_fb, *pending_fb, *effect_fb;
uint8_t* effect_source_fb;
EpdRippleColumn* ripple_columns;
size_t fb_bytes;
SemaphoreHandle_t mailbox;
TaskHandle_t worker;
std::atomic<int> refresh_profile{1};
std::atomic<bool> save_profile{false};
std::atomic<int> black_depth{0};
std::atomic<bool> save_black_depth{false};
constexpr int kBlackDepthMv[] = {1200, 1600, 2000};
bool pending_full, pending_stream, pending_light, pending_reader_aa;
bool panel_powered=false; // Worker-owned; retained only across a page animation.
bool pending_scroll=false, pending_scroll_settle=false;
std::atomic<bool> ui_scrolling{false}, next_scroll_settle{false};
int pending_turn;
int pending_orient = 0;
uint32_t submitted;
uint32_t drawing_sequence; // 仅刷新任务写入，用于关联一次提交中的多次绘制
std::atomic<uint32_t> completed{0}, updates{0}, errors{0}, coalesced{0}, last_ms{0};
std::atomic<int> last_error{0}, last_mode{0}, partials{0}, effect{1}, next_turn{0};
// Testing an animation must not enable it for ordinary book navigation.
std::atomic<int> reader_effect{static_cast<int>(EpdEffect::None)};
constexpr unsigned kScreenChange = 1;
constexpr unsigned kFullGc = 2;
std::atomic<unsigned> next_screen_change{0};
void (*after_submit)() = nullptr; // 下一次提交后的单次回调
unsigned pending_screen_change=0;
int64_t pending_at=0;
std::atomic<bool> busy{false}, test_active{false}, stop_test{false};
std::atomic<uint32_t> cancel_generation{0};
std::atomic<uint32_t> tests_submitted{0}, tests_completed{0}, test_ms{0}, test_frames{0};
std::atomic<int> test_mode{0};
struct TestJob { EpdPattern pattern; int mode; bool clean; int repeats; uint32_t generation, sequence; };
TestJob test_job{};
bool test_pending;
int pattern = -1; // Worker-owned. Test patterns survive status-bar updates.
int pattern_mode = MODE_GC16;
int pattern_phase;
bool state_unknown;
bool gray_on_panel; // Worker-owned: native gray must not become a new baseline.
bool startup_frame=true; // 首帧先完成白屏 GC 清理，再推白显示界面

void KeepWhiteBorder(uint8_t* fb) {
    constexpr int border=DISPLAY_WHITE_BORDER_PIXELS;
    const int row_bytes=epd_width()/2;
    for (int y=0;y<epd_height();++y) {
        uint8_t* row=fb+y*row_bytes;
        if (y<border || y>=epd_height()-border) memset(row,255,row_bytes);
        else { memset(row,255,border/2); memset(row+row_bytes-border/2,255,border/2); }
    }
}

void Put(uint8_t* fb, int x, int y, int color) {
    uint8_t& v = fb[y * (epd_width()/2) + x/2];
    v = x & 1 ? (v & 15) | (color << 4) : (v & 240) | color;
}
void PatternLabel(uint8_t* fb, const char* text, int x, int y, int size) {
    for (const char* c=text;*c;++c) {
        GlyphInfo g{};
        if (!get_glyph(*c,size,2,&g)) continue;
        for (int gy=0;gy<g.height;++gy) for (int gx=0;gx<g.width;++gx) {
            const int n=gy*g.width+gx;
            if (n/4>=g.bmp_len) continue;
            const int coverage=(g.bitmap[n/4]>>(6-2*(n&3)))&3;
            const int lx=x+g.x_offset+gx, ly=y+gy;
            if (lx>=24 && lx<DISPLAY_CONTENT_W-24 && ly>=540 && ly<1090)
                Put(fb,ly+DISPLAY_CONTENT_TOP_INSET,DISPLAY_WIDTH-1-(lx+DISPLAY_CONTENT_LEFT_INSET),
                    coverage>=2 ? 0 : 15);
        }
        x+=g.advance;
    }
}
void Pattern(uint8_t* fb, int selected, int phase = 0) {
    static constexpr int bayer[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    // Logical preview rectangle matches the test page. Controls remain usable.
    for (int y = 540; y < 1090; ++y) {
        for (int x = 24; x < DISPLAY_CONTENT_W-24; ++x) {
            int c = 15;
            switch (static_cast<EpdPattern>(selected)) {
                case EpdPattern::White: c = phase & 1 ? 0 : 15; break;
                case EpdPattern::Black: c = phase & 1 ? 15 : 0; break;
                case EpdPattern::Checker: c = ((x/24 + y/24 + phase)&1) ? 0 : 15; break;
                case EpdPattern::Gray: c = y<1020 ? (x-24)*16/(DISPLAY_CONTENT_W-48) : 15; break;
                case EpdPattern::Dither: {
                    const int level=(x-24)*16/(DISPLAY_CONTENT_W-48);
                    c=y>=1020 || bayer[y&3][x&3]<(level*16+7)/15 ? 15 : 0;
                    break;
                }
                case EpdPattern::Lines: c = ((x+phase*4)%16<2 || y%40<2) ? 0 : 15; break;
                case EpdPattern::Text: c = 15; break;
            }
            Put(fb, y+DISPLAY_CONTENT_TOP_INSET,
                DISPLAY_WIDTH-1-(x+DISPLAY_CONTENT_LEFT_INSET), c);
        }
        if ((y&63)==0) vTaskDelay(1);
    }
    if (selected==static_cast<int>(EpdPattern::Gray) || selected==static_cast<int>(EpdPattern::Dither)) {
        for (int level=0;level<16;++level) {
            char label[3]; snprintf(label,sizeof(label),"%02d",level);
            PatternLabel(fb,label,29+level*(DISPLAY_CONTENT_W-48)/16,1025,18);
        }
        PatternLabel(fb,selected==static_cast<int>(EpdPattern::Gray)?"GC/GL native 0=black 15=white":"B/W dither reference 0=black 15=white",40,1057,18);
    }
    if (selected==static_cast<int>(EpdPattern::Text)) {
        const char* sample="Metalio 0123456789 AaBb Ww Mm";
        for (int row=0;row<8;++row) {
            int x=42;
            for (const char* c=sample;*c;++c) {
                GlyphInfo g{};
                if (!get_glyph(*c,25,2,&g)) continue;
                for (int gy=0;gy<g.height;++gy) for(int gx=0;gx<g.width;++gx) {
                    int n=gy*g.width+gx;
                    if (n/4>=g.bmp_len) continue;
                    int coverage=(g.bitmap[n/4]>>(6-2*(n&3)))&3;
                    int lx=x+g.x_offset+gx, ly=565+row*60+gy;
                    if (lx>=24 && lx<DISPLAY_CONTENT_W-24 && ly<1090)
                        Put(fb,ly+DISPLAY_CONTENT_TOP_INSET,DISPLAY_WIDTH-1-(lx+DISPLAY_CONTENT_LEFT_INSET),
                            pattern_mode==MODE_DU ? (coverage>=2?0:15) : 15-coverage*5);
                }
                x+=g.advance;
            }
        }
    }
}
const EpdWaveform* ProfileWaveform(int profile, int mode=MODE_DU, bool reader_aa=false) {
    if (mode==MODE_GL16 && (profile==3 || (reader_aa && profile>=1))) return my_waveform_gray8();
    if (profile>=1 && my_waveform_fast()) return my_waveform_fast();
    return &MY_WAVEFORM;
}
EpdDrawError Draw(EpdiyHighlevelState* hl, int mode, bool full, bool clean, bool keep_power=false,
                  PageDraw page=PageDraw::None, int orient=0, GrayRefresh gray={}) {
    const bool cleanup=full || clean || state_unknown;
    // Decide against the actual old/new buffers at submission, not only the
    // earlier worker analysis: an animation may have advanced the baseline.
    // Unchanged gray does not promote a black/white-only update to GL16.
    if (pattern<0 && !cleanup && page==PageDraw::None && mode==MODE_DU &&
        EpdAnalyzeChanges(hl->back_fb,hl->front_fb,fb_bytes).gray_changed) {
        mode=MODE_GL16;
    }
    const bool complete_waveform=full || state_unknown || (clean && mode==MODE_GC16);
    const bool reveal=page==PageDraw::Binary;
    const bool gray_reveal=page==PageDraw::Gray;
    const int profile=refresh_profile.load();
    const EpdWaveform* spatial=gray_reveal ? my_waveform_wipe_gl(profile) :
        (reveal ? (profile>=2 ? my_waveform_wipe_reveal_short() : my_waveform_wipe_reveal()) : nullptr);
    if(page!=PageDraw::None && !spatial) {
        last_error.store(EPD_DRAW_FAILED_ALLOC);
        ++errors;
        ESP_LOGW(TAG,"animation waveform allocation failed; fall back to full GC");
        return EPD_DRAW_FAILED_ALLOC;
    }
    // The existing 30-phase optical 8-gray table is sufficient for A2 text edges.
    // Reuse it unchanged for reading (including intermediate frames); cleaning stays GC16.
    const bool ota_progress = OtaUpgradeScreen::IsActive() && !cleanup && page==PageDraw::None;
    const EpdWaveform* ordinary = (profile>=2 || ota_progress) && mode==MODE_DU
        ? (profile==2 && !ota_progress ? my_waveform_fast_du() : my_waveform_short_du())
        : ProfileWaveform(profile,mode,gray.reader_aa && !cleanup);
    const EpdWaveform* waveform=complete_waveform ? &MY_WAVEFORM : (spatial ? spatial : ordinary);
    const char* path=complete_waveform ? "full" : (spatial ? (gray_reveal ? "wipe-gray" : "wipe-bw") :
        (ota_progress && mode==MODE_DU ? "ota-du" : "normal"));
    int phases=0;
    for (int i=0;i<waveform->num_modes;++i) {
        if (waveform->mode_data[i]->type==mode) {
            phases=waveform->mode_data[i]->range_data[0]->phases;
            break;
        }
    }
    if (gray_reveal && !complete_waveform) phases+=9;
    epd_hl_waveform(hl,waveform);
    KeepWhiteBorder(hl->front_fb);
    // Serialize the complete waveform (including recovery clears) with OTA flash
    // writes; holding only the log storage lock does not protect against OTA.
    EpdFlashGuard flash_guard;
    // Flash writes pause cache/other-core execution and starve LCD bounce refill.
    // CaptureLog still buffers messages; the logger flushes after the waveform ends.
    struct LogStorageGuard {
        LogStorageGuard() { SdLogWriter_LockForDisplayRefresh(); }
        ~LogStorageGuard() { SdLogWriter_UnlockForDisplayRefresh(); }
    } log_storage_guard;
    const int64_t start = esp_timer_get_time();
    Display::SetPanelBusy(true);
    PowerHwCpuFreqHold cpu_freq_hold;
    metalio_touch_set_feed_paused(true);
    if (!panel_powered) {
        epd_poweron();
        panel_powered=true;
    }
    if (startup_frame) {
        assert(epd_lcd_white_border_pixels()==DISPLAY_WHITE_BORDER_PIXELS);
        epd_lcd_prime_white_border(true);
    }
    metalio_touch_set_feed_paused(false);
    EpdDrawError result = EPD_DRAW_SUCCESS;
    if (!epd_board_metalio_eink4_plus_tps_ok()) {
        result = EPD_DRAW_EMPTY_LINE_QUEUE;
    } else {
        if (page!=PageDraw::None && !cleanup) {
            // Pack every pixel explicitly so equality/dirtiness checks cannot
            // suppress conditioning pulses. Binary pages encode band/flags;
            // gray pages retain target/old gray and use renderer band delays.
            if (reveal) EpdPackBinaryReveal(hl->difference_fb,hl->back_fb,hl->front_fb,
                                            epd_width(),epd_height(),orient);
            else EpdPackDifference(hl->difference_fb,hl->back_fb,hl->front_fb,fb_bytes);
            const int spatial_mode=gray_reveal ? MODE_GL16 | MODE_SPATIAL_WIPE_GL | ((orient&3)<<13) : MODE_DU;
            result=epd_draw_base(epd_full_screen(),hl->difference_fb,epd_full_screen(),
                static_cast<EpdDrawMode>(spatial_mode | MODE_PACKING_1PPB_DIFFERENCE),
                25,nullptr,nullptr,spatial);
            if (result==EPD_DRAW_SUCCESS) {
                memcpy(hl->back_fb,hl->front_fb,fb_bytes);
            }
        } else if (clean || state_unknown) {
            // Only reset the old-frame baseline AFTER physically clearing the panel.
            result = epd_clear_checked();
            if (result == EPD_DRAW_SUCCESS)
                result = epd_hl_update_screen_from_white(hl, static_cast<EpdDrawMode>(mode), 25);
        } else if (full || mode == MODE_GC16 || pattern>=0 ||
                   (mode == MODE_GL16 && !gray.differential)) {
            // Diagnostic patterns intentionally exercise their requested
            // waveform, including equal-value conditioning, like full draws.
            result = epd_hl_update_screen_full(hl, static_cast<EpdDrawMode>(mode), 25);
        } else {
            result = epd_hl_update_screen(hl, static_cast<EpdDrawMode>(mode), 25);
        }
    }
    const auto initial_result=result;
    const bool recovered=(result & EPD_DRAW_EMPTY_LINE_QUEUE) && epd_board_metalio_eink4_plus_tps_ok();
    if (recovered) {
        // An interrupted waveform leaves the optical state unknown. Recover
        // once while flash is still excluded; another differential update cannot
        // safely use the old framebuffer as its physical baseline.
        ESP_LOGW(TAG, "scan underrun; recover with clear and full GC16");
        epd_hl_waveform(hl, &MY_WAVEFORM);
        result = epd_clear_checked();
        if (result != EPD_DRAW_SUCCESS) {
            ESP_LOGW(TAG, "clear interrupted; reset panel power and retry once");
            epd_poweroff();
            panel_powered = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            epd_poweron();
            panel_powered = true;
            result = epd_clear_checked();
        }
        if (result == EPD_DRAW_SUCCESS) {
            result = epd_hl_update_screen_from_white(hl, MODE_GC16, 25);
        }
        mode = MODE_GC16;
        full = true;
        if (result != EPD_DRAW_SUCCESS) {
            ESP_LOGE(TAG, "scan recovery failed: 0x%x", static_cast<unsigned>(result));
        }
    }
    if (startup_frame) {
        epd_lcd_prime_white_border(false);
        if (result==EPD_DRAW_SUCCESS) startup_frame=false;
    }
    metalio_touch_set_feed_paused(true);
    if (!keep_power || result!=EPD_DRAW_SUCCESS) {
        epd_poweroff();
        panel_powered=false;
    }
    metalio_touch_set_feed_paused(false);
    Display::SetPanelBusy(panel_powered);
    state_unknown = result != EPD_DRAW_SUCCESS;
    if (state_unknown) ++errors;
    last_error.store(result);
    last_mode.store(mode);
    last_ms.store((esp_timer_get_time()-start)/1000);
    ++updates;
    ESP_LOGI(TAG, "seq=%u profile=%d/%d path=%s phases=%d orient=%d aa=%d "
             "mode=%d full=%d clean=%d recover=%d first_err=0x%x cost=%ums err=0x%x",
             (unsigned)drawing_sequence,profile,refresh_profile.load(),path,phases,orient,
             gray.reader_aa,mode,full,clean,recovered,(unsigned)initial_result,
             (unsigned)last_ms.load(),(unsigned)result);
    return result;
}
EpdDrawError FullRefreshBeforeFrame(EpdiyHighlevelState* hl) {
    if (!effect_fb) {
        last_error.store(EPD_DRAW_FAILED_ALLOC);
        ++errors;
        ESP_LOGE(TAG, "screen cleaning failed: no transition buffer");
        return EPD_DRAW_FAILED_ALLOC;
    }
    uint8_t* target=hl->front_fb;
    memset(effect_fb,255,fb_bytes);
    hl->front_fb=effect_fb;
    const auto result=Draw(hl,MODE_GC16,true,false,true);
    hl->front_fb=target;
    if (result==EPD_DRAW_SUCCESS) gray_on_panel=false;
    return result;
}
void Transition(EpdiyHighlevelState* hl, int direction, EpdEffect selected_effect, int orient = 0,
                bool gray = false, bool reader_aa = false) {
    if (!effect_fb || !direction || selected_effect==EpdEffect::None || state_unknown || gray_on_panel ||
        pattern==static_cast<int>(EpdPattern::Gray)) return;
    const bool ripple = selected_effect==EpdEffect::Ripple;
    if (ripple && (!effect_source_fb || !ripple_columns)) return;
    uint8_t* target = hl->front_fb;
    const auto logical = EpdPageLogicalPoint(0, 0, epd_width(), epd_height(), orient);
    const bool landscape = orient==1 || orient==2;
    const int left = landscape ? DISPLAY_CONTENT_TOP_INSET : DISPLAY_CONTENT_LEFT_INSET;
    const int top = landscape ? DISPLAY_CONTENT_LEFT_INSET : DISPLAY_CONTENT_TOP_INSET;
    const int width = logical.width - left - (landscape ? DISPLAY_CONTENT_BOTTOM_INSET : DISPLAY_CONTENT_RIGHT_INSET);
    const int height = logical.height - top - (landscape ? DISPLAY_CONTENT_RIGHT_INSET : DISPLAY_CONTENT_BOTTOM_INSET);
    if (ripple) {
        if (width<=0 || height<=0) return;
        // The physical back frame changes after every update. Sample a frozen old
        // page so a moving lens never magnifies an already warped frame.
        memcpy(effect_source_fb, hl->back_fb, fb_bytes);
    }
    const int frames = ripple ? kEpdRippleSteps : 2;
    const int64_t animation_start=esp_timer_get_time();
    int64_t previous_frame_us=0;
    for (int step=1; step<=frames; ++step) {
        if (stop_test.load()) break;
        // A slow panel must not queue all intermediate frames ahead of the page.
        if (ripple && step>1 && esp_timer_get_time()-animation_start+previous_frame_us>kEpdRippleBudgetUs) break;
        const int64_t frame_start=esp_timer_get_time();
        const auto wave = EpdMakeRippleFrame(width, height, direction, step);
        if (ripple) {
            EpdComposeRippleFrame(effect_fb,effect_source_fb,target,ripple_columns,wave,
                left,top,epd_width(),epd_height(),orient);
        } else EpdComposePageFrame(effect_fb,hl->back_fb,target,epd_width(),epd_height(),orient,direction,step);
        hl->front_fb=effect_fb;
        // The caller always draws the final page and closes the power session.
        auto result=Draw(hl, gray ? MODE_GL16 : MODE_DU, false, false, true, PageDraw::None, orient, {gray,reader_aa});
        hl->front_fb=target;
        previous_frame_us=esp_timer_get_time()-frame_start;
        if (result != EPD_DRAW_SUCCESS) break;
    }
}
void Worker(void*) {
    auto* hl=MetalioEpd_Hl();
    uint32_t consumed=0;
    int applied_profile=refresh_profile.load();
    epd_hl_waveform(hl,ProfileWaveform(applied_profile));
    bool profile_full=true;
    int applied_depth=black_depth.load();
    epd_set_vcom(kBlackDepthMv[applied_depth]);
    int64_t last_full=esp_timer_get_time();
    for (;;) {
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));
        bool full=false, stream=false, light=false, frame=false, test=false, screen_change=false;
        bool scrolling=false, scroll_settle=false, reader_aa=false, full_gc=false;
        int turn=0, orient=0;
        uint32_t sequence=consumed;
        TestJob job{};
        xSemaphoreTake(mailbox, portMAX_DELAY);
        if (submitted!=consumed) {
            // Merge navigation construction/status invalidations into one target.
            if (pending_screen_change && esp_timer_get_time()-pending_at<60000) {
                xSemaphoreGive(mailbox); continue;
            }
            memcpy(hl->front_fb,pending_fb,fb_bytes);
            sequence=submitted; full=pending_full; stream=pending_stream;
            light=pending_light; turn=pending_turn; orient=pending_orient;
            reader_aa=pending_reader_aa;
            scrolling=pending_scroll; scroll_settle=pending_scroll_settle; pending_scroll_settle=false;
            screen_change=(pending_screen_change & kScreenChange)!=0;
            full_gc=(pending_screen_change & kFullGc)!=0; pending_screen_change=0;
            pending_full=pending_light=false; pending_turn=0;
            frame=true;
        }
        if (test_pending) { job=test_job; test_pending=false; test=true; }
        if (frame || test) busy.store(true);
        xSemaphoreGive(mailbox);
        drawing_sequence=sequence;
        const bool persist_profile=save_profile.exchange(false);
        const int selected_profile=refresh_profile.load();
        if (selected_profile!=applied_profile) {
            epd_hl_waveform(hl,ProfileWaveform(selected_profile));
            applied_profile=selected_profile;
            profile_full=true;
        }
        if (persist_profile) {
            Settings settings("screen",true);
            settings.SetInt("refresh_profile",selected_profile);
        }
        const bool persist_depth=save_black_depth.exchange(false);
        const int selected_depth=black_depth.load();
        if (selected_depth!=applied_depth) {
            // 与 P14 复位共用闸门，VCOM/关屏也须等待断电窗口结束。
            EpdFlashGuard panel_guard;
            if (panel_powered) { epd_poweroff(); panel_powered=false; }
            epd_set_vcom(kBlackDepthMv[selected_depth]);
            applied_depth=selected_depth;
            profile_full=true;
            ESP_LOGI(TAG,"black depth=%d vcom=%d mV",selected_depth,kBlackDepthMv[selected_depth]);
        }
        if (persist_depth) {
            Settings settings("screen",true);
            settings.SetInt("black_depth",selected_depth);
        }
        // 退出测试时保留物理灰阶状态，由下一次推白或必要清屏更新基线。
        if (!test_active.load()) { pattern=-1; test=false; if (job.sequence) tests_completed.store(job.sequence); }
        if (frame || test) busy.store(true);
        if (test) {
            unsigned drawn=0;
            pattern=static_cast<int>(job.pattern); pattern_mode=job.mode;
            for (int i=0;i<job.repeats && test_active.load() && job.generation==cancel_generation.load();++i) {
                pattern_phase=i;
                Pattern(hl->front_fb,pattern,pattern_phase);
                if (i==0) {
                    const int requested_turn=next_turn.exchange(0);
                    if (turn || requested_turn) Transition(hl,turn ? turn : requested_turn,
                        static_cast<EpdEffect>(effect.load()), orient);
                }
                // Pico's native-gray test always starts from physical white and
                // clears again before leaving it. Repeated gray tests need the
                // same starting state, even when the logical pixels are equal.
                const bool native_gray=job.pattern==EpdPattern::Gray;
                const auto result=Draw(hl,job.mode,false,
                    native_gray || gray_on_panel || (job.clean && i==0));
                if (result==EPD_DRAW_SUCCESS) gray_on_panel=native_gray;
                ++drawn; test_ms.store(last_ms.load()); test_mode.store(job.mode);
                if (result!=EPD_DRAW_SUCCESS) break;
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            frame=false; partials.store(0);
            test_frames.store(drawn); tests_completed.store(job.sequence);
        }
        if (frame) {
            if (pattern>=0) Pattern(hl->front_fb,pattern,pattern_phase);
            const uint32_t draws_before=updates.load();
            const bool clean_reveal=full_gc || startup_frame;
            const auto preparation=clean_reveal ? FullRefreshBeforeFrame(hl) : EPD_DRAW_SUCCESS;
            const auto analysis=EpdAnalyzeChanges(hl->back_fb,hl->front_fb,fb_bytes);
            const int changed=analysis.changed;
            const bool binary_pages=analysis.binary,gray_changed=analysis.gray_changed;
            const bool gray_page_turn=turn && !analysis.binary;
            const int partials_before=partials.load();
            const int64_t full_age_ms=(esp_timer_get_time()-last_full)/1000;
            // 开机、待机和关机先对白屏全刷，再推白显现目标；普通切页沿用现有动画。
            // 小范围状态更新同样累计局刷次数，避免长期刷新却无法触发清屏。
            bool promote=!clean_reveal && (profile_full || full || state_unknown || (pattern<0 && !stream && !scrolling && !scroll_settle && changed>0 &&
                partials.load()>=24 && esp_timer_get_time()-last_full>=60000000));
            const auto page_effect=static_cast<EpdEffect>(pattern<0 ? reader_effect.load() : effect.load());
            const bool wipe_turn=turn && page_effect==EpdEffect::Wipe;
            const int wipe_orient=screen_change || clean_reveal ? orient : EpdWipeOrient(orient,turn);
            bool combined=false;
            if ((screen_change || wipe_turn || clean_reveal) && !promote && preparation==EPD_DRAW_SUCCESS) {
                if (pattern<0 && !gray_on_panel && binary_pages && effect_fb) {
                    combined=Draw(hl,MODE_DU,false,false,false,PageDraw::Binary,wipe_orient)==EPD_DRAW_SUCCESS;
                    if (!combined) promote=true;
                } else if (pattern<0 && !gray_on_panel && effect_fb) {
                    combined=Draw(hl,MODE_GL16,false,false,false,PageDraw::Gray,wipe_orient)==EPD_DRAW_SUCCESS;
                    if (!combined) promote=true;
                } else promote=true;
            }
            if (!promote && !screen_change && !wipe_turn && !clean_reveal) Transition(hl,turn,page_effect,orient,gray_page_turn,reader_aa);
            // A failed intermediate frame invalidates the physical baseline.
            promote |= state_unknown;
            // A status/button redraw must not recondition unchanged native gray bars.
            // Difference DU touches only changed binary UI pixels; the test job owns
            // explicit GC/GL pattern updates and deep cleaning.
            const bool quality_turn=turn && page_effect==EpdEffect::Page;
            int mode=promote ? MODE_GC16 : ((gray_changed || scroll_settle || (!scrolling && (quality_turn || gray_page_turn))) ? MODE_GL16 : MODE_DU);
            const bool reset_gray=!full_gc && gray_on_panel &&
                (pattern!=static_cast<int>(EpdPattern::Gray) || promote || turn);
            const auto result=preparation!=EPD_DRAW_SUCCESS ? preparation :
                (!combined && (changed || promote || reset_gray || screen_change || turn || scroll_settle)
                ? Draw(hl,mode,promote,reset_gray,false,PageDraw::None,orient,{!scroll_settle,reader_aa}) : EPD_DRAW_SUCCESS);
            // 每个实际上屏的目标帧只记录一次，扫描期间不打印或遍历额外像素。
            if (updates.load()!=draws_before || result!=EPD_DRAW_SUCCESS) {
                ESP_LOGI(TAG,"route seq=%u merged=%u screen=%d full_gc=%d turn=%d stream=%d scroll=%d settle=%d "
                         "changed=%d binary=%d gray=%d promote=%d combined=%d partials=%d gc_age=%lldms",
                         (unsigned)sequence,(unsigned)(sequence-consumed-1),screen_change,full_gc,turn,stream,scrolling,scroll_settle,
                         changed,binary_pages,gray_changed,promote,combined,partials_before,(long long)full_age_ms);
            }
            if (result==EPD_DRAW_SUCCESS) {
                profile_full=false;
                gray_on_panel=pattern==static_cast<int>(EpdPattern::Gray);
                if (mode==MODE_GC16 || clean_reveal) { partials.store(0); last_full=esp_timer_get_time(); }
                else if (changed>0) ++partials;
                if (light) Frontlight::GetInstance().RestoreBrightness();
            }
        }
        consumed=sequence;
        completed.store(consumed,std::memory_order_release);
        busy.store(false);
    }
}
}

bool EpdRefreshInit() {
    if (worker) return true;
    {
        Settings settings("screen");
        int saved=settings.GetInt("refresh_profile",-1);
        if (saved<0) {
            const int legacy=settings.GetInt("refresh_mode",-1);
            saved=legacy==0 ? 0 : 1; // Preserve old full/trimmed selections.
        }
        if (saved<0 || saved>3) saved=1;
        if (saved>=1 && !my_waveform_fast()) saved=0;
        refresh_profile.store(saved);
        ESP_LOGI(TAG,"refresh profile=%d (0=clear 1=balanced 2=fast 3=extreme)",saved);
        const int depth=settings.GetInt("black_depth",0);
        black_depth.store(depth>=0 && depth<=2 ? depth : 0);
    }
#if defined(CONFIG_IDF_TARGET_ESP32S31)
    if (!epd_pixel_order_selftest()) {
        ESP_LOGE(TAG,"S31 pixel/LUT/mask contract failed"); return false;
    }
    ESP_LOGI(TAG,"S31 self-test passed (32 masks, 24576 gray transitions, 8192 white-border pixels)");
#endif
    fb_bytes=static_cast<size_t>(epd_width()/2)*epd_height();
    // EPDiy's vector difference routine requires 16-byte-aligned source buffers.
    render_fb=static_cast<uint8_t*>(heap_caps_aligned_alloc(16,fb_bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    pending_fb=static_cast<uint8_t*>(heap_caps_aligned_alloc(16,fb_bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    effect_fb=static_cast<uint8_t*>(heap_caps_aligned_alloc(16,fb_bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    // One immutable old page plus a small column table, allocated once.
    const size_t column_bytes=std::max(epd_width(),epd_height())*sizeof(EpdRippleColumn);
    effect_source_fb=static_cast<uint8_t*>(heap_caps_aligned_alloc(16,fb_bytes+column_bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (effect_source_fb) ripple_columns=reinterpret_cast<EpdRippleColumn*>(effect_source_fb+fb_bytes);
    else ESP_LOGW(TAG,"vertical ripple disabled: no source-frame memory");
    mailbox=xSemaphoreCreateMutex();
    if (!render_fb || !pending_fb || !mailbox) return false;
    memset(render_fb,255,fb_bytes); memset(pending_fb,255,fb_bytes);
    return xTaskCreatePinnedToCore(Worker,"epd_refresh",6144,nullptr,2,&worker,1)==pdPASS;
}
uint8_t* EpdRenderBuffer() { return render_fb; }
uint32_t EpdSubmit(bool full, bool stream, bool restore_light) {
    if (!worker) return 0;
    xSemaphoreTake(mailbox,portMAX_DELAY);
    if (submitted!=completed.load()) ++coalesced;
    memcpy(pending_fb,render_fb,fb_bytes);
    pending_full |= full; pending_stream=stream; pending_light |= restore_light;
    pending_reader_aa=EpdReaderAaEnabled();
    pending_scroll=ui_scrolling.load();
    pending_scroll_settle |= next_scroll_settle.exchange(false);
    if (pending_scroll) pending_scroll_settle=false;
    pending_screen_change |= next_screen_change.exchange(false);
    pending_at=esp_timer_get_time();
    const int turn=next_turn.exchange(0);
    if (turn) pending_turn=turn;
    pending_orient=DisplayUiGetOrient();
    uint32_t seq=++submitted;
    xSemaphoreGive(mailbox);
    xTaskNotifyGive(worker);
    void (*fn)() = after_submit;
    after_submit = nullptr;
    if (fn) fn();
    return seq;
}
void EpdAfterNextSubmit(void (*fn)()) { after_submit = fn; }
bool EpdWait(uint32_t sequence, uint32_t timeout_ms) {
    if (!sequence) return false;
    const int64_t until=esp_timer_get_time()+timeout_ms*1000LL;
    while (static_cast<int32_t>(completed.load(std::memory_order_acquire)-sequence)<0) {
        if (esp_timer_get_time()>until) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return last_error.load()==0;
}
EpdRefreshStats EpdGetStats() {
    uint32_t seq=0; bool pending=false;
    if (mailbox) { xSemaphoreTake(mailbox,portMAX_DELAY); seq=submitted; pending=test_pending; xSemaphoreGive(mailbox); }
    return {seq,completed.load(),coalesced.load(),updates.load(),errors.load(),last_ms.load(),
            last_error.load(),last_mode.load(),partials.load(),busy.load() || pending || seq!=completed.load(),test_active.load(),
            tests_submitted.load(),tests_completed.load(),test_ms.load(),test_frames.load(),test_mode.load()};
}
void EpdSetEffect(EpdEffect value) { effect.store(static_cast<int>(value)); }
void EpdSetReaderEffect(EpdEffect value) { reader_effect.store(static_cast<int>(value)); }
void EpdRequestPageTurn(int direction) { stop_test.store(false); next_turn.store(direction); }
void EpdCancelPageTurn() {
    stop_test.store(true); next_turn.store(0);
    if (mailbox) {
        xSemaphoreTake(mailbox,portMAX_DELAY);
        pending_turn=0;
        xSemaphoreGive(mailbox);
    }
}
void EpdRequestScreenChange(bool full_gc) {
    next_screen_change.fetch_or(kScreenChange | (full_gc ? kFullGc : 0));
}
void EpdSetTestActive(bool active) { test_active.store(active); if (!active) EpdTestStop(); }
bool EpdTestRequest(EpdPattern p,int mode,bool clean,int repeats) {
    if (!worker || !test_active.load() || repeats<1 || repeats>20 ||
        (p==EpdPattern::Gray && mode==MODE_DU) ||
        (mode!=MODE_DU && mode!=MODE_GL16 && mode!=MODE_GC16)) return false;
    xSemaphoreTake(mailbox,portMAX_DELAY);
    bool ok=!test_pending;
    if (ok) { stop_test.store(false); test_job={p,mode,clean,repeats,cancel_generation.load(),++tests_submitted}; test_pending=true; }
    xSemaphoreGive(mailbox);
    if (ok) xTaskNotifyGive(worker);
    return ok;
}
void EpdTestStop() { ++cancel_generation; stop_test.store(true); }

void EpdSetUiScrolling(bool scrolling) { ui_scrolling.store(scrolling); if (scrolling) next_scroll_settle.store(false); }
void EpdRequestUiScrollSettle() { next_scroll_settle.store(true); }

EpdRefreshProfile EpdGetRefreshProfile() {
    return static_cast<EpdRefreshProfile>(refresh_profile.load());
}
bool EpdSetRefreshProfile(EpdRefreshProfile profile) {
    if (!worker || (profile!=EpdRefreshProfile::Clear && profile!=EpdRefreshProfile::Balanced && profile!=EpdRefreshProfile::Fast && profile!=EpdRefreshProfile::Extreme)) return false;
    if (profile!=EpdRefreshProfile::Clear && !my_waveform_fast()) return false;
    if (refresh_profile.exchange(static_cast<int>(profile))!=static_cast<int>(profile)) {
        save_profile.store(true);
        xTaskNotifyGive(worker);
    }
    return true;
}

EpdBlackDepth EpdGetBlackDepth() {
    return static_cast<EpdBlackDepth>(black_depth.load());
}
bool EpdSetBlackDepth(EpdBlackDepth depth) {
    const int value=static_cast<int>(depth);
    if (!worker || value<0 || value>2) return false;
    if (black_depth.exchange(value)!=value) {
        save_black_depth.store(true);
        xTaskNotifyGive(worker);
    }
    return true;
}

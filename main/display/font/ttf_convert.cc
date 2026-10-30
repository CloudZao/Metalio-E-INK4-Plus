#include "assets/lang_reader.h"
#include "assets/lang_config.h"
#include "ttf_convert.h"

#include "epdfont.h"
#include "ttf_face.h"
#include "sd_access.h"
#include "sd_paths.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

static_assert(sizeof(epdfont_header_t) == 64, "epdfont_header_t size");
static_assert(sizeof(epdfont_interval_t) == 12, "epdfont_interval_t size");
static_assert(sizeof(epdfont_glyph_t) == 16, "epdfont_glyph_t size");

namespace ttf_convert {

namespace {

constexpr const char* TAG = "ttf_conv";
constexpr int kStateIdle = 0;
constexpr int kStateRunning = 1;
constexpr int kStateDone = 2;
constexpr int kStateFailed = 3;
constexpr uint32_t kMaxCharsetSize = 50000;
/** FreeType 栅格化栈深：LVGL 亦要求 ≥32KB；SPIRAM 任务栈用 48KB 留余量。 */
constexpr uint32_t kWorkerStack = 48 * 1024;
constexpr UBaseType_t kWorkerPrio = tskIDLE_PRIORITY + 2;
constexpr BaseType_t kWorkerCore = 0;
/** 每处理这么多码点让出 CPU，避免饿死 LVGL / 喂狗。 */
constexpr size_t kYieldEvery = 32;
constexpr FT_Int32 kLoadFlags = FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL;

std::atomic<int> s_state{kStateIdle};
std::atomic<bool> s_worker_active{false};
std::atomic<bool> s_cancel{false};
std::atomic<int> s_percent{0};
std::atomic<bool> s_io_failed{false};
char s_error[64] = "";

struct GlyphRec {
    uint32_t cp;
    uint8_t w;
    uint8_t h;
    uint16_t adv;
    int16_t ox;
    int16_t oy;
    uint16_t bmp_len;
    uint32_t bmp_off;
};

struct Job {
    std::string ttf_path;
    std::string template_path;
    std::string out_dir;
    uint16_t sizes[3];
    int size_count;
};

bool Cancelled() {
    return s_cancel.load(std::memory_order_acquire) || sd_access::IsQuiescing();
}

void Fail(const char* msg) {
    std::snprintf(s_error, sizeof(s_error), "%s", msg ? msg : Lang::Strings::COMMON_UNKNOWN);
    ESP_LOGE(TAG, "failed: %s", s_error);
    s_state.store(kStateFailed, std::memory_order_release);
}

void FinishOk() {
    s_percent.store(100, std::memory_order_release);
    s_state.store(kStateDone, std::memory_order_release);
    ESP_LOGI(TAG, "done");
}

/** WithCaps 创建的任务必须用 WithCaps 删除，否则栈块泄漏。 */
[[noreturn]] void ExitWorker() {
    vTaskDeleteWithCaps(nullptr);
}

bool LoadCharset(const char* template_ef_path, std::vector<uint32_t>* cps) {
    if (template_ef_path == nullptr || template_ef_path[0] == '\0') {
        // 与 PC 工具 fontconvert.py 的 reading_zh 范围一致，首次导入无需 SD 字库。
        constexpr uint32_t ranges[][2] = {
            {0x0020, 0x007E}, {0x00A0, 0x00FF}, {0x2000, 0x206F},
            {0x3000, 0x303F}, {0x4E00, 0x9FA5}, {0xFF00, 0xFFEF},
        };
        size_t count = 0;
        for (const auto& range : ranges) {
            count += range[1] - range[0] + 1;
        }
        cps->clear();
        try {
            cps->reserve(count);
            for (const auto& range : ranges) {
                for (uint32_t cp = range[0]; cp <= range[1]; ++cp) {
                    cps->push_back(cp);
                }
            }
        } catch (...) {
            cps->clear();
            return false;
        }
        return true;
    }
    FILE* f = fopen(template_ef_path, "rb");
    if (f == nullptr) {
        return false;
    }
    epdfont_header_t h{};
    bool ok = fread(&h, sizeof(h), 1, f) == 1;
    if (!ok || std::memcmp(h.magic, EPDFONT_MAGIC, 8) != 0 || h.version != EPDFONT_VERSION) {
        fclose(f);
        return false;
    }
    if (h.interval_count == 0 || h.interval_count > 100000 ||
        fseek(f, static_cast<long>(h.intervals_offset), SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    std::vector<epdfont_interval_t> ivs;
    try {
        ivs.resize(h.interval_count);
    } catch (...) {
        fclose(f);
        return false;
    }
    ok = fread(ivs.data(), sizeof(epdfont_interval_t), h.interval_count, f) == h.interval_count;
    fclose(f);
    if (!ok) {
        return false;
    }
    // 设备端解析器按 intervals 升序二分（epdfont.cc FindInterval），
    // 模板乱序/重叠会产出查不到字的字体：直接判坏，不静默丢字。
    cps->clear();
    try {
        cps->reserve(std::min<size_t>(h.glyph_count > 0 ? h.glyph_count : 4096, kMaxCharsetSize));
    } catch (...) {
        return false;
    }
    uint32_t prev_last = 0;
    bool first_iv = true;
    for (const auto& iv : ivs) {
        if (iv.last < iv.first || iv.last - iv.first > kMaxCharsetSize ||
            (!first_iv && iv.first <= prev_last)) {
            cps->clear();
            return false;
        }
        first_iv = false;
        prev_last = iv.last;
        for (uint32_t c = iv.first;; ++c) {
            try {
                cps->push_back(c);
            } catch (...) {
                cps->clear();
                return false;
            }
            if (cps->size() > kMaxCharsetSize) {
                cps->clear();
                return false;
            }
            if (c == iv.last) {
                break;
            }
        }
    }
    return !cps->empty();
}

/** 8bpp 灰度 → 2bpp MSB-first（4 像素/字节），阈值与 PC 工具一致。out 调用前须已清零。 */
void Quantize2bpp(const uint8_t* gray, int pitch, int w, int h, uint8_t* out) {
    size_t bit_i = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = gray + static_cast<size_t>(y) * pitch;
        for (int x = 0; x < w; ++x) {
            uint8_t v = row[x];
            uint8_t q = (v < 32) ? 0 : (v < 96) ? 1 : (v < 160) ? 2 : 3;
            out[bit_i >> 2] |= static_cast<uint8_t>(q << (6 - 2 * (bit_i & 3)));
            ++bit_i;
        }
    }
}

std::string FontStem(const char* ttf_path) {
    if (ttf_path == nullptr || ttf_path[0] == '\0') return {};
    const char* base = std::strrchr(ttf_path, '/');
    std::string stem = base ? base + 1 : ttf_path;
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos) stem.resize(dot);
    return stem;
}

bool WriteEf(const char* out_path, int line_height, int base_line,
             const std::vector<GlyphRec>& recs, const uint8_t* blob, size_t blob_size) {
    if (recs.empty() || out_path == nullptr || out_path[0] == '\0') {
        return false;
    }
    struct Interval {
        uint32_t first;
        uint32_t last;
        uint32_t gidx;
    };
    std::vector<Interval> ivs;
    uint32_t start = recs[0].cp;
    uint32_t prev = recs[0].cp;
    size_t start_idx = 0;
    for (size_t i = 1; i < recs.size(); ++i) {
        uint32_t cp = recs[i].cp;
        if (cp == prev + 1) {
            prev = cp;
            continue;
        }
        ivs.push_back({start, prev, static_cast<uint32_t>(start_idx)});
        start = cp;
        prev = cp;
        start_idx = i;
    }
    ivs.push_back({start, prev, static_cast<uint32_t>(start_idx)});

    epdfont_header_t h{};
    std::memcpy(h.magic, EPDFONT_MAGIC, 8);
    h.version = EPDFONT_VERSION;
    h.flags = 2;  // bpp
    h.line_height = static_cast<uint16_t>(line_height);
    h.base_line = static_cast<uint16_t>(base_line);
    h.ascender = static_cast<int16_t>(line_height - base_line);
    h.descender = static_cast<int16_t>(-base_line);
    h.interval_count = static_cast<uint32_t>(ivs.size());
    h.glyph_count = static_cast<uint32_t>(recs.size());
    h.intervals_offset = sizeof(epdfont_header_t);
    h.glyphs_offset = h.intervals_offset + static_cast<uint32_t>(ivs.size() * sizeof(epdfont_interval_t));
    h.bitmaps_offset = h.glyphs_offset + static_cast<uint32_t>(recs.size() * sizeof(epdfont_glyph_t));

    const std::string backup = std::string(out_path) + ".bak";
    struct stat saved{};
    if (stat(backup.c_str(), &saved) == 0) {
        struct stat current{};
        if (stat(out_path, &current) != 0) {
            if (rename(backup.c_str(), out_path) != 0) return false;
        } else if (unlink(backup.c_str()) != 0) {
            return false;
        }
    }
    std::string tmp(out_path);
    tmp += ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = !Cancelled() && fwrite(&h, sizeof(h), 1, f) == 1;
    for (const auto& iv : ivs) {
        epdfont_interval_t r{iv.first, iv.last, iv.gidx};
        ok = ok && !Cancelled() && fwrite(&r, sizeof(r), 1, f) == 1;
    }
    for (const auto& g : recs) {
        epdfont_glyph_t r{g.w, g.h, g.adv, g.ox, g.oy, g.bmp_len, g.bmp_off, 0};
        ok = ok && fwrite(&r, sizeof(r), 1, f) == 1;
    }
    if (blob_size > 0) {
        for (size_t offset = 0; ok && offset < blob_size;) {
            const size_t count = std::min<size_t>(16 * 1024, blob_size - offset);
            ok = !Cancelled() && fwrite(blob + offset, 1, count, f) == count;
            offset += count;
            vTaskDelay(1);
        }
    }
    if (fflush(f) != 0) {
        ok = false;
    }
    if (fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok || Cancelled()) {
        unlink(tmp.c_str());
        return false;
    }
    // 替换前保留旧字库；提交失败可回滚，取消只在提交前生效。
    struct stat old{};
    const bool exists = stat(out_path, &old) == 0;
    if (exists && rename(out_path, backup.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), out_path) != 0) {
        if (exists && rename(backup.c_str(), out_path) != 0) {
            ESP_LOGE(TAG, "restore failed: %s", backup.c_str());
        }
        unlink(tmp.c_str());
        return false;
    }
    if (exists) unlink(backup.c_str());
    return true;
}

bool RenderOneSize(FT_Face face, const std::vector<uint32_t>& cps, int size_px,
                   uint32_t* done, uint32_t total, const char* out_path, char* fail_msg,
                   size_t fail_msg_len) {
    if (FT_Set_Pixel_Sizes(face, 0, size_px) != 0) {
        std::snprintf(fail_msg, fail_msg_len, Lang::Strings::TTF_SIZE_FAILED_FMT, size_px);
        return false;
    }
    const int asc = static_cast<int>(face->size->metrics.ascender >> 6);
    const int desc = static_cast<int>(-(face->size->metrics.descender >> 6));
    const int line_height = std::max(asc + desc, 1);
    const int base_line = std::max(desc, 0);

    std::vector<GlyphRec> recs;
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> blob(nullptr, heap_caps_free);
    size_t blob_size = 0;
    size_t blob_capacity = 0;
    try {
        recs.reserve(cps.size());
    } catch (...) {
        std::snprintf(fail_msg, fail_msg_len, "%s", Lang::Strings::BOOK_OUT_OF_MEMORY);
        return false;
    }

    for (size_t ci = 0; ci < cps.size(); ++ci) {
        if (Cancelled()) {
            std::snprintf(fail_msg, fail_msg_len, Lang::Strings::TTF_CANCELLED);
            return false;
        }
        if (s_io_failed.load(std::memory_order_acquire)) {
            std::snprintf(fail_msg, fail_msg_len, "%s", Lang::Strings::TTF_SD_IO_FAILED);
            return false;
        }
        const uint32_t cp = cps[ci];
        FT_UInt gid = FT_Get_Char_Index(face, cp);
        int tab_mul = 1;
        if (gid == 0 && cp != 0) {
            if (cp == 0x09) {  // 无 Tab 字形：复用空格、adv×2（与 PC 工具一致）
                gid = FT_Get_Char_Index(face, 0x20);
                tab_mul = 2;
                if (gid == 0) {
                    ++(*done);
                    continue;
                }
            } else {
                ++(*done);
                continue;
            }
        }
        if (FT_Load_Glyph(face, gid, kLoadFlags) != 0) {
            ++(*done);
            continue;
        }
        FT_GlyphSlot slot = face->glyph;
        const FT_Bitmap& bm = slot->bitmap;
        const int w = static_cast<int>(bm.width);
        const int h = static_cast<int>(bm.rows);
        int adv = static_cast<int>((slot->advance.x + 32) >> 6) * tab_mul;
        const int ox = slot->bitmap_left;
        const int oy = slot->bitmap_top - h;

        if (w > 255 || h > 255) {
            ESP_LOGW(TAG, "skip oversize U+%04X %dx%d", static_cast<unsigned>(cp), w, h);
            ++(*done);
            continue;
        }
        GlyphRec rec{};
        rec.cp = cp;
        rec.w = static_cast<uint8_t>(w);
        rec.h = static_cast<uint8_t>(h);
        rec.adv = static_cast<uint16_t>(std::max(adv, 1));
        rec.ox = static_cast<int16_t>(ox);
        rec.oy = static_cast<int16_t>(oy);
        const size_t len = (w == 0 || h == 0) ? 1 : (w * h + 3) / 4;
        const size_t required = blob_size + len;
        if (required > blob_capacity) {
            // 固定步长增长，避免 vector 翻倍扩容的峰值；失败时保留原缓冲并正常退出。
            constexpr size_t chunk = 64 * 1024;
            const size_t capacity = ((required + chunk - 1) / chunk) * chunk;
            auto* next = static_cast<uint8_t*>(heap_caps_realloc(
                blob.get(), capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (next == nullptr) {
                std::snprintf(fail_msg, fail_msg_len, Lang::Strings::TTF_GLYPH_MEMORY);
                return false;
            }
            blob.release();
            blob.reset(next);
            blob_capacity = capacity;
        }
        rec.bmp_len = static_cast<uint16_t>(len);
        rec.bmp_off = static_cast<uint32_t>(blob_size);
        std::memset(blob.get() + blob_size, 0, len);
        if (w != 0 && h != 0) {
            Quantize2bpp(bm.buffer, bm.pitch, w, h, blob.get() + blob_size);
        }
        blob_size = required;
        try {
            recs.push_back(rec);
        } catch (...) {
            std::snprintf(fail_msg, fail_msg_len, "%s", Lang::Strings::BOOK_OUT_OF_MEMORY);
            return false;
        }
        ++(*done);
        if (((*done) & (kYieldEvery - 1)) == 0) {
            if (total > 0) {
                s_percent.store(static_cast<int>((*done) * 100 / total), std::memory_order_release);
            }
            vTaskDelay(1);
        }
    }
    if (total > 0) {
        s_percent.store(static_cast<int>((*done) * 100 / total), std::memory_order_release);
    }

    if (recs.empty()) {
        std::snprintf(fail_msg, fail_msg_len, Lang::Strings::TTF_NO_GLYPHS_FMT, size_px);
        return false;
    }
    ESP_LOGI(TAG, "writing %s glyphs=%u bytes=%u", out_path,
             static_cast<unsigned>(recs.size()), static_cast<unsigned>(blob_size));
    if (!WriteEf(out_path, line_height, base_line, recs, blob.get(), blob_size)) {
        std::snprintf(fail_msg, fail_msg_len, "%s", Lang::Strings::TTF_WRITE_FAILED);
        return false;
    }
    // 自校验：复用设备端解析器确认格式可读
    epdfont_t* chk = epdfont_open(out_path);
    if (chk == nullptr || epdfont_glyph_count(chk) != recs.size()) {
        std::snprintf(fail_msg, fail_msg_len, "%s", Lang::Strings::TTF_VERIFY_FAILED);
        if (chk != nullptr) {
            epdfont_close(chk);
        }
        return false;
    }
    epdfont_close(chk);
    // 本档结束后立刻释放大块，再进下一档，压峰值内存
    return true;
}

/** 转换主流程：正常返回，确保 fam/cps/face/lib/job 全部释放（析构 + release）。 */
void RunWorker(Job* job) {
    // 统一出口：保证 face / lib / job 只释放一次。
    FT_Library lib = nullptr;
    FT_Face face = nullptr;
    auto release = [&]() {
        if (face != nullptr) {
            FT_Done_Face(face);
            face = nullptr;
        }
        if (lib != nullptr) {
            FT_Done_FreeType(lib);
            lib = nullptr;
        }
        delete job;
        job = nullptr;
    };

    sd_access::Transaction transaction;
    if (!transaction || Cancelled()) {
        Fail(Lang::Strings::TTF_CANCELLED_OR_NO_SD);
        release();
        return;
    }
    const std::string fam = FontStem(job->ttf_path.c_str());
    std::vector<uint32_t> cps;
    if (!LoadCharset(job->template_path.c_str(), &cps)) {
        Fail(job->template_path.empty() ? Lang::Strings::BOOK_OUT_OF_MEMORY
                                       : Lang::Strings::TTF_TEMPLATE_INVALID);
        release();
        return;
    }
    ESP_LOGI(TAG, "charset %u cps from %s", static_cast<unsigned>(cps.size()),
             job->template_path.empty() ? "built-in reading_zh" : job->template_path.c_str());

    if (Cancelled()) {
        Fail(Lang::Strings::TTF_CANCELLED);
        release();
        return;
    }
    if (FT_Init_FreeType(&lib) != 0) {
        lib = nullptr;  // 半初始化 library 不可 FT_Done_FreeType
        Fail(Lang::Strings::TTF_ENGINE_INIT_FAILED);
        release();
        return;
    }
    if (!TtfOpenFace(lib, job->ttf_path.c_str(), &face, &s_io_failed)) {
        Fail(Lang::Strings::TTF_SOURCE_OPEN_FAILED);
        release();
        return;
    }

    const uint32_t total = static_cast<uint32_t>(cps.size()) * static_cast<uint32_t>(job->size_count);
    uint32_t done = 0;
    char fail_msg[64] = "";
    bool failed = false;

    for (int si = 0; si < job->size_count; ++si) {
        const int size_px = job->sizes[si];
        char out_path[SD_FILE_PATH_CAPACITY];
        std::snprintf(out_path, sizeof(out_path), "%s/%s_%d_2.ef", job->out_dir.c_str(),
                      fam.c_str(), size_px);
        if (!RenderOneSize(face, cps, size_px, &done, total, out_path, fail_msg,
                           sizeof(fail_msg))) {
            failed = true;
            break;
        }
    }

    release();
    if (failed) {
        Fail(fail_msg);
    } else {
        FinishOk();
    }
}

void Worker(void* arg) {
    Job* job = static_cast<Job*>(arg);
    if (job == nullptr) {
        Fail(Lang::Strings::TTF_JOB_INVALID);
        ExitWorker();
    }
    // 转换流程正常返回，栈上 std::string/std::vector 析构在此完成，
    // 避免 vTaskDeleteWithCaps 不展开 C++ 栈导致 heap 缓冲泄漏。
    RunWorker(job);
    s_worker_active.store(false, std::memory_order_release);
    // 任务栈走 WithCaps 回收（见 xTaskCreatePinnedToCoreWithCaps）。
    ExitWorker();
}

}  // namespace

bool Start(const char* ttf_path, const char* template_ef_path, const char* out_dir,
           const uint16_t* sizes_px, int size_count) {
    if (ttf_path == nullptr || ttf_path[0] == '\0' || out_dir == nullptr || out_dir[0] == '\0' ||
        sizes_px == nullptr || size_count <= 0 || size_count > 3) {
        return false;
    }
    for (int i = 0; i < size_count; ++i) {
        if (sizes_px[i] == 0 || sizes_px[i] > 128) {
            return false;
        }
    }

    bool inactive = false;
    if (!s_worker_active.compare_exchange_strong(inactive, true, std::memory_order_acq_rel)) {
        return false;
    }
    s_cancel.store(false, std::memory_order_release);
    s_state.store(kStateRunning, std::memory_order_release);

    Job* job = new (std::nothrow) Job();
    if (job == nullptr) {
        Fail(Lang::Strings::BOOK_OUT_OF_MEMORY);
        s_worker_active.store(false, std::memory_order_release);
        return false;
    }
    try {
        job->ttf_path = ttf_path;
        job->template_path = template_ef_path != nullptr ? template_ef_path : "";
        job->out_dir = out_dir;
        const std::string stem = FontStem(ttf_path);
        for (int i = 0; i < size_count; ++i) {
            const std::string name = stem + "_" + std::to_string(sizes_px[i]) + "_2.ef";
            if (!SdIsValidFileName(name.c_str()) ||
                job->out_dir.size() + 1 + name.size() >= SD_FILE_PATH_CAPACITY) {
                delete job;
                Fail(Lang::Strings::TTF_OUTPUT_NAME_TOO_LONG);
                s_worker_active.store(false, std::memory_order_release);
                return false;
            }
        }
    } catch (...) {
        delete job;
        Fail(Lang::Strings::BOOK_OUT_OF_MEMORY);
        s_worker_active.store(false, std::memory_order_release);
        return false;
    }
    for (int i = 0; i < size_count; ++i) {
        job->sizes[i] = sizes_px[i];
    }
    job->size_count = size_count;

    s_error[0] = '\0';
    s_percent.store(0, std::memory_order_release);
    s_io_failed.store(false, std::memory_order_release);

    // 参数顺序：…, core, caps（与 book_open / asst_cam 一致）
    if (xTaskCreatePinnedToCoreWithCaps(Worker, "ttf_conv", kWorkerStack, job, kWorkerPrio, nullptr,
                                        kWorkerCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
        pdPASS) {
        delete job;
        ESP_LOGE(TAG,
                 "task create failed: stack=%u SPIRAM; int free=%u largest=%u; "
                 "psram free=%u largest=%u",
                 static_cast<unsigned>(kWorkerStack),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        Fail(Lang::Strings::TTF_START_FAILED);
        s_worker_active.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

int State() { return s_state.load(std::memory_order_acquire); }

int Percent() { return s_percent.load(std::memory_order_acquire); }

const char* Error() { return s_error; }

bool Busy() { return s_worker_active.load(std::memory_order_acquire); }

void Cancel() { s_cancel.store(true, std::memory_order_release); }

void FamilyFromPath(const char* ttf_path, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    const std::string fam = FontStem(ttf_path);
    std::snprintf(out, out_len, "%s", fam.c_str());
}

void FormatOutputFileList(const char* ttf_path, const uint16_t* sizes_px, int size_count,
                          char* buf, size_t buf_len) {
    if (buf == nullptr || buf_len == 0) {
        return;
    }
    const uint16_t defaults[1] = {25};
    if (sizes_px == nullptr || size_count <= 0) {
        sizes_px = defaults;
        size_count = 1;
    }
    if (size_count > 3) {
        size_count = 3;
    }
    const std::string fam = FontStem(ttf_path);
    size_t off = 0;
    for (int i = 0; i < size_count && off + 1 < buf_len; ++i) {
        if (i > 0) {
            off += static_cast<size_t>(std::snprintf(buf + off, buf_len - off, "\n"));
            if (off >= buf_len) {
                break;
            }
        }
        off += static_cast<size_t>(
            std::snprintf(buf + off, buf_len - off, "%s_%u_2.ef", fam.c_str(),
                          static_cast<unsigned>(sizes_px[i])));
    }
    if (off >= buf_len) {
        buf[buf_len - 1] = '\0';
    }
}

}  // namespace ttf_convert

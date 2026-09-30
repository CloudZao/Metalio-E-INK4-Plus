#include "ttf_convert.h"

#include "epdfont.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

void Fail(const char* msg) {
    std::snprintf(s_error, sizeof(s_error), "%s", msg ? msg : "unknown");
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

unsigned long FtStreamIo(FT_Stream stream, unsigned long offset, unsigned char* buffer,
                         unsigned long count) {
    FILE* f = static_cast<FILE*>(stream->descriptor.pointer);
    if (f == nullptr || fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
        s_io_failed.store(true, std::memory_order_release);
        return 0;
    }
    if (count == 0) {
        return 0;
    }
    const size_t got = fread(buffer, 1, count, f);
    if (got < count) {
        s_io_failed.store(true, std::memory_order_release);
    }
    return static_cast<unsigned long>(got);
}

void FtStreamClose(FT_Stream stream) {
    if (stream == nullptr) {
        return;
    }
    if (stream->descriptor.pointer != nullptr) {
        fclose(static_cast<FILE*>(stream->descriptor.pointer));
        stream->descriptor.pointer = nullptr;
    }
    free(stream);
}

bool OpenFace(FT_Library lib, const char* ttf_path, FT_Face* face_out) {
    FT_Stream stream = static_cast<FT_Stream>(calloc(1, sizeof(FT_StreamRec)));
    if (stream == nullptr) {
        return false;
    }
    FILE* f = fopen(ttf_path, "rb");
    if (f == nullptr) {
        free(stream);
        return false;
    }
    // 大小优先 stat：FATFS 上部分文件 fseek(SEEK_END) 会 EIO（见提交 2e68283）
    unsigned long fsize = 0;
    struct stat sb{};
    if (stat(ttf_path, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size > 0) {
        fsize = static_cast<unsigned long>(sb.st_size);
    } else if (fseek(f, 0, SEEK_END) == 0) {
        const long sz = ftell(f);
        if (sz > 0) {
            fsize = static_cast<unsigned long>(sz);
        }
    }
    if (fsize == 0) {
        fclose(f);
        free(stream);
        return false;
    }
    stream->size = fsize;
    stream->descriptor.pointer = f;
    stream->read = FtStreamIo;
    stream->close = FtStreamClose;

    FT_Open_Args args{};
    args.flags = FT_OPEN_STREAM;
    args.stream = stream;
    FT_Error err = FT_Open_Face(lib, &args, 0, face_out);
    if (err != 0) {
        // FreeType 失败路径已 FT_Stream_Free → stream->close（勿重复释放）
        return false;
    }
    return true;
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

std::string SanitizeFamily(const char* ttf_path) {
    if (ttf_path == nullptr || ttf_path[0] == '\0') {
        return "font";
    }
    const char* base = std::strrchr(ttf_path, '/');
    base = (base == nullptr) ? ttf_path : base + 1;
    std::string fam;
    for (const char* p = base; *p != '\0' && *p != '.' && fam.size() < 40; ++p) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-') {
            fam.push_back(c);
        }
    }
    if (fam.empty()) {
        fam = "font";
    }
    return fam;
}

bool WriteEf(const char* out_path, int line_height, int base_line,
             const std::vector<GlyphRec>& recs, const std::vector<uint8_t>& blob) {
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

    std::string tmp(out_path);
    tmp += ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
    for (const auto& iv : ivs) {
        epdfont_interval_t r{iv.first, iv.last, iv.gidx};
        ok = ok && fwrite(&r, sizeof(r), 1, f) == 1;
    }
    for (const auto& g : recs) {
        epdfont_glyph_t r{g.w, g.h, g.adv, g.ox, g.oy, g.bmp_len, g.bmp_off, 0};
        ok = ok && fwrite(&r, sizeof(r), 1, f) == 1;
    }
    if (!blob.empty()) {
        ok = ok && fwrite(blob.data(), 1, blob.size(), f) == blob.size();
    }
    if (fflush(f) != 0) {
        ok = false;
    }
    fsync(fileno(f));
    fclose(f);
    if (!ok) {
        unlink(tmp.c_str());
        return false;
    }
    // FATFS 的 f_rename 不覆盖已存在目标（FR_EXIST）：先删目标，实现「同名覆盖」语义。
    unlink(out_path);
    if (rename(tmp.c_str(), out_path) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool RenderOneSize(FT_Face face, const std::vector<uint32_t>& cps, int size_px,
                   uint32_t* done, uint32_t total, const char* out_path, char* fail_msg,
                   size_t fail_msg_len) {
    if (FT_Set_Pixel_Sizes(face, 0, size_px) != 0) {
        std::snprintf(fail_msg, fail_msg_len, "set size %d", size_px);
        return false;
    }
    const int asc = static_cast<int>(face->size->metrics.ascender >> 6);
    const int desc = static_cast<int>(-(face->size->metrics.descender >> 6));
    const int line_height = std::max(asc + desc, 1);
    const int base_line = std::max(desc, 0);

    std::vector<GlyphRec> recs;
    std::vector<uint8_t> blob;
    try {
        recs.reserve(cps.size());
        // 保守预留：平均字形远小于 size²；失败则按需增长，避免一次吃满 PSRAM
        const size_t soft_cap =
            std::min(cps.size() * (static_cast<size_t>(size_px) * size_px / 8 + 4),
                     static_cast<size_t>(2 * 1024 * 1024));
        blob.reserve(soft_cap);
    } catch (...) {
        std::snprintf(fail_msg, fail_msg_len, "oom reserve %d", size_px);
        return false;
    }

    for (size_t ci = 0; ci < cps.size(); ++ci) {
        if (s_io_failed.load(std::memory_order_acquire)) {
            std::snprintf(fail_msg, fail_msg_len, "sd io error");
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
        try {
            if (w == 0 || h == 0) {
                rec.bmp_len = 1;  // 与 PC 工具一致：空字形占 1 字节 0x00
                rec.bmp_off = static_cast<uint32_t>(blob.size());
                blob.push_back(0);
            } else {
                const uint16_t len = static_cast<uint16_t>((w * h + 3) / 4);
                rec.bmp_len = len;
                rec.bmp_off = static_cast<uint32_t>(blob.size());
                const size_t old = blob.size();
                blob.resize(old + len, 0);
                Quantize2bpp(bm.buffer, bm.pitch, w, h, blob.data() + rec.bmp_off);
            }
            recs.push_back(rec);
        } catch (...) {
            std::snprintf(fail_msg, fail_msg_len, "oom glyph %d", size_px);
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
        std::snprintf(fail_msg, fail_msg_len, "no glyphs size %d", size_px);
        return false;
    }
    ESP_LOGI(TAG, "writing %s glyphs=%u bytes=%u", out_path,
             static_cast<unsigned>(recs.size()), static_cast<unsigned>(blob.size()));
    if (!WriteEf(out_path, line_height, base_line, recs, blob)) {
        std::snprintf(fail_msg, fail_msg_len, "write %.40s", out_path);
        return false;
    }
    // 自校验：复用设备端解析器确认格式可读
    epdfont_t* chk = epdfont_open(out_path);
    if (chk == nullptr || epdfont_glyph_count(chk) != recs.size()) {
        std::snprintf(fail_msg, fail_msg_len, "verify %u_2", static_cast<unsigned>(size_px));
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

    const std::string fam = SanitizeFamily(job->ttf_path.c_str());
    std::vector<uint32_t> cps;
    if (!LoadCharset(job->template_path.c_str(), &cps)) {
        Fail("bad template ef");
        release();
        return;
    }
    ESP_LOGI(TAG, "charset %u cps from %s", static_cast<unsigned>(cps.size()),
             job->template_path.c_str());

    if (FT_Init_FreeType(&lib) != 0) {
        lib = nullptr;  // 半初始化 library 不可 FT_Done_FreeType
        Fail("FT_Init_FreeType");
        release();
        return;
    }
    if (!OpenFace(lib, job->ttf_path.c_str(), &face)) {
        Fail("open ttf failed");
        release();
        return;
    }

    const uint32_t total = static_cast<uint32_t>(cps.size()) * static_cast<uint32_t>(job->size_count);
    uint32_t done = 0;
    char fail_msg[64] = "";
    bool failed = false;

    for (int si = 0; si < job->size_count; ++si) {
        const int size_px = job->sizes[si];
        char out_path[192];
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
        Fail("null job");
        ExitWorker();
    }
    // 转换流程正常返回，栈上 std::string/std::vector 析构在此完成，
    // 避免 vTaskDeleteWithCaps 不展开 C++ 栈导致 heap 缓冲泄漏。
    RunWorker(job);
    // 任务栈走 WithCaps 回收（见 xTaskCreatePinnedToCoreWithCaps）。
    ExitWorker();
}

}  // namespace

bool Start(const char* ttf_path, const char* template_ef_path, const char* out_dir,
           const uint16_t* sizes_px, int size_count) {
    if (ttf_path == nullptr || ttf_path[0] == '\0' || template_ef_path == nullptr ||
        template_ef_path[0] == '\0' || out_dir == nullptr || out_dir[0] == '\0' ||
        sizes_px == nullptr || size_count <= 0 || size_count > 3) {
        return false;
    }
    for (int i = 0; i < size_count; ++i) {
        if (sizes_px[i] == 0 || sizes_px[i] > 128) {
            return false;
        }
    }

    // Idle/Done/Failed → Running；已在 Running 则拒绝（防双开）。
    int expected = s_state.load(std::memory_order_acquire);
    do {
        if (expected == kStateRunning) {
            return false;
        }
    } while (!s_state.compare_exchange_weak(expected, kStateRunning, std::memory_order_acq_rel,
                                            std::memory_order_acquire));

    Job* job = new (std::nothrow) Job();
    if (job == nullptr) {
        Fail("oom job");
        return false;
    }
    try {
        job->ttf_path = ttf_path;
        job->template_path = template_ef_path;
        job->out_dir = out_dir;
    } catch (...) {
        delete job;
        Fail("oom paths");
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
        Fail("task create");
        return false;
    }
    return true;
}

int State() { return s_state.load(std::memory_order_acquire); }

int Percent() { return s_percent.load(std::memory_order_acquire); }

const char* Error() { return s_error; }

bool Busy() { return State() == kStateRunning; }

void FamilyFromPath(const char* ttf_path, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    const std::string fam = SanitizeFamily(ttf_path);
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
    const std::string fam = SanitizeFamily(ttf_path);
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

#include "progressive_jpeg_priv.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>

namespace reader::jpeg {
namespace {
#include "stb_jpeg_core.h"

struct Progressive {
    stbi__jpeg decoder; // 上游熵解码与 IDCT 状态
    stbi__context input; // 有界压缩输入读取状态
    DecodeContext* context; // 当前请求
    uint32_t position; // 输入游标
    uint32_t base[4]; // 各分量系数在 SD 工作文件中的偏移
    uint8_t levels[4][64]; // 各频谱系数已完成的精度，255 表示尚无扫描
    short block[64]; // 当前系数块
    uint8_t tile[64]; // 当前灰度块
    uint32_t scans; // 已完成扫描数
    uint64_t ac_spectral_mask; // 当前扫描频谱对应的自然顺序显著性位
    uint32_t block_checks; // 每 32 个系数块检查取消，输入和换页仍独立检查
    alignas(8) uint8_t pages[3 * 8192 + 32768]; // 三个显著性位图页及一个亮度系数页，均位于 PSRAM
    uint32_t page[4]; // 当前缓存页的字节偏移
    uint32_t work_size[4]; // 各存储区已初始化的末尾
    bool dirty[4]; // 当前页有未落盘写入
    uint32_t raw_base; // 紧排亮度系数区起点
    uint16_t target_w, target_h; // 最终目标尺寸
    uint16_t selected_w, selected_h; // 参与输出的亮度块网格
    bool banded; // 当前请求使用扫描游标与 MCU 行条带
    bool output_begun; // 输出布局已建立
    unsigned band_first, band_end; // 当前条带的 MCU 行范围
    uint8_t* band_data; // 工作缓存中位于扫描快照之后的条带
    size_t band_capacity; // 条带可用字节数
    uint32_t band_base[4]; // 各区域在条带中的字节偏移
    uint32_t band_start[4]; // 各区域对应的全局工作偏移
    uint32_t band_length[4]; // 各区域当前条带长度
    stbi__uint16 final_quantization[4][64]; // 完整标记流解析后的反量化表
    int final_transform; // 完整标记流解析后的颜色变换
};

#include "progressive_entropy.h"

uint8_t* PageData(Progressive& state, unsigned slot) {
    return state.pages + (slot == 3 ? 3 * 8192 : slot * 8192);
}

size_t PageSize(const Progressive&, unsigned slot) {
    return slot == 3 ? 32768 : 8192;
}

bool StorePage(Progressive& state, unsigned slot, uint32_t page) {
    return state.context->WorkWrite(page, PageData(state, slot), PageSize(state, slot));
}

bool LoadPage(Progressive& state, unsigned slot, uint32_t page) {
    return state.context->WorkRead(page, PageData(state, slot), PageSize(state, slot));
}

bool Flush(Progressive& state, unsigned component) {
    if (!state.dirty[component]) return true;
    const uint32_t page = state.page[component];
    const size_t bytes = PageSize(state, component);
    if (!StorePage(state, component, page)) return false;
    state.work_size[component] = std::max<uint32_t>(state.work_size[component], page + bytes);
    state.dirty[component] = false;
    return true;
}

short* Coefficients(Progressive& state, unsigned component, uint32_t offset) {
    if (state.banded) {
        const size_t bytes = component == 3 ? sizeof(state.block) : sizeof(uint64_t);
        if (offset < state.band_start[component]) { state.context->Fail(ImageDecodeError::kInvalidData); return nullptr; }
        const uint32_t local = offset - state.band_start[component];
        if (local > state.band_length[component] || bytes > state.band_length[component] - local) {
            state.context->Fail(ImageDecodeError::kInvalidData);
            return nullptr;
        }
        return reinterpret_cast<short*>(state.band_data + state.band_base[component] + local);
    }
    const uint32_t size = PageSize(state, component);
    const uint32_t page = offset & ~(size - 1);
    if (page != state.page[component]) {
        if (!Flush(state, component)) return nullptr;
        std::memset(PageData(state, component), 0, size);
        // 非交错扫描可能跳过尚未写入的页，显式补零避免依赖文件扩展内容。
        while (state.work_size[component] < page) {
            if (!StorePage(state, component, state.work_size[component])) return nullptr;
            state.work_size[component] += size;
        }
        if (page < state.work_size[component] && !LoadPage(state, component, page)) return nullptr;
        state.page[component] = page;
    }
    return reinterpret_cast<short*>(PageData(state, component) + (offset - page));
}

int ReadProgressive(void* user, char* bytes, int requested) {
    auto& state = *static_cast<Progressive*>(user);
    if (requested <= 0 || state.context->Cancelled()) return 0;
    const size_t count = std::min<size_t>(requested, state.context->InputSize() - state.position);
    if (count != 0 && !state.context->Read(state.position, bytes, count)) return 0;
    state.position += count;
    return static_cast<int>(count);
}

void SkipProgressive(void* user, int count) {
    auto& state = *static_cast<Progressive*>(user);
    const int64_t next = int64_t(state.position) + count;
    if (next < 0 || next > state.context->InputSize()) {
        state.context->Fail(ImageDecodeError::kInvalidData);
        return;
    }
    state.position = static_cast<uint32_t>(next);
}

int EofProgressive(void* user) {
    const auto& state = *static_cast<Progressive*>(user);
    return state.context->error != ImageDecodeError::kNone || state.position >= state.context->InputSize();
}

bool Frame(Progressive& state) {
    auto& decoder = state.decoder;
    auto* input = decoder.s;
    const unsigned length = stbi__get16be(input);
    const unsigned precision = stbi__get8(input);
    input->img_y = stbi__get16be(input);
    input->img_x = stbi__get16be(input);
    input->img_n = stbi__get8(input);
    if (precision != 8 || (input->img_n != 1 && input->img_n != 3)) {
        state.context->Fail(ImageDecodeError::kUnsupported);
        return false;
    }
    if (input->img_x == 0 || input->img_y == 0 || length != unsigned(8 + 3 * input->img_n)) return false;
    decoder.img_h_max = decoder.img_v_max = 1;
    for (int n = 0; n < input->img_n; ++n) {
        auto& component = decoder.img_comp[n];
        component.id = stbi__get8(input);
        const int sampling = stbi__get8(input);
        component.h = sampling >> 4;
        component.v = sampling & 15;
        component.tq = stbi__get8(input);
        if (component.h < 1 || component.h > 4 || component.v < 1 || component.v > 4 || component.tq > 3) return false;
        for (int prior = 0; prior < n; ++prior)
            if (decoder.img_comp[prior].id == component.id) return false;
        decoder.img_h_max = std::max(decoder.img_h_max, component.h);
        decoder.img_v_max = std::max(decoder.img_v_max, component.v);
    }
    if (input->img_n == 1) {
        decoder.img_comp[0].h = decoder.img_comp[0].v = 1;
        decoder.img_h_max = decoder.img_v_max = 1;
    }
    if (decoder.img_comp[0].h != decoder.img_h_max || decoder.img_comp[0].v != decoder.img_v_max ||
        (input->img_n == 3 && decoder.img_comp[0].id == 'R' &&
         decoder.img_comp[1].id == 'G' && decoder.img_comp[2].id == 'B')) {
        state.context->Fail(ImageDecodeError::kUnsupported);
        return false;
    }
    decoder.img_mcu_w = decoder.img_h_max * 8;
    decoder.img_mcu_h = decoder.img_v_max * 8;
    decoder.img_mcu_x = (input->img_x + decoder.img_mcu_w - 1) / decoder.img_mcu_w;
    decoder.img_mcu_y = (input->img_y + decoder.img_mcu_h - 1) / decoder.img_mcu_h;
    const double ratio = state.context->max_width > 0 && state.context->max_height > 0
        ? std::min({1.0, double(state.context->max_width) / input->img_x,
                    double(state.context->max_height) / input->img_y}) : 1.0;
    state.target_w = std::max(1, int(input->img_x * ratio));
    state.target_h = std::max(1, int(input->img_y * ratio));
    state.selected_w = (input->img_x + 7) / 8;
    state.selected_h = (input->img_y + 7) / 8;
    uint64_t position = 0;
    for (int n = 0; n < input->img_n; ++n) {
        auto& component = decoder.img_comp[n];
        if (decoder.img_h_max % component.h != 0 || decoder.img_v_max % component.v != 0) return false;
        component.x = (input->img_x * component.h + decoder.img_h_max - 1) / decoder.img_h_max;
        component.y = (input->img_y * component.v + decoder.img_v_max - 1) / decoder.img_v_max;
        component.coeff_w = decoder.img_mcu_x * component.h;
        component.coeff_h = decoder.img_mcu_y * component.v;
        state.base[n] = static_cast<uint32_t>(position);
        state.work_size[n] = state.base[n];
        if (n != 0)
            position += uint64_t(component.coeff_w) * component.coeff_h * sizeof(uint64_t);
        position = (position + 8191) & ~uint64_t(8191);
        if (position > INT32_MAX) { state.context->Fail(ImageDecodeError::kNoMemory); return false; }
    }
    state.context->output_bytes = size_t(state.target_w) * state.target_h;
    position = (position + 32767) & ~uint64_t(32767);
    state.raw_base = static_cast<uint32_t>(position);
    state.work_size[3] = state.raw_base;
    position += uint64_t(state.selected_w) * state.selected_h * sizeof(state.block);
    position = (position + 32767) & ~uint64_t(32767);
    if (position > INT32_MAX) { state.context->Fail(ImageDecodeError::kNoMemory); return false; }
    state.context->CacheWork(static_cast<uint32_t>(position));
    return true;
}

bool ValidateScan(Progressive& state) {
    auto& decoder = state.decoder;
    if ((decoder.spec_start == 0 && decoder.spec_end != 0) ||
        (decoder.spec_start != 0 && decoder.scan_n != 1) ||
        (decoder.succ_high != 0 && decoder.succ_low + 1 != decoder.succ_high)) return false;
    for (int index = 0; index < decoder.scan_n; ++index) {
        const int component = decoder.order[index];
        for (int previous = 0; previous < index; ++previous)
            if (decoder.order[previous] == component) return false;
        const auto& plane = decoder.img_comp[component];
        if (decoder.spec_start == 0 && decoder.succ_high == 0 && decoder.huff_dc[plane.hd].symbols == 0) return false;
        if (decoder.spec_start != 0 && decoder.huff_ac[plane.ha].symbols == 0) return false;
        if (decoder.spec_start != 0 && state.levels[component][0] == 255) return false;
        for (int coefficient = decoder.spec_start; coefficient <= decoder.spec_end; ++coefficient) {
            const int previous = state.levels[component][coefficient];
            if ((decoder.succ_high == 0 && previous != 255) ||
                (decoder.succ_high != 0 && previous != decoder.succ_high)) return false;
        }
    }
    return true;
}

uint32_t BlockOffset(const Progressive& state, unsigned component, unsigned x, unsigned y) {
    const auto& plane = state.decoder.img_comp[component];
    return state.base[component] + (y * plane.coeff_w + x) * sizeof(uint64_t);
}

unsigned SelectedBefore(unsigned block, unsigned selected, unsigned total) {
    return std::min(selected, unsigned((block * selected + total - 1) / total));
}

bool SelectedOffset(const Progressive& state, unsigned x, unsigned y, uint32_t& offset) {
    const auto& plane = state.decoder.img_comp[0];
    const unsigned columns = (state.input.img_x + 7) / 8;
    const unsigned rows = (state.input.img_y + 7) / 8;
    if (x >= columns || y >= rows) return false;
    const unsigned sx = x, sy = y;
    // 保持 MCU 内顺序，避免交错 DC 在相邻亮度行之间逐块换页。
    if (plane.h == 1 && plane.v == 1) {
        offset = state.raw_base + (sy * state.selected_w + sx) * sizeof(state.block);
        return true;
    }
    const unsigned x0 = SelectedBefore(x / plane.h * plane.h, state.selected_w, columns);
    const unsigned x1 = SelectedBefore((x / plane.h + 1) * plane.h, state.selected_w, columns);
    const unsigned y0 = SelectedBefore(y / plane.v * plane.v, state.selected_h, rows);
    const unsigned y1 = SelectedBefore((y / plane.v + 1) * plane.v, state.selected_h, rows);
    const uint32_t rank = y0 * state.selected_w + (y1 - y0) * x0 + (sy - y0) * (x1 - x0) + sx - x0;
    offset = state.raw_base + rank * sizeof(state.block);
    return true;
}

bool Block(Progressive& state, int component, unsigned x, unsigned y) {
    auto& decoder = state.decoder;
    auto& plane = decoder.img_comp[component];
    if ((state.block_checks++ & 31) == 0 && state.context->Cancelled()) return false;
    // 首次 AC 的延续 EOBRUN 不访问或修改任何系数，重启检查仍由扫描层执行。
    if (decoder.spec_start != 0 && decoder.succ_high == 0 && decoder.eob_run > 0) {
        --decoder.eob_run;
        return state.context->error == ImageDecodeError::kNone;
    }
    uint32_t position;
    const bool selected = component == 0 && SelectedOffset(state, x, y, position);
    short* coefficients = selected ? Coefficients(state, 3, position) : state.block;
    if (!coefficients) return false;
    uint64_t* significance = nullptr;
    if (!selected) {
        state.block[0] = 0;
        if (decoder.spec_start != 0) {
            significance = reinterpret_cast<uint64_t*>(Coefficients(state, component, BlockOffset(state, component, x, y)));
            if (!significance) return false;
        }
    }
    const uint64_t previous = significance ? *significance : 0;
    const bool ok = decoder.spec_start == 0
        ? StrictDc(&decoder, coefficients, &decoder.huff_dc[plane.hd], component) != 0
        : selected ? StrictAc<false>(&decoder, coefficients, &decoder.huff_ac[plane.ha], decoder.fast_ac[plane.ha]) != 0
        : StrictAc<true>(&decoder, nullptr, &decoder.huff_ac[plane.ha], decoder.fast_ac[plane.ha], significance, state.ac_spectral_mask) != 0;
    if (!ok || state.context->error != ImageDecodeError::kNone) return false;
    if (selected) state.dirty[3] = true;
    else if (significance && *significance != previous) state.dirty[component] = true;
    return true;
}

bool Restart(Progressive& state, bool more, unsigned& sequence) {
    auto& decoder = state.decoder;
    if (--decoder.todo > 0 || !more) return true;
    if (decoder.eob_run != 0) return false;
    if (decoder.code_bits < 24) StrictGrow(&decoder);
    if (decoder.marker != 0xd0 + (sequence++ & 7)) return false;
    stbi__jpeg_reset(&decoder);
    return true;
}

void CompleteScan(Progressive& state) {
    const auto& decoder = state.decoder;
    for (int index = 0; index < decoder.scan_n; ++index)
        for (int coefficient = decoder.spec_start; coefficient <= decoder.spec_end; ++coefficient)
            state.levels[decoder.order[index]][coefficient] = decoder.succ_low;
    ++state.scans;
}

void PrepareSpectralMask(Progressive& state) {
    uint64_t mask = 0;
    const auto& decoder = state.decoder;
    if (decoder.spec_start != 0)
        for (int coefficient = decoder.spec_start; coefficient <= decoder.spec_end; ++coefficient)
            mask |= uint64_t(1) << stbi__jpeg_dezigzag[coefficient];
    state.ac_spectral_mask = mask;
}

bool Scan(Progressive& state) {
    PrepareSpectralMask(state);
    auto& decoder = state.decoder;
    stbi__jpeg_reset(&decoder);
    unsigned sequence = 0;
    if (decoder.scan_n == 1) {
        const unsigned component = decoder.order[0];
        const auto& plane = decoder.img_comp[component];
        const unsigned width = (plane.x + 7) / 8;
        const unsigned height = (plane.y + 7) / 8;
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
                if (!Block(state, component, x, y) ||
                    !Restart(state, x + 1 < width || y + 1 < height, sequence)) return false;
    } else {
        for (int y = 0; y < decoder.img_mcu_y; ++y) {
            for (int x = 0; x < decoder.img_mcu_x; ++x) {
                for (int index = 0; index < decoder.scan_n; ++index) {
                    const unsigned component = decoder.order[index];
                    const auto& plane = decoder.img_comp[component];
                    for (int dy = 0; dy < plane.v; ++dy)
                        for (int dx = 0; dx < plane.h; ++dx)
                            if (!Block(state, component, x * plane.h + dx, y * plane.v + dy)) return false;
                }
                if (!Restart(state, x + 1 < decoder.img_mcu_x || y + 1 < decoder.img_mcu_y, sequence)) return false;
            }
        }
    }
    if (decoder.eob_run != 0) return false;
    CompleteScan(state);
    return state.context->error == ImageDecodeError::kNone;
}

void RenderBlock(Progressive& state, const short* coefficients, const stbi__uint16* quantization) {
    bool uniform = true;
    for (unsigned index = 1; index < 64; ++index) {
        if (coefficients[index] != 0) { uniform = false; break; }
    }
    if (uniform) {
        const short dc = static_cast<short>(coefficients[0] * quantization[0]);
        const int value = ((int(dc) + 4) >> 3) + 128;
        std::memset(state.tile, std::max(0, std::min(255, value)), sizeof(state.tile));
        return;
    }
    for (unsigned index = 0; index < 64; ++index)
        state.block[index] = static_cast<short>(coefficients[index] * quantization[index]);
    stbi__idct_block(state.tile, 8, state.block);
}

bool Finish(Progressive& state) {
    auto& decoder = state.decoder;
    for (int component = 0; component < decoder.s->img_n; ++component) {
        if (state.levels[component][0] == 255) return false;
        for (unsigned value : decoder.dequant[decoder.img_comp[component].tq])
            if (value == 0) return false;
    }
    if (decoder.s->img_n == 3 && decoder.app14_color_transform >= 0 && decoder.app14_color_transform != 1) {
        state.context->Fail(ImageDecodeError::kUnsupported);
        return false;
    }
    const unsigned target_w = state.target_w, target_h = state.target_h;
    if (!state.output_begun) {
        if (!state.context->Begin(target_w, target_h)) return false;
        state.output_begun = true;
    }
    const unsigned source_w = state.input.img_x, source_h = state.input.img_y;
    const auto& plane = decoder.img_comp[0];
    const unsigned columns = (source_w + 7) / 8, rows = (source_h + 7) / 8;
    const unsigned first = state.banded ? state.band_first * plane.v : 0;
    const unsigned end = state.banded ? std::min(rows, state.band_end * plane.v) : rows;
    for (unsigned y = first; y < end; ++y) {
        const unsigned top = (y * 8 * target_h + source_h - 1) / source_h;
        const unsigned bottom = std::min(target_h, ((y + 1) * 8 * target_h + source_h - 1) / source_h);
        if (top == bottom) continue;
        for (unsigned x = 0; x < columns; ++x) {
            if ((state.block_checks++ & 31) == 0 && state.context->Cancelled()) return false;
            const unsigned left = (x * 8 * target_w + source_w - 1) / source_w;
            const unsigned right = std::min(target_w, ((x + 1) * 8 * target_w + source_w - 1) / source_w);
            if (left == right) continue;
            uint32_t offset;
            if (!SelectedOffset(state, x, y, offset)) return false;
            const short* coefficients = Coefficients(state, 3, offset);
            if (!coefficients) return false;
            RenderBlock(state, coefficients, decoder.dequant[plane.tq]);
            // 与原 stb 路径一致：完整 8×8 IDCT 后按原图坐标作最近邻采样。
            uint8_t line[8];
            for (unsigned dy = top; dy < bottom; ++dy) {
                const unsigned sy = dy * source_h / target_h - y * 8;
                for (unsigned dx = left; dx < right; ++dx)
                    line[dx - left] = state.tile[sy * 8 + dx * source_w / target_w - x * 8];
                if (!state.context->Output(dy * target_w + left, line, right - left)) return false;
            }
        }
    }
    return true;
}

constexpr unsigned kScanLimit = 64; // 扫描快照上限，超出时沿用分页策略

struct ScanCursor {
    stbi__jpeg decoder; // 该扫描的表、预测器、位缓冲及重启状态
    uint32_t position; // 下一次输入应消费的压缩字节位置
    uint32_t end_position; // 索引阶段记录的扫描结束标记之后的位置
    unsigned sequence; // 下一重启标记序号
    unsigned end_marker; // 索引阶段记录的后继标记
};

enum class BandResult {
    kDone, // 条带解码完成
    kFallback, // 工作缓存不足，改用分页策略
    kFailed, // 输入、取消或文件操作失败
};

uint32_t InputPosition(const Progressive& state) {
    // EOF 回填的占位字节不属于输入流。
    return state.position - (state.input.read_from_callbacks
        ? static_cast<uint32_t>(state.input.img_buffer_end - state.input.img_buffer) : 0);
}

void Initialize(Progressive& state, DecodeContext& context) {
    std::memset(&state, 0, sizeof(state));
    state.context = &context;
    for (auto& page : state.page) page = UINT32_MAX;
    std::memset(state.levels, 255, sizeof(state.levels));
    stbi_io_callbacks callbacks{ReadProgressive, SkipProgressive, EofProgressive};
    stbi__start_callbacks(&state.input, &callbacks, &state);
    state.decoder.s = &state.input;
    state.decoder.marker = STBI__MARKER_none;
    state.decoder.app14_color_transform = -1;
    state.decoder.progressive = 1;
}


bool SkipEncodedScan(Progressive& state, ScanCursor& cursor) {
    while (!stbi__at_eof(&state.input)) {
        if (stbi__get8(&state.input) != 255) continue;
        unsigned marker;
        do {
            if (stbi__at_eof(&state.input)) return false;
            marker = stbi__get8(&state.input);
        } while (marker == 255);
        if (marker == 0 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        cursor.end_marker = marker;
        cursor.end_position = InputPosition(state);
        state.decoder.marker = marker;
        return true;
    }
    return false;
}

BandResult IndexScans(Progressive& state, unsigned& count) {
    auto& decoder = state.decoder;
    auto& context = *state.context;
    bool framed = false;
    bool ended = false;
    bool ok = stbi__get_marker(&decoder) == 0xd8;
    while (ok && !context.Cancelled()) {
        const unsigned marker = stbi__get_marker(&decoder);
        if (marker == 0xd9) { ended = framed && count != 0; break; }
        if (marker == 0xc2) {
            if (framed) return BandResult::kFailed;
            ok = Frame(state);
            framed = ok;
        } else if (marker == 0xda) {
            if (decoder.s->img_n == 3 && decoder.app14_color_transform >= 0 && decoder.app14_color_transform != 1) {
                context.Fail(ImageDecodeError::kUnsupported);
                return BandResult::kFailed;
            }
            if (!framed || !stbi__process_scan_header(&decoder) || !ValidateScan(state)) return BandResult::kFailed;
            // 扫描数量及单行条带超出缓存时保留既有分页能力。
            if (!context.work_cache || count >= kScanLimit ||
                (count + 1) * sizeof(ScanCursor) >= context.work_cache_size) return BandResult::kFallback;
            auto& cursor = reinterpret_cast<ScanCursor*>(context.work_cache)[count];
            cursor.decoder = decoder;
            stbi__jpeg_reset(&cursor.decoder);
            cursor.position = InputPosition(state);
            cursor.sequence = 0;
            if (!SkipEncodedScan(state, cursor)) return BandResult::kFailed;
            CompleteScan(state);
            ++count;
        } else if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4) {
            context.Fail(ImageDecodeError::kUnsupported);
            return BandResult::kFailed;
        } else {
            ok = stbi__process_marker(&decoder, marker) != 0;
        }
        if (stbi__at_eof(&state.input) && decoder.marker == STBI__MARKER_none) ok = false;
    }
    if (!ok || !ended || context.error != ImageDecodeError::kNone) return BandResult::kFailed;
    for (int component = 0; component < decoder.s->img_n; ++component) {
        if (state.levels[component][0] == 255) return BandResult::kFailed;
        for (unsigned value : decoder.dequant[decoder.img_comp[component].tq])
            if (value == 0) return BandResult::kFailed;
    }
    if (decoder.s->img_n == 3 && decoder.app14_color_transform >= 0 && decoder.app14_color_transform != 1) {
        context.Fail(ImageDecodeError::kUnsupported);
        return BandResult::kFailed;
    }
    std::memcpy(state.final_quantization, decoder.dequant, sizeof(state.final_quantization));
    state.final_transform = decoder.app14_color_transform;
    return BandResult::kDone;
}

size_t LayoutBand(Progressive& state, unsigned first, unsigned end) {
    size_t position = 0;
    const auto& decoder = state.decoder;
    for (int component = 0; component < state.input.img_n; ++component) {
        const auto& plane = decoder.img_comp[component];
        state.band_base[component] = position;
        state.band_start[component] = state.base[component] + first * plane.v * plane.coeff_w * sizeof(uint64_t);
        state.band_length[component] = component == 0 ? 0 :
            (end - first) * plane.v * plane.coeff_w * sizeof(uint64_t);
        position += state.band_length[component];
    }
    position = (position + 15) & ~size_t(15);
    const auto& plane = decoder.img_comp[0];
    const unsigned rows = (state.input.img_y + 7) / 8;
    const unsigned first_row = SelectedBefore(first * plane.v, state.selected_h, rows);
    const unsigned end_row = SelectedBefore(end * plane.v, state.selected_h, rows);
    state.band_base[3] = position;
    state.band_start[3] = state.raw_base + first_row * state.selected_w * sizeof(state.block);
    state.band_length[3] = (end_row - first_row) * state.selected_w * sizeof(state.block);
    return position + state.band_length[3];
}

bool ScanBand(Progressive& state, ScanCursor& cursor) {
    PrepareSpectralMask(state);
    auto& decoder = state.decoder;
    if (decoder.scan_n == 1) {
        const unsigned component = decoder.order[0];
        const auto& plane = decoder.img_comp[component];
        const unsigned width = (plane.x + 7) / 8;
        const unsigned height = (plane.y + 7) / 8;
        const unsigned end = std::min(height, state.band_end * plane.v);
        for (unsigned y = state.band_first * plane.v; y < end; ++y)
            for (unsigned x = 0; x < width; ++x)
                if (!Block(state, component, x, y) ||
                    !Restart(state, x + 1 < width || y + 1 < height, cursor.sequence)) return false;
    } else {
        for (unsigned y = state.band_first; y < state.band_end; ++y) {
            for (int x = 0; x < decoder.img_mcu_x; ++x) {
                for (int index = 0; index < decoder.scan_n; ++index) {
                    const unsigned component = decoder.order[index];
                    const auto& plane = decoder.img_comp[component];
                    for (int dy = 0; dy < plane.v; ++dy)
                        for (int dx = 0; dx < plane.h; ++dx)
                            if (!Block(state, component, x * plane.h + dx, y * plane.v + dy)) return false;
                }
                if (!Restart(state, x + 1 < decoder.img_mcu_x || y + 1 < unsigned(decoder.img_mcu_y), cursor.sequence)) return false;
            }
        }
    }
    if (state.band_end != unsigned(decoder.img_mcu_y)) return state.context->error == ImageDecodeError::kNone;
    if (decoder.eob_run != 0) return false;
    if (decoder.marker == STBI__MARKER_none) decoder.marker = stbi__skip_jpeg_junk_at_end(&decoder);
    return decoder.marker == cursor.end_marker && InputPosition(state) == cursor.end_position &&
           state.context->error == ImageDecodeError::kNone;
}

BandResult DecodeBanded(Progressive& state) {
    unsigned count = 0;
    const BandResult indexed = IndexScans(state, count);
    if (indexed != BandResult::kDone) return indexed;
    auto& context = *state.context;
    const size_t metadata = (count * sizeof(ScanCursor) + 15) & ~size_t(15);
    if (metadata >= context.work_cache_size) return BandResult::kFallback;
    state.band_data = context.work_cache + metadata;
    state.band_capacity = context.work_cache_size - metadata;
    const unsigned rows = state.decoder.img_mcu_y;
    for (unsigned y = 0; y < rows; ++y)
        if (LayoutBand(state, y, y + 1) > state.band_capacity) return BandResult::kFallback;
    state.banded = true;
    auto* cursors = reinterpret_cast<ScanCursor*>(context.work_cache);
    for (unsigned first = 0; first < rows;) {
        unsigned low = first + 1, high = rows, end = first;
        while (low <= high) {
            const unsigned middle = low + (high - low) / 2;
            if (LayoutBand(state, first, middle) <= state.band_capacity) { end = middle; low = middle + 1; }
            else high = middle - 1;
        }
        state.band_first = first;
        state.band_end = end;
        const size_t bytes = LayoutBand(state, first, end);
        std::memset(state.band_data, 0, bytes);
        for (unsigned index = 0; index < count; ++index) {
            auto& cursor = cursors[index];
            state.decoder = cursor.decoder;
            state.decoder.s = &state.input;
            state.position = cursor.position;
            stbi_io_callbacks callbacks{ReadProgressive, SkipProgressive, EofProgressive};
            stbi__start_callbacks(&state.input, &callbacks, &state);
            if (!ScanBand(state, cursor)) return BandResult::kFailed;
            cursor.position = InputPosition(state);
            cursor.decoder = state.decoder;
        }
        std::memcpy(state.decoder.dequant, state.final_quantization, sizeof(state.final_quantization));
        state.decoder.app14_color_transform = state.final_transform;
        if (!Finish(state)) return BandResult::kFailed;
        first = end;
    }
    return BandResult::kDone;
}

bool DecodePaged(Progressive& state) {
    auto& decoder = state.decoder;
    auto& context = *state.context;
    bool framed = false;
    bool ended = false;
    bool ok = stbi__get_marker(&decoder) == 0xd8;
    while (ok && !context.Cancelled()) {
        const unsigned marker = stbi__get_marker(&decoder);
        if (marker == 0xd9) { ended = framed && state.scans != 0; break; }
        if (marker == 0xc2) {
            if (framed) { ok = false; break; }
            ok = Frame(state);
            framed = ok;
        } else if (marker == 0xda) {
            if (decoder.s->img_n == 3 && decoder.app14_color_transform >= 0 && decoder.app14_color_transform != 1) {
                context.Fail(ImageDecodeError::kUnsupported);
                return false;
            }
            ok = framed && stbi__process_scan_header(&decoder) && ValidateScan(state) && Scan(state);
            if (ok && decoder.marker == STBI__MARKER_none)
                decoder.marker = stbi__skip_jpeg_junk_at_end(&decoder);
        } else if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4) {
            context.Fail(ImageDecodeError::kUnsupported);
            return false;
        } else {
            ok = stbi__process_marker(&decoder, marker) != 0;
        }
        if (stbi__at_eof(&state.input) && decoder.marker == STBI__MARKER_none) ok = false;
    }
    return ok && ended && context.error == ImageDecodeError::kNone && Finish(state);
}
}

size_t ProgressiveWorkspaceSize() {
    return (sizeof(Progressive) + 15) & ~size_t(15);
}

bool DecodePagedProgressive(DecodeContext& context) {
    auto* state = static_cast<Progressive*>(context.Allocate(sizeof(Progressive)));
    if (state == nullptr) return false;
    Initialize(*state, context);
    const BandResult result = DecodeBanded(*state);
    if (result == BandResult::kDone) return true;
    if (result == BandResult::kFailed) { context.Fail(ImageDecodeError::kInvalidData); return false; }
    if (context.work_cache) std::memset(context.work_cache, 0, context.work_cache_size);
    Initialize(*state, context);
    const bool ok = DecodePaged(*state);
    if (!ok) context.Fail(ImageDecodeError::kInvalidData);
    return ok;
}
}

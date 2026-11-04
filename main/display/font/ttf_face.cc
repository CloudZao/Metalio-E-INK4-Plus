#include "ttf_face.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <sys/stat.h>

#include <esp_heap_caps.h>

namespace {
constexpr size_t kReadBlock = 4096; // 单个字体读窗口的字节数
constexpr size_t kReadBlocks = 512; // 同一源字体最多共享 2 MiB 原始文件缓存
constexpr size_t kCacheWays = 4; // 每组窗口采用最近最少使用淘汰
struct ReadBlock {
    size_t offset = SIZE_MAX; // 文件起点；SIZE_MAX 表示无效
    uint64_t used = 0; // 最近访问次序
};
struct FontSource {
    FILE* file = nullptr; // 同一字体不同字号共享的 SD 文件
    std::string path; // 源文件完整路径
    size_t size = 0; // 文件字节数
    time_t modified = 0; // 用于识别同名源文件替换
    uint8_t* bytes = nullptr; // 有容量上限的 PSRAM 读窗口
    size_t block_count = 0; // 实际分配的窗口数量
    size_t position = SIZE_MAX; // 文件句柄的已知读取位置
    uint64_t clock = 0; // 窗口访问次序
    ReadBlock blocks[kReadBlocks]{}; // 四路组相联索引
    std::mutex mutex; // 串行化多个 FreeType 文件流的定位和读取

    ~FontSource() {
        if (file) std::fclose(file);
        heap_caps_free(bytes);
    }
};
struct FontStream {
    FT_StreamRec stream{}; // FreeType 文件流，descriptor 指回本包装对象
    std::shared_ptr<FontSource> source; // 随最后一个字号关闭释放文件和缓存
    std::atomic<bool>* io_failed = nullptr; // 当前字体实例的读取错误标记
};
std::mutex source_mutex;
std::weak_ptr<FontSource> last_source;

size_t ReadFile(FontSource& source, size_t offset, uint8_t* buffer, size_t count) {
    if (source.position != offset) {
        if (std::fseek(source.file, static_cast<long>(offset), SEEK_SET) != 0) {
            source.position = SIZE_MAX;
            return 0;
        }
    }
    const size_t got = std::fread(buffer, 1, count, source.file);
    source.position = offset + got;
    return got;
}

unsigned long Read(FT_Stream stream, unsigned long offset, unsigned char* buffer,
                   unsigned long count) {
    auto* owner = static_cast<FontStream*>(stream->descriptor.pointer);
    if (offset > stream->size) {
        if (owner->io_failed) owner->io_failed->store(true, std::memory_order_release);
        return count == 0 ? 1 : 0;
    }
    if (count == 0) return 0;
    auto& source = *owner->source;
    std::lock_guard<std::mutex> lock(source.mutex);
    const size_t wanted = std::min<unsigned long>(count, stream->size - offset);
    size_t got = 0;
    if (!source.bytes) {
        got = ReadFile(source, offset, buffer, wanted);
    } else {
        while (got < wanted) {
            const size_t position = offset + got;
            const size_t base = position / kReadBlock * kReadBlock;
            const uint32_t hash = static_cast<uint32_t>(base / kReadBlock) * 2654435761u;
            const size_t first = ((static_cast<uint64_t>(hash) * (source.block_count / kCacheWays)) >> 32) * kCacheWays;
            size_t slot = first;
            bool cached = false;
            for (size_t i = first; i < first + kCacheWays; ++i) {
                if (source.blocks[i].offset == base) { slot = i; cached = true; break; }
                if (source.blocks[i].used < source.blocks[slot].used) slot = i;
            }
            if (!cached) {
                source.blocks[slot].offset = SIZE_MAX;
                source.blocks[slot].used = 0;
                const size_t size = std::min(kReadBlock, source.size - base);
                if (ReadFile(source, base, source.bytes + slot * kReadBlock, size) != size) {
                    std::clearerr(source.file);
                    got += ReadFile(source, position, buffer + got, wanted - got);
                    break;
                }
                source.blocks[slot].offset = base;
            }
            source.blocks[slot].used = ++source.clock;
            const size_t within = position - base;
            const size_t size = std::min(wanted - got, kReadBlock - within);
            std::memcpy(buffer + got, source.bytes + slot * kReadBlock + within, size);
            got += size;
        }
    }
    if (got < count && owner->io_failed) owner->io_failed->store(true, std::memory_order_release);
    return static_cast<unsigned long>(got);
}

void Close(FT_Stream stream) { delete static_cast<FontStream*>(stream->descriptor.pointer); }

std::shared_ptr<FontSource> OpenSource(const char* path, FT_Error* error) {
    struct stat info{};
    const bool have_stat = stat(path, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_size > 0 && info.st_size <= LONG_MAX;
    std::lock_guard<std::mutex> lock(source_mutex);
    if (auto source = last_source.lock(); have_stat && source && source->path == path &&
        source->size == static_cast<size_t>(info.st_size) && source->modified == info.st_mtime) return source;
    auto source = std::make_shared<FontSource>();
    source->path = path;
    source->file = std::fopen(path, "rb");
    if (!source->file) {
        if (error) *error = FT_Err_Cannot_Open_Resource;
        return {};
    }
    if (have_stat) {
        source->size = info.st_size;
        source->modified = info.st_mtime;
    } else if (std::fseek(source->file, 0, SEEK_END) == 0) {
        const long end = std::ftell(source->file);
        if (end > 0) source->size = end;
    }
    if (!source->size) {
        if (error) *error = FT_Err_Invalid_Stream_Read;
        return {};
    }
    constexpr size_t reserve = 2 * 1024 * 1024;
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    constexpr size_t group = kReadBlock * kCacheWays;
    const size_t file_budget = std::min(kReadBlock * kReadBlocks, (source->size + group - 1) / group * group);
    const size_t budgets[] = {file_budget, std::min<size_t>(file_budget, 256 * 1024),
        std::min<size_t>(file_budget, 64 * 1024)};
    size_t previous = 0;
    for (size_t budget : budgets) {
        if (budget == previous) continue;
        previous = budget;
        if (free <= reserve + budget) continue;
        source->bytes = static_cast<uint8_t*>(heap_caps_malloc(budget, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (source->bytes) { source->block_count = budget / kReadBlock; break; }
    }
    last_source = source;
    return source;
}
} // namespace

bool TtfOpenFace(FT_Library library, const char* path, FT_Face* face,
                 std::atomic<bool>* io_failed, FT_Error* error) {
    if (error) *error = FT_Err_Out_Of_Memory;
    try {
        auto file = OpenSource(path, error);
        if (!file) return false;
        auto* source = new(std::nothrow) FontStream;
        if (!source) return false;
        source->source = std::move(file);
        source->io_failed = io_failed;
        source->stream.size = source->source->size;
        source->stream.descriptor.pointer = source;
        source->stream.read = Read;
        source->stream.close = Close;
        FT_Open_Args args{};
        args.flags = FT_OPEN_STREAM;
        args.stream = &source->stream;
        // FreeType 的失败路径也会关闭传入的文件流。
        const FT_Error result = FT_Open_Face(library, &args, 0, face);
        if (error) *error = result;
        return result == 0;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

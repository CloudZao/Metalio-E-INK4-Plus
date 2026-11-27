#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <esp_heap_caps.h>
#include <sys/stat.h>
#include "boards/common/storage/sd_access.h"
#include "infra/diagnostics/firmware_memory.h"
#include "ttf_face.h"

static constexpr size_t kReadBlock = 4096;  // 单个字体读窗口的字节数
static constexpr size_t kReadBlocks = 512;  // 同一源字体最多共享 2 MiB 原始文件缓存
static constexpr size_t kCacheWays = 4;     // 每组窗口采用最近最少使用淘汰

struct TtfFaceReadBlock {
    size_t offset = SIZE_MAX;  // 文件起点；SIZE_MAX 表示无效
    uint64_t used = 0;         // 最近访问次序
};

struct TtfFaceFontSource {
    bool memory_backed = false;             // 完整不可变快照，不依赖 SD 句柄或挂载代次
    bool transient = false;                 // 界面字体按次读文件，允许待机和虚拟 U 盘卸载
    unsigned generation = 0;                // 阻止旧字体对象读取重新挂载后的同名文件
    FILE* file = nullptr;                    // 同一字体不同字号共享的 SD 文件
    std::string path;                        // 源文件完整路径
    size_t size = 0;                         // 文件字节数
    time_t modified = 0;                     // 用于识别同名源文件替换
    uint8_t* bytes = nullptr;                // 完整快照或有界读窗口的 PSRAM 数据
    size_t block_count = 0;                  // 实际分配的窗口数量
    size_t position = SIZE_MAX;              // 文件句柄的已知读取位置
    uint64_t clock = 0;                      // 窗口访问次序
    TtfFaceReadBlock blocks[kReadBlocks]{};  // 四路组相联索引
    std::mutex mutex;                        // 串行化多个 FreeType 文件流的定位和读取

    ~TtfFaceFontSource() {
        if (file) {
            std::fclose(file);
        }
        heap_caps_free(bytes);
    }
};

struct TtfFaceFontStream {
    FT_StreamRec stream{};                      // FreeType 文件流，descriptor 指回本包装对象
    std::shared_ptr<TtfFaceFontSource> source;  // 随最后一个字号关闭释放文件和缓存
    std::atomic<bool>* io_failed = nullptr;     // 当前字体实例的读取错误标记
};

static std::mutex source_mutex;
static std::weak_ptr<TtfFaceFontSource> last_ui_source;
static std::weak_ptr<TtfFaceFontSource> last_reader_source;

static size_t ReadFile(TtfFaceFontSource& source, size_t offset, uint8_t* buffer, size_t count) {
    if (source.transient) {
        SdAccessTransaction transaction;
        if (!transaction || source.generation != SdAccessMountGeneration()) {
            return 0;
        }
        FILE* file = std::fopen(source.path.c_str(), "rb");
        if (!file) {
            return 0;
        }
        struct stat info{};
        // FAT VFS 的 fstat 不提供修改时间，按路径核对大小和修改时间。
        if (stat(source.path.c_str(), &info) != 0 || info.st_size != static_cast<off_t>(source.size) ||
            info.st_mtime != source.modified) {
            std::fclose(file);
            return 0;
        }
        const size_t got = std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0
                               ? std::fread(buffer, 1, count, file) : 0;
        std::fclose(file);
        return got;
    }
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

static unsigned long Read(FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count) {
    auto* owner = static_cast<TtfFaceFontStream*>(stream->descriptor.pointer);
    if (owner->source->transient && !owner->source->memory_backed &&
        owner->source->generation != SdAccessMountGeneration()) {
        if (owner->io_failed) {
            owner->io_failed->store(true, std::memory_order_release);
        }
        return count ? 0 : 1;
    }
    if (offset > stream->size) {
        if (owner->io_failed) {
            owner->io_failed->store(true, std::memory_order_release);
        }
        return count == 0 ? 1 : 0;
    }
    if (count == 0) {
        return 0;
    }
    auto& source = *owner->source;
    std::lock_guard<std::mutex> lock(source.mutex);
    const size_t wanted = std::min<unsigned long>(count, stream->size - offset);
    size_t got = 0;
    if (source.memory_backed) {
        std::memcpy(buffer, source.bytes + offset, wanted);
        got = wanted;
    } else if (!source.bytes) {
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
                if (source.blocks[i].offset == base) {
                    slot = i;
                    cached = true;
                    break;
                }
                if (source.blocks[i].used < source.blocks[slot].used) {
                    slot = i;
                }
            }
            if (!cached) {
                source.blocks[slot].offset = SIZE_MAX;
                source.blocks[slot].used = 0;
                const size_t size = std::min(kReadBlock, source.size - base);
                if (ReadFile(source, base, source.bytes + slot * kReadBlock, size) != size) {
                    if (source.file) {
                        std::clearerr(source.file);
                    }
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
    if (got < count && owner->io_failed) {
        owner->io_failed->store(true, std::memory_order_release);
    }
    return static_cast<unsigned long>(got);
}

static void Close(FT_Stream stream) {
    delete static_cast<TtfFaceFontStream*>(stream->descriptor.pointer);
}

static std::shared_ptr<TtfFaceFontSource> OpenSource(const char* path, FT_Error* error, bool transient) {
    struct stat info{};
    const bool have_stat =
        stat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= LONG_MAX;
    FirmwareMemoryDemand demand(have_stat && transient ? size_t(info.st_size) : kReadBlock * kReadBlocks);
    std::lock_guard<std::mutex> lock(source_mutex);
    auto& last_source = transient ? last_ui_source : last_reader_source;
    if (auto source = last_source.lock(); have_stat && source && source->path == path && source->transient == transient &&
                                          (!transient || source->generation == SdAccessMountGeneration()) &&
                                          source->size == static_cast<size_t>(info.st_size) &&
                                          source->modified == info.st_mtime) {
        return source;
    }
    std::shared_ptr<TtfFaceFontSource> source;
    if (transient) {
        void* memory = heap_caps_malloc(sizeof(TtfFaceFontSource), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!memory) {
            return {};
        }
        source = std::shared_ptr<TtfFaceFontSource>(new (memory) TtfFaceFontSource, [](TtfFaceFontSource* value) {
            value->~TtfFaceFontSource();
            heap_caps_free(value);
        });
    } else {
        source = std::make_shared<TtfFaceFontSource>();
    }
    source->transient = transient;
    source->generation = SdAccessMountGeneration();
    source->path = path;
    source->file = std::fopen(path, "rb");
    if (!source->file) {
        if (error) {
            *error = FT_Err_Cannot_Open_Resource;
        }
        return {};
    }
    if (have_stat) {
        source->size = info.st_size;
        source->modified = info.st_mtime;
    } else if (std::fseek(source->file, 0, SEEK_END) == 0) {
        const long end = std::ftell(source->file);
        if (end > 0) {
            source->size = end;
        }
    }
    if (!source->size) {
        if (error) {
            *error = FT_Err_Invalid_Stream_Read;
        }
        return {};
    }
    if (transient) {
        if (FirmwarePsramCanAllocate(source->size)) {
            source->bytes = static_cast<uint8_t*>(
                heap_caps_malloc(source->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (source->bytes) {
                std::rewind(source->file);
                if (std::fread(source->bytes, 1, source->size, source->file) == source->size) {
                    source->memory_backed = true;
                } else {
                    heap_caps_free(source->bytes);
                    source->bytes = nullptr;
                    std::clearerr(source->file);
                }
            }
        }
    }
    if (transient) {
        std::fclose(source->file);
        source->file = nullptr;
    }
    if (source->memory_backed) {
        last_source = source;
        return source;
    }
    constexpr size_t group = kReadBlock * kCacheWays;
    const size_t file_budget = std::min(kReadBlock * kReadBlocks, (source->size + group - 1) / group * group);
    const size_t budgets[] = {file_budget, std::min<size_t>(file_budget, 256 * 1024),
                              std::min<size_t>(file_budget, 64 * 1024)};
    size_t previous = 0;
    for (size_t budget : budgets) {
        if (budget == previous) {
            continue;
        }
        previous = budget;
        if (!FirmwarePsramCanAllocate(budget)) {
            continue;
        }
        source->bytes = static_cast<uint8_t*>(heap_caps_malloc(budget, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (source->bytes) {
            source->block_count = budget / kReadBlock;
            break;
        }
    }
    last_source = source;
    return source;
}

bool TtfOpenFace(FT_Library library, const char* path, FT_Face* face, std::atomic<bool>* io_failed, FT_Error* error,
                 bool transient, bool* memory_backed) {
    if (memory_backed) {
        *memory_backed = false;
    }
    if (error) {
        *error = FT_Err_Out_Of_Memory;
    }
    try {
        auto file = OpenSource(path, error, transient);
        if (!file) {
            return false;
        }
        const bool snapshot = file->memory_backed;
        auto* source = new (std::nothrow) TtfFaceFontStream;
        if (!source) {
            return false;
        }
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
        if (error) {
            *error = result;
        }
        if (memory_backed && result == 0) {
            *memory_backed = snapshot;
        }
        return result == 0;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool TtfFaceSnapshotMatchesFile(FT_Face face) {
    if (!face || !face->stream || face->stream->close != Close) {
        return false;
    }
    auto* stream = static_cast<TtfFaceFontStream*>(face->stream->descriptor.pointer);
    const auto source = stream->source;
    if (!source || !source->memory_backed) {
        return false;
    }
    SdAccessTransaction transaction;
    if (!transaction) {
        return false;
    }
    struct stat before{}, after{};
    if (stat(source->path.c_str(), &before) != 0 || before.st_size != static_cast<off_t>(source->size)) {
        return false;
    }
    auto* buffer = static_cast<uint8_t*>(heap_caps_malloc(kReadBlock, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) {
        return false;
    }
    FILE* file = std::fopen(source->path.c_str(), "rb");
    bool matches = file != nullptr;
    for (size_t offset = 0; matches && offset < source->size; offset += kReadBlock) {
        const size_t size = std::min(kReadBlock, source->size - offset);
        matches = std::fread(buffer, 1, size, file) == size &&
                  std::memcmp(buffer, source->bytes + offset, size) == 0;
    }
    if (file) {
        std::fclose(file);
    }
    heap_caps_free(buffer);
    return matches && stat(source->path.c_str(), &after) == 0 &&
           before.st_size == after.st_size && before.st_mtime == after.st_mtime;
}

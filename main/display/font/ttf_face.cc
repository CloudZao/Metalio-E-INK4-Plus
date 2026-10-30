#include "ttf_face.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <new>
#include <sys/stat.h>

#include <esp_heap_caps.h>

namespace {
constexpr size_t kReadBlock = 4096; // 单个字体读窗口的字节数
constexpr size_t kReadBlocks = 16; // 读窗口总计最多占用 64 KiB PSRAM
struct FontStream {
    FT_StreamRec stream{}; // FreeType 文件流，必须位于首字段
    FILE* file = nullptr; // 随字体关闭的 SD 文件
    std::atomic<bool>* io_failed = nullptr; // 可选的调用方错误标记
    uint8_t* bytes = nullptr; // 读窗口共用的 PSRAM 缓冲，申请失败时直接读取文件
    size_t offsets[kReadBlocks]{}; // 各窗口的文件起始位置；SIZE_MAX 表示无效
    size_t next_block = 0; // 下一次替换的读窗口
};

size_t ReadFile(FontStream& source, size_t offset, uint8_t* buffer, size_t count) {
    if (std::fseek(source.file, static_cast<long>(offset), SEEK_SET) != 0) return 0;
    return std::fread(buffer, 1, count, source.file);
}

unsigned long Read(FT_Stream stream, unsigned long offset, unsigned char* buffer,
                   unsigned long count) {
    auto* source = reinterpret_cast<FontStream*>(stream);
    if (offset > stream->size) {
        if (source->io_failed) source->io_failed->store(true, std::memory_order_release);
        return count == 0 ? 1 : 0;
    }
    // FreeType 的零长度调用只检查逻辑定位，不需要访问 SD。
    if (count == 0) return 0;
    const size_t wanted = std::min<unsigned long>(count, stream->size - offset);
    size_t got = 0;
    if (!source->bytes) {
        got = ReadFile(*source, offset, buffer, wanted);
    } else {
        while (got < wanted) {
            const size_t position = offset + got;
            const size_t base = position / kReadBlock * kReadBlock;
            size_t slot = 0;
            while (slot < kReadBlocks && source->offsets[slot] != base) ++slot;
            if (slot == kReadBlocks) {
                slot = source->next_block++ % kReadBlocks;
                source->offsets[slot] = SIZE_MAX;
                const size_t size = std::min<size_t>(kReadBlock, stream->size - base);
                if (ReadFile(*source, base, source->bytes + slot * kReadBlock, size) != size) {
                    // 预取失败时只重读本次所需范围，缓存不能使有效的小读失败。
                    std::clearerr(source->file);
                    got += ReadFile(*source, position, buffer + got, wanted - got);
                    break;
                }
                source->offsets[slot] = base;
            }
            const size_t within = position - base;
            const size_t size = std::min(wanted - got, kReadBlock - within);
            std::memcpy(buffer + got, source->bytes + slot * kReadBlock + within, size);
            got += size;
        }
    }
    if (got < count && source->io_failed) source->io_failed->store(true, std::memory_order_release);
    return static_cast<unsigned long>(got);
}

void Close(FT_Stream stream) {
    auto* source = reinterpret_cast<FontStream*>(stream);
    if (source->file) std::fclose(source->file);
    heap_caps_free(source->bytes);
    delete source;
}
} // namespace

bool TtfOpenFace(FT_Library library, const char* path, FT_Face* face,
                 std::atomic<bool>* io_failed, FT_Error* error) {
    if (error) *error = FT_Err_Out_Of_Memory;
    auto* source = new(std::nothrow) FontStream;
    if (!source) return false;
    std::fill_n(source->offsets, kReadBlocks, SIZE_MAX);
    source->file = std::fopen(path, "rb");
    if (!source->file) {
        if (error) *error = FT_Err_Cannot_Open_Resource;
        delete source;
        return false;
    }
    // FATFS 上部分文件不支持 SEEK_END，优先使用目录元数据。
    struct stat info{};
    unsigned long size = 0;
    if (stat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= LONG_MAX) {
        size = static_cast<unsigned long>(info.st_size);
    } else if (std::fseek(source->file, 0, SEEK_END) == 0) {
        const long end = std::ftell(source->file);
        if (end > 0) size = static_cast<unsigned long>(end);
    }
    if (size == 0) {
        if (error) *error = FT_Err_Invalid_Stream_Read;
        Close(&source->stream);
        return false;
    }
    source->io_failed = io_failed;
    source->stream.size = size;
    source->stream.read = Read;
    source->stream.close = Close;
    source->bytes = static_cast<uint8_t*>(
        heap_caps_malloc(kReadBlock * kReadBlocks, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    FT_Open_Args args{};
    args.flags = FT_OPEN_STREAM;
    args.stream = &source->stream;
    // FreeType 的失败路径也会关闭传入的文件流。
    const FT_Error result = FT_Open_Face(library, &args, 0, face);
    if (error) *error = result;
    return result == 0;
}

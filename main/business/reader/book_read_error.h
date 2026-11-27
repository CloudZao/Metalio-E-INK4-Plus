#pragma once

enum class BookReadError {
    kNone,         // 无错误
    kFileIo,       // 源文件读取失败
    kInvalidData,  // 文件损坏或内容格式无效
    kUnsupported,  // 尚不支持的格式或安全边界
    kNoMemory,     // 内存分配失败
    kCacheIo,      // SD 工作缓存创建或读写失败
    kCancelled,    // 用户退出或排版已作废
};

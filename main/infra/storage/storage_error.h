#pragma once
#include <cstdint>

enum class StorageErrorCode : uint8_t {
    None = 0, // 成功
    NotReady = 1, // 介质尚未就绪，不等同于确认未插卡
    Busy = 2, // 访问暂停或介质切换中
    UsbOwned = 3, // 介质由 USB 主机使用
    Timeout = 4, // 设备响应超时
    NotFound = 5, // 文件或目录不存在
    AccessDenied = 6, // 访问权限不足
    NoSpace = 7, // 存储空间不足
    ReadOnly = 8, // 只读介质
    NoMemory = 9, // 操作内存或许可槽不足
    InvalidArgument = 10, // 参数或路径无效
    Io = 11, // 未细分的存储 I/O 失败
    Conflict = 12, // 目标冲突
    Rejected = 13, // 后台工作未被接纳
    Cancelled = 14, // 操作取消
};

enum class StorageStage : uint8_t {
    None = 0,
    Access = 1,
    Mount = 2,
    Submit = 3,
    DirectoryOpen = 4,
    DirectoryRead = 5,
    Stat = 6,
    Open = 7,
    Read = 8,
    Seek = 9,
    Write = 10,
    Flush = 11,
    Close = 12,
    CreateDirectory = 13,
    Backup = 14,
    Replace = 15,
    Restore = 16,
    Remove = 17,
    Capacity = 18,
};

struct StorageError {
    StorageErrorCode code = StorageErrorCode::None; // 稳定分类，不依赖平台错误码取值
    StorageStage stage = StorageStage::None; // 首次失败阶段
    int system_error = 0; // 在失败点保存的 errno，零表示底层未提供
    int platform_error = 0; // 原始 SDK 错误码，零表示底层未提供

    explicit operator bool() const { return code != StorageErrorCode::None; }
};

/** @brief 将失败点已保存的 errno 转为存储错误；零值保留为未细分 I/O 失败。 */
StorageError StorageErrorFromErrno(StorageStage stage, int error);

/** @brief 获取固定错误分类名称，可用于诊断日志，不含路径或用户内容。 */
const char* StorageErrorName(StorageErrorCode code);

/** @brief 获取固定阶段名称，可用于诊断日志。 */
const char* StorageStageName(StorageStage stage);

/** @brief 记录存储失败及原始错误；正常取消和成功不输出，调用方负责重复异常限频，tag 须为固定模块标识。 */
void LogStorageError(const char* tag, const StorageError& error);

/** @brief 保存首次文件系统失败；空输出参数或已有错误时不覆盖。 */
void CaptureStorageError(StorageError* output, StorageStage stage, int error);

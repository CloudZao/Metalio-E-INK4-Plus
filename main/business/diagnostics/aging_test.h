#pragma once
#include <cstdint>
#include <memory>
#include <esp_err.h>
#include "infra/image/image_decode.h"
#include "infra/image/raster_image.h"

enum class AgingTestPhase { Idle, Running, Stopping, Restarting };

enum class AgingSdStatus { kReady, kEmpty, kFailed, kCancelled };

struct AgingSdResult {
    uint32_t generation = 0;
    int side = 0;
    RasterImage image;
    AgingSdStatus status = AgingSdStatus::kEmpty;
    StorageError error; // 共享存储错误，随结果交付
    ImageDecodeError decode_error = ImageDecodeError::kNone;
};

struct AgingTestSnapshot {
    AgingTestPhase phase = AgingTestPhase::Idle;
    int32_t restart_count = -1;
    uint32_t cycles = 0;
    bool save_failed = false;
    std::shared_ptr<const AgingSdResult> preview; // 只读结果持有像素，界面解绑后才能归还
};

/** @brief 启动时加载续跑状态，不创建界面或测试负载。 */
bool AgingTestLoadBootState();

/** @brief 启动音频、振动、SD读取及重启协调，预览尺寸由呈现方提供。 */
bool AgingTestStart(int preview_side);

/** @brief 请求停止测试并持久化退出状态，离页不影响保存重试和资源收尾。 */
void AgingTestStop();

/** @brief 从系统节拍重试尚未提交的状态，计时器故障或页面退出后仍可收尾。 */
void AgingTestPoll();

/** @brief 查询测试是否处于运行或重启协调阶段。 */
bool AgingTestIsActive();

/** @brief 读取测试进度与最新图片结果，不执行硬件或存储操作。 */
AgingTestSnapshot AgingTestRead();

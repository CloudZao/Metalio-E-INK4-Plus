#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

inline constexpr size_t kPanelCalibrationStorageIdCapacity = 80;  // 校准标签缓冲容量
struct PanelCalibrationStorageResult {
    int vcom_mv = 0;                                   // 校准负毫伏值
    char panel[kPanelCalibrationStorageIdCapacity]{};  // 校准标签
    const char* error = nullptr;                       // 稳定结果码，空表示成功
    const char* stage = "read";                        // 最后执行阶段
};

/** @brief 检查面板电压是否满足所选板型范围及步进。 */
bool PanelCalibrationStorageValidMv(int mv);

/** @brief 检查校准标签长度及允许字符。 */
bool PanelCalibrationStorageValidId(const char* panel);

/** @brief 同步读取并校验蓝牙芯片校准记录，不操作 UI 或电源策略。 */
PanelCalibrationStorageResult PanelCalibrationStorageRead();

/** @brief 写入、刷盘并读回校验蓝牙芯片校准记录，保留现有记录格式。 */
PanelCalibrationStorageResult PanelCalibrationStorageWrite(int mv, const char* panel);

/** @brief 向校准事务交付 UART 数据，由硬件串口观察者调用。 */
void PanelCalibrationStorageOnUartData(const std::vector<uint8_t>& data);

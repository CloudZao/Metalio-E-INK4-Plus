#pragma once
#include <atomic>
#include <memory>

struct CellDiagnosticResult {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> completed{false};
    int slot = 0;
    bool pass = false;
    char detail[64] = {};
};
using CellDiagnosticRequest = std::shared_ptr<CellDiagnosticResult>;

/** @brief 检测指定SIM注册与连通性，结束或取消后恢复原槽位；忙碌时未接纳。 */
CellDiagnosticRequest CellDiagnosticStart(int slot);

/** @brief 请求取消，单次AT等待返回后继续执行射频和槽位恢复。 */
void CellDiagnosticCancel(const CellDiagnosticRequest& request);

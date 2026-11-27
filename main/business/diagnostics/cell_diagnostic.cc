#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "infra/tasks/task_create.h"
#include "assets/lang_cellular.h"
#include "boards/common/board/dual_network_board.h"
#include "boards/common/board/nt26_board.h"
#include "business/network/network_settings.h"
#include "sysmgmt/power/policy/power_policy.h"
#include "cell_diagnostic.h"

static constexpr const char* kTag = "CellDiagnostic";
static constexpr int kSettingsTestSimExternal = 0, kSettingsTestSimInternal = 1;
static constexpr unsigned kStackBytes = 8192;
static thread_local CellDiagnosticResult* s_active = nullptr;
static thread_local bool s_restoring_sim = false;
static uint32_t s_last_cfun_ms = 0;
static std::mutex s_mutex;
static bool s_running = false;

static bool CellSessionLive() {
    return s_restoring_sim || (s_active && !s_active->cancelled.load());
}

// 手册：AT+ECPING="url",number,size,delay(ms)
static constexpr int kPingCount = 3;
static constexpr int kPingSize = 32;
static constexpr int kPingDelayMs = 15000;
static constexpr uint32_t kEcpingTimeoutMs = static_cast<uint32_t>(kPingCount) * kPingDelayMs + 20000;
static constexpr uint32_t kSwitchRegWaitMs = 30000;
static constexpr uint32_t kAtShortMs = 3000;
static constexpr uint32_t kAtMediumMs = 8000;
static constexpr uint32_t kAtLongMs = 12000;
static constexpr uint32_t kAtProbeMs = 800;
static constexpr uint32_t kPostCfunSettleMs = 40000;
static constexpr uint32_t kModemResetSettleMs = 1500;
static constexpr int kAtProbeRetries = 5;
static constexpr int kAtProbeAfterResetRetries = 12;
static constexpr int kAtDefaultRetries = 3;

static bool HasAtError(const std::string& resp) {
    if (resp.find("+CME ERROR") != std::string::npos || resp.find("+CMS ERROR") != std::string::npos) {
        return true;
    }
    if (resp.find("ERROR") != std::string::npos && resp.find("OK") == std::string::npos) {
        return true;
    }
    return false;
}

static bool RunAtOk(Nt26Board* nt26, const std::string& cmd, std::string& resp, uint32_t timeout_ms) {
    resp.clear();
    const esp_err_t err = nt26->SendAtCommand(cmd, resp, timeout_ms, true);
    if (err != ESP_OK) {
        return false;
    }
    return resp.find("OK") != std::string::npos && !HasAtError(resp);
}

static bool RunAtOkRetry(Nt26Board* nt26, const std::string& cmd, std::string& resp, uint32_t timeout_ms,
                         int max_retries) {
    for (int i = 0; i < max_retries; ++i) {
        if (!CellSessionLive()) {
            return false;
        }
        if (RunAtOk(nt26, cmd, resp, timeout_ms)) {
            return true;
        }
        if (i + 1 < max_retries) {
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
    return false;
}

static bool ProbeModemAt(Nt26Board* nt26, std::string& resp, uint32_t timeout_ms, esp_err_t* out_err) {
    resp.clear();
    const esp_err_t err = nt26->SendAtCommand("AT", resp, timeout_ms, true);
    if (out_err != nullptr) {
        *out_err = err;
    }
    return err == ESP_OK && resp.find("OK") != std::string::npos && !HasAtError(resp);
}

static bool TryAtProbeBurst(Nt26Board* nt26, int attempts, uint32_t timeout_ms, uint32_t gap_ms, esp_err_t* out_err) {
    std::string resp;
    for (int i = 0; i < attempts; ++i) {
        if (!CellSessionLive()) {
            return false;
        }
        if (ProbeModemAt(nt26, resp, timeout_ms, out_err)) {
            return true;
        }
        if (i + 1 < attempts) {
            vTaskDelay(pdMS_TO_TICKS(gap_ms));
        }
    }
    return false;
}

static void RunAt(Nt26Board* nt26, const std::string& cmd, std::string& resp, uint32_t timeout_ms) {
    resp.clear();
    nt26->SendAtCommand(cmd, resp, timeout_ms, true);
}

static void SoftResetModem(Nt26Board* nt26) {
    std::string resp;
    RunAt(nt26, "AT+ECRST", resp, 500);
    vTaskDelay(pdMS_TO_TICKS(kModemResetSettleMs));
}

static bool EnsureModemAlive(Nt26Board* nt26, char* detail, size_t detail_len) {
    esp_err_t last_err = ESP_OK;

    if (TryAtProbeBurst(nt26, kAtProbeRetries, kAtProbeMs, 200, &last_err)) {
        return true;
    }

    ESP_LOGW(kTag, "AT probe failed err=%s, try ECRST", esp_err_to_name(last_err));
    SoftResetModem(nt26);
    if (TryAtProbeBurst(nt26, kAtProbeAfterResetRetries, 500, 150, &last_err)) {
        ESP_LOGI(kTag, "modem responsive after ECRST");
        return true;
    }

    std::string resp;
    RunAtOkRetry(nt26, "AT+CFUN=1", resp, kAtLongMs, 2);
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (TryAtProbeBurst(nt26, 4, kAtShortMs, 300, &last_err)) {
        ESP_LOGI(kTag, "modem responsive after CFUN=1");
        return true;
    }

    if (last_err == ESP_ERR_INVALID_STATE) {
        std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_MODEM_NOT_READY);
    } else if (last_err == ESP_ERR_TIMEOUT) {
        std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_MODEM_NO_RESP);
    } else {
        std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_MODEM_NO_RESP);
    }
    ESP_LOGE(kTag, "modem not alive after recovery err=%s", esp_err_to_name(last_err));
    return false;
}

static bool IsSimAbsent(const std::string& resp) {
    return resp.find("+CME ERROR: 10") != std::string::npos || resp.find("+CME ERROR: 13") != std::string::npos ||
           resp.find("+CME ERROR: 14") != std::string::npos;
}

static int ParseSimSlotFromEcsimcfg(const std::string& resp) {
    constexpr const char* kKey = "\"SimSlot\"";
    size_t pos = 0;
    while ((pos = resp.find(kKey, pos)) != std::string::npos) {
        const size_t comma = resp.find(',', pos);
        if (comma == std::string::npos) {
            return -1;
        }
        size_t i = comma + 1;
        while (i < resp.size() && (resp[i] == ' ' || resp[i] == '\t')) {
            ++i;
        }
        if (i >= resp.size() || !std::isdigit(static_cast<unsigned char>(resp[i]))) {
            pos = comma + 1;
            continue;
        }
        int slot = 0;
        while (i < resp.size() && std::isdigit(static_cast<unsigned char>(resp[i]))) {
            slot = slot * 10 + (resp[i] - '0');
            ++i;
        }
        return slot;
    }
    return -1;
}

static int QueryCurrentSimSlot(Nt26Board* nt26) {
    std::string resp;
    if (!RunAtOkRetry(nt26, "AT+ECSIMCFG?", resp, kAtShortMs, kAtDefaultRetries)) {
        return -1;
    }
    return ParseSimSlotFromEcsimcfg(resp);
}

static int ParseCeregStat(const std::string& resp) {
    const size_t pos = resp.find("+CEREG:");
    if (pos == std::string::npos) {
        return -1;
    }
    int n = 0;
    int stat = 0;
    const char* p = resp.c_str() + pos;
    if (std::sscanf(p, "+CEREG: %d,%d", &n, &stat) >= 2 && n <= 3) {
        return stat;
    }
    if (std::sscanf(p, "+CEREG: %d", &stat) == 1) {
        return stat;
    }
    return -1;
}

static bool IsSimReady(const std::string& resp) {
    return resp.find("+CPIN: READY") != std::string::npos;
}

static bool HasIpv4Address(const std::string& resp) {
    if (resp.find("+CGPADDR:") == std::string::npos) {
        return false;
    }
    int a = 0, b = 0, c = 0, d = 0;
    return std::sscanf(resp.c_str(), "+CGPADDR: %*d,\"%d.%d.%d.%d", &a, &b, &c, &d) == 4 ||
           std::sscanf(resp.c_str(), "\r\n+CGPADDR: %*d,\"%d.%d.%d.%d", &a, &b, &c, &d) == 4;
}

static void MarkModemRadioReset() {
    s_last_cfun_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void ClearModemRadioReset() {
    s_last_cfun_ms = 0;
}

static bool NeedsRegistrationWait(int target_slot, int current_slot) {
    if (current_slot != target_slot) {
        return true;
    }
    if (s_last_cfun_ms == 0) {
        return false;
    }
    const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    return (now - s_last_cfun_ms) < kPostCfunSettleMs;
}

static bool WaitSimRegisteredForPing(Nt26Board* nt26, char* detail, size_t detail_len, uint32_t max_wait_ms) {
    const uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    std::string resp;
    RunAtOk(nt26, "AT+CEREG=2", resp, kAtShortMs);

    while ((xTaskGetTickCount() * portTICK_PERIOD_MS - start) < max_wait_ms) {
        if (!CellSessionLive()) {
            std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_CANCELLED);
            return false;
        }
        RunAt(nt26, "AT+CPIN?", resp, kAtShortMs);
        if (IsSimAbsent(resp)) {
            std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_NO_SIM);
            return false;
        }
        if (!IsSimReady(resp)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (RunAtOk(nt26, "AT+CEREG?", resp, kAtShortMs)) {
            const int stat = ParseCeregStat(resp);
            if (stat == 3) {
                std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_REG_REJECT);
                return false;
            }
            if (stat == 1 || stat == 5) {
                return true;
            }
        }
        if (RunAtOk(nt26, "AT+CGPADDR=1", resp, kAtShortMs) && HasIpv4Address(resp)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_SEARCH_TIMEOUT);
    return false;
}

static bool ApplySimSlot(Nt26Board* nt26, int target_slot, char* detail, size_t detail_len) {
    std::string resp;
    if (!RunAtOkRetry(nt26, "AT+CFUN=0", resp, kAtMediumMs, kAtDefaultRetries)) {
        std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_CFUN0_FAIL);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "AT+ECSIMCFG=SimSlot,%d", target_slot);
    if (!RunAtOkRetry(nt26, buf, resp, kAtShortMs, kAtDefaultRetries)) {
        RunAtOkRetry(nt26, "AT+CFUN=1", resp, kAtLongMs, 2);
        std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_SIM_SWITCH_FAIL);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!RunAtOkRetry(nt26, "AT+CFUN=1", resp, kAtLongMs, kAtDefaultRetries)) {
        std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_CFUN1_FAIL);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (QueryCurrentSimSlot(nt26) != target_slot) {
        std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_SLOT_FAIL);
        return false;
    }
    MarkModemRadioReset();
    return true;
}

static void RestoreSimSlotQuiet(Nt26Board* nt26, int target_slot) {
    char detail[64];
    s_restoring_sim = true;
    try {
        if (!ApplySimSlot(nt26, target_slot, detail, sizeof(detail))) {
            ESP_LOGE(kTag, "operation=cell_test stage=restore_slot result=failed");
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(kTag, "operation=cell_test stage=restore_slot result=no_memory");
    }
    s_restoring_sim = false;
}

static bool ParseEcpingSummary(const std::string& resp, int* tx, int* rx) {
    size_t pos = 0;
    while ((pos = resp.find("+ECPING:", pos)) != std::string::npos) {
        const char* p = resp.c_str() + pos;
        int local_tx = 0;
        int local_rx = 0;
        if (std::sscanf(p, "+ECPING: dest: %*[^,], %d packets transmitted, %d received", &local_tx, &local_rx) == 2 ||
            std::sscanf(p, "+ECPING: dest: %*[^,], %d packets transmittted, %d received", &local_tx, &local_rx) == 2) {
            *tx = local_tx;
            *rx = local_rx;
            return true;
        }
        pos += 8;
    }
    return false;
}

static bool IsEcpingSuccess(const std::string& resp) {
    if (HasAtError(resp)) {
        return false;
    }
    if (resp.find("+ECPING: FAIL") != std::string::npos || resp.find("+ECPING: TIMEOUT") != std::string::npos) {
        return false;
    }
    if (resp.find("+ECPING: SUCC") != std::string::npos) {
        return true;
    }
    int tx = 0;
    int rx = 0;
    if (ParseEcpingSummary(resp, &tx, &rx)) {
        return rx > 0 && (tx <= 0 || rx >= tx);
    }
    return false;
}

static bool RunEcpingTest(Nt26Board* nt26, char* detail, size_t detail_len) {
    char cmd[96];
    std::snprintf(cmd, sizeof(cmd), "AT+ECPING=\"www.baidu.com\",%d,%d,%d", kPingCount, kPingSize, kPingDelayMs);
    std::string resp;

    auto run_once = [&]() -> bool {
        resp.clear();
        const esp_err_t err = nt26->SendAtCommandCollectUntil(cmd, resp, kEcpingTimeoutMs, "+ECPING: DONE", true);
        ESP_LOGI(kTag, "ECPING err=%d len=%u", static_cast<int>(err), static_cast<unsigned>(resp.size()));
        if (IsEcpingSuccess(resp)) {
            return true;
        }
        if (HasAtError(resp)) {
            std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_PING_CMD_FAIL);
        } else if (err == ESP_ERR_TIMEOUT) {
            std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_PING_WAIT_TIMEOUT);
        } else {
            int tx = 0;
            int rx = 0;
            if (ParseEcpingSummary(resp, &tx, &rx)) {
                std::snprintf(detail, detail_len, LangString_SETTINGS_TEST_CELL_LOSS_FMT, rx, tx);
            } else {
                std::snprintf(detail, detail_len, "%s", LangString_SETTINGS_TEST_CELL_PING_FAIL);
            }
        }
        return false;
    };

    if (run_once()) {
        return true;
    }

    // PING 超时/失败后再探活并重试一次（避免偶发 AT 通道未唤醒）
    char alive_detail[32];
    if (!EnsureModemAlive(nt26, alive_detail, sizeof(alive_detail))) {
        return false;
    }
    ESP_LOGW(kTag, "ECPING retry after modem recovery");
    return run_once();
}


struct CellDiagnosticRestore {
    Nt26Board& board;
    int original_slot = -1;
    bool changed = false;
    ~CellDiagnosticRestore() {
        if (!changed) {
            return;
        }
        if (original_slot >= 0) {
            RestoreSimSlotQuiet(&board, original_slot);
        }
        // 包含取消、切换中断和内存异常出口，不能将调制解调器留在CFUN=0。
        try {
            std::string response;
            if (!RunAtOk(&board, "AT+CFUN=1", response, kAtLongMs)) {
                ESP_LOGE(kTag, "operation=cell_test stage=restore_radio result=failed");
            }
        } catch (const std::bad_alloc&) {
            ESP_LOGE(kTag, "operation=cell_test stage=restore_radio result=no_memory");
        }
    }
};

static void RunCellTest(CellDiagnosticResult& result) {
    auto& board = Board::GetInstance();
    auto* dual = dynamic_cast<DualNetworkBoard*>(&board);
    auto* nt26 = dynamic_cast<Nt26Board*>(dual ? &dual->GetCurrentBoard() : &board);
    if (!nt26) {
        std::snprintf(result.detail, sizeof(result.detail), "%s", LangString_SETTINGS_TEST_CELL_NEED_4G);
        return;
    }
    if (!EnsureModemAlive(nt26, result.detail, sizeof(result.detail))) {
        return;
    }
    CellDiagnosticRestore restore{*nt26};
    restore.original_slot = QueryCurrentSimSlot(nt26);
    if (restore.original_slot != 0 && restore.original_slot != 1) {
        restore.original_slot = -1;
    }
    if (restore.original_slot != result.slot) {
        restore.changed = true;
        if (!ApplySimSlot(nt26, result.slot, result.detail, sizeof(result.detail))) {
            return;
        }
    }
    if (NeedsRegistrationWait(result.slot, restore.original_slot) || restore.changed) {
        char detail[64];
        if (!WaitSimRegisteredForPing(nt26, detail, sizeof(detail), kSwitchRegWaitMs)) {
            if (!CellSessionLive() || std::strcmp(detail, LangString_SETTINGS_TEST_CELL_NO_SIM) == 0) {
                std::snprintf(result.detail, sizeof(result.detail), "%s", detail);
                return;
            }
            ESP_LOGW(kTag, "operation=cell_test stage=registration result=timeout; attempting_ping");
        }
    }
    if (CellSessionLive() && RunEcpingTest(nt26, result.detail, sizeof(result.detail))) {
        result.pass = true;
        std::snprintf(result.detail, sizeof(result.detail), "%s", LangString_SETTINGS_TEST_CELL_PING_OK);
        ClearModemRadioReset();
    }
}

static void CellWorker(void* context) {
    CellDiagnosticRequest result;
    {
        std::unique_ptr<CellDiagnosticRequest> owner(static_cast<CellDiagnosticRequest*>(context));
        result = *owner;
        PowerNeedHold hold(PowerNeed::StandbyInhibit);
        NetworkModemSession modem;
        s_active = result.get();
        if (modem) {
            try {
                RunCellTest(*result);
            } catch (const std::bad_alloc&) {
                ESP_LOGE(kTag, "operation=cell_test stage=execute result=no_memory");
            }
        } else {
            std::snprintf(result->detail, sizeof(result->detail), "%s", LangString_SETTINGS_TEST_CELL_WAIT_RESOURCE);
        }
        s_active = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_running = false;
    }
    result->completed.store(true, std::memory_order_release);
    result.reset();
    vTaskDelete(nullptr);
}

CellDiagnosticRequest CellDiagnosticStart(int slot) try {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_running || (slot != 0 && slot != 1)) {
        return {};
    }
    auto result = std::make_shared<CellDiagnosticResult>();
    result->slot = slot;
    std::snprintf(result->detail, sizeof(result->detail), "%s", LangString_SETTINGS_TEST_CELL_TEST_FAIL);
    auto owner = std::make_unique<CellDiagnosticRequest>(result);
    s_running = true;
    if (FirmwareCreateTask(CellWorker, "cell_diagnostic", kStackBytes, owner.get(), 5, nullptr, tskNO_AFFINITY, 0) != pdPASS) {
        s_running = false;
        ESP_LOGW(kTag, "operation=cell_test stage=create_task result=unavailable");
        return {};
    }
    owner.release();
    return result;
} catch (const std::bad_alloc&) {
    ESP_LOGE(kTag, "operation=cell_test stage=submit result=no_memory");
    return {};
}

void CellDiagnosticCancel(const CellDiagnosticRequest& request) {
    if (request) {
        request->cancelled.store(true);
    }
}

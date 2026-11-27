#include <atomic>
#include <esp_log.h>
#include "sysmgmt/time/network_time.h"
#include "boards/common/board/board.h"
#include "infra/network/time/ntp_server.h"
#include "sysmgmt/power/policy/power_policy.h"

static const char* TAG = "NetworkTime";

static std::atomic<bool> s_network_time_synced{false};

void TimeServiceCommitNetworkTime() {
    s_network_time_synced.store(true, std::memory_order_release);
    Board::GetInstance().OnNetworkTimeSynced();
}

void TimeServiceSyncNetwork() {
    auto& board = Board::GetInstance();
    if (s_network_time_synced.load() || !board.IsNetworkReady() || board.IsWifiConfigMode()) {
        return;
    }

    PowerNeedHold network(PowerNeed::ForegroundNetwork);
    NtpServer::Result result;
    if (!NtpServer::GetInstance().Sync("", result)) {
        return;
    }
    TimeServiceCommitNetworkTime();
    ESP_LOGI(TAG, "NTP fallback synchronized");
}

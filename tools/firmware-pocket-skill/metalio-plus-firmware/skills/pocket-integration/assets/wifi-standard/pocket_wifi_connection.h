#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pocket {

struct WifiAccessPoint {
    std::string ssid;
    int rssi = -100;
    bool secured = true;
};

enum class WifiFailure { None, Storage, QueueBusy, Reset, Authentication,
                         Association, NoIp, Start, Timeout };
// Safe human display; preserves raw SSID for storage/connection separately.
std::string WifiSsidLabel(const std::string& raw_ssid);
std::string WifiFailureMessage(WifiFailure failure, int reason_code = 0);

struct WifiConnectionView {
    uint32_t revision = 0;
    bool ready = false;
    bool startup_complete = false;
    uint32_t connection_id = 0;
    bool scanning = false;
    bool connecting = false;
    bool connected = false;
    int signal_bars = 0;
    std::string ssid;
    std::string ip;
    std::string message;
    WifiFailure failure = WifiFailure::None;
    int reason_code = 0;
    std::vector<WifiAccessPoint> access_points;
};

// Start once from an internal-RAM task after board/storage and InitializeApplicationWifi. The
// service owns radio operations; UI calls below only copy cached data or queue
// requests. Credentials remain in pocket_wifi and are never exposed in View.
void StartWifiConnection();
WifiConnectionView WifiConnectionSnapshot();
bool RequestWifiScan();
// Caller saves/validates the user's entered password first. This API reads the
// latest cached credential on its worker, including every automatic reconnect;
// the network thread never reads TF or writes public Wi-Fi authority.
bool RequestWifiConnect(const std::string& ssid);
bool RequestWifiDisconnect();
// Controller only. Cancel radio work, stop the driver, and await acknowledgement.
// The service worker/event loop remains allocated and parked until reboot or Resume.
// This is not HTTP/audio shutdown; stop those separately before flash/return/sleep.
// A timeout is failure: do not claim radio stopped or proceed with unsafe shutdown.
bool SuspendWifiConnection(uint32_t timeout_ms);
void ResumeWifiConnection();

}  // namespace pocket

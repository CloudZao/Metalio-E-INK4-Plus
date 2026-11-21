#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pocket {

constexpr std::size_t kMaximumApplicationWifiNetworks = 10;
constexpr std::size_t kMaximumWifiSsidBytes = 32;
constexpr std::size_t kMaximumWifiPasswordBytes = 64;

struct WifiNetwork {
    std::string ssid;
    std::string password;
};

// Storage-worker APIs: directories are existing, trusted paths supplied by the
// runtime after mounting TF and validating the run. Never create/fallback to a
// different user/app directory. No NVS or public-authority writes are performed.
// Standalone passes integrated=false and empty run_id. An integrated run_id is
// exactly 32 lowercase hex bytes. Guide 1.1 shared/wifi_current is optional/read-only;
// absent, corrupt or unsupported public data never blocks local Wi-Fi setup.
// Private errors return false but leave readable cached public/empty credentials
// so scanning can still work. Private saves remain disabled until reinitialized.
bool InitializeApplicationWifi(const std::string& shared_directory,
                               const std::string& private_directory,
                               bool integrated, const std::string& run_id,
                               std::string* error);
void DisableApplicationWifiStorage();

// Cached-only reads: safe on the network task; never touch TF. Last entry is
// preferred. New runs prefer the latest public credentials; this run's manual
// change takes precedence, including after reboot with the same run_id.
bool ReadApplicationWifi(std::vector<WifiNetwork>* networks, std::string* error);

// Only user actions call Save. Persist in private wifi_local with readback before
// connection, even if connection later fails. Integrated manual Save also marks
// one private pending candidate bound to this run. Startup/import never does.
// SSID 1..32 bytes, password 0..64 bytes; embedded NUL is rejected. Never log
// returned credentials. A full private list rejects a new SSID, not an update.
bool SaveApplicationWifi(const std::string& ssid, const std::string& password,
                         std::string* error);
// Forget affects the private list and current RAM list only, never public data.
bool ForgetApplicationWifi(const std::string& ssid, std::string* error);

// Runtime may offer this credential through the protocol on orderly return.
// This module never writes shared authority, wifi_offer or protocol receipts.
// New run/standalone cannot read an earlier run's pending offer. Clear only after
// successful protocol submission. Submission failure need not block app exit.
bool ReadPendingWifiOffer(WifiNetwork* network, bool* available, std::string* error);
bool ClearPendingWifiOffer(std::string* error);

}  // namespace pocket

#pragma once

#include "pocket_wifi_connection.h"
#include <algorithm>
#include <utility>

namespace pocket::wifi_detail {

inline bool ValidRawSsid(const std::string& text) {
    return !text.empty() && text.size() <= 32 && text.find('\0') == std::string::npos;
}

inline bool DisplayableSsid(const std::string& text) {
    if (text.empty() || text.size() > 32) return false;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i++]);
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) continue;
        unsigned n = 0, cp = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 7; minimum = 0x10000; }
        else return false;
        if (i + n > text.size()) return false;
        while (n--) {
            c = static_cast<unsigned char>(text[i++]);
            if ((c & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            (cp >= 0x80 && cp <= 0x9f)) return false;
    }
    return true;
}

// Local admission gate; SDK events do not carry our request generation.
// A stopped-driver + event barrier separates old and new physical sessions.
inline bool AcceptLinkEvent(bool admitted, std::uint32_t active_generation,
                            std::uint32_t requested_generation, bool suspending) {
    return admitted && active_generation == requested_generation && !suspending;
}

// Exact SDK NOT_INIT evidence only. Unknown driver/AP errors are not a stop
// acknowledgement, even if the UI happened to display "not ready".
inline bool UninitializedRadioIsStopped(bool driver_not_initialized, bool ap_not_initialized,
                                       bool radio_started, bool netif_up) {
    return driver_not_initialized && ap_not_initialized && !radio_started && !netif_up;
}

inline int SignalBars(int rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -78) return 2;
    return 1;
}

// An old IP left in esp_netif, an association without DHCP, or an AP other than
// the requested SSID must never produce a connected icon.
inline bool LinkReady(bool associated, bool fresh_ip_event, bool netif_up,
                      bool nonzero_ip, const std::string& expected_ssid,
                      const std::string& actual_ssid) {
    return associated && fresh_ip_event && netif_up && nonzero_ip &&
           !expected_ssid.empty() && expected_ssid == actual_ssid;
}

inline std::vector<WifiAccessPoint> VisibleAccessPoints(std::vector<WifiAccessPoint> input) {
    input.erase(std::remove_if(input.begin(), input.end(), [](const WifiAccessPoint& item) {
        return !ValidRawSsid(item.ssid);
    }), input.end());
    std::stable_sort(input.begin(), input.end(), [](const WifiAccessPoint& a, const WifiAccessPoint& b) {
        return a.rssi > b.rssi || (a.rssi == b.rssi && a.ssid < b.ssid);
    });
    std::vector<WifiAccessPoint> result;
    result.reserve(std::min<size_t>(input.size(), 24));
    for (auto& item : input) {
        if (std::any_of(result.begin(), result.end(), [&](const WifiAccessPoint& prior) {
            return prior.ssid == item.ssid;
        })) continue;
        result.push_back(std::move(item));
        if (result.size() == 24) break;
    }
    return result;
}

}  // namespace pocket::wifi_detail

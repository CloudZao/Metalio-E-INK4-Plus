#include <font_awesome.h>
#include "network_status.h"
#include "assets/lang_config.h"

const char* UiNetworkIcon(const NetworkStatus& status) {
    if (status.kind == NetworkKind::Wifi) {
        if (status.phase == NetworkPhase::Provisioning) {
            return FONT_AWESOME_WIFI;
        }
        if (status.phase != NetworkPhase::Ready) {
            return FONT_AWESOME_PLANE;
        }
        if (status.signal >= -60) {
            return FONT_AWESOME_WIFI;
        }
        return status.signal >= -70 ? FONT_AWESOME_WIFI_FAIR : FONT_AWESOME_WIFI_WEAK;
    }
    if (status.phase != NetworkPhase::Ready || status.signal < 0 || status.signal > 31) {
        return FONT_AWESOME_SIGNAL_OFF;
    }
    if (status.signal <= 9) {
        return FONT_AWESOME_SIGNAL_WEAK;
    }
    if (status.signal <= 14) {
        return FONT_AWESOME_SIGNAL_FAIR;
    }
    return status.signal <= 19 ? FONT_AWESOME_SIGNAL_GOOD : FONT_AWESOME_SIGNAL_STRONG;
}

const char* UiNetworkBusyText(const NetworkStatus& status) {
    if (status.phase == NetworkPhase::Scanning) {
        return LangString_WIFI_STATUS_SCANNING;
    }
    if (status.phase != NetworkPhase::Connecting) {
        return nullptr;
    }
    return status.kind == NetworkKind::Wifi ? LangString_WIFI_STATUS_CONNECTING : LangString_REGISTERING_NETWORK;
}

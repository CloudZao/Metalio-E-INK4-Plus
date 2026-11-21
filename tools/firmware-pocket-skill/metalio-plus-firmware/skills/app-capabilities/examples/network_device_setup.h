#pragma once
#include "pocket_wifi.h"
#include "pocket_wifi_connection.h"
#include <string>

namespace plus_examples {
// Invoke on a serialized internal-RAM storage/controller worker, after bridge
// validation and mounted-card ownership checks. private_directory is a trusted,
// app-owned directory ALREADY created by the application, not shared authority.
// false reports private storage trouble, but radio/scanning is still started.
inline bool InitializeDeviceWifi(const std::string& shared_directory,
                                 const std::string& private_directory,
                                 bool integrated, const std::string& verified_run_id,
                                 std::string* storage_error) {
    const bool storage_ok = pocket::InitializeApplicationWifi(
        shared_directory, private_directory, integrated, verified_run_id, storage_error);
    // Do not make scan conditional on storage_ok: a missing/corrupt config is
    // not a missing radio. UI should offer setup and report storage separately.
    pocket::StartWifiConnection();
    return storage_ok;
}
enum class WifiSubmit { StorageFailed, QueueBusy, Connecting };
// UI collects SSID/password on DEVICE using the app's keyboard. Then dispatch
// this action to storage worker; no blocking TF IO inside touch callbacks.
inline WifiSubmit SaveAndConnectDeviceWifi(const std::string& ssid, const std::string& password,
                                          std::string* error) {
    if (!pocket::SaveApplicationWifi(ssid, password, error)) {
        if (error) *error = pocket::WifiFailureMessage(pocket::WifiFailure::Storage);
        return WifiSubmit::StorageFailed;
    }
    if (!pocket::RequestWifiConnect(ssid)) {
        if (error) *error = pocket::WifiFailureMessage(pocket::WifiFailure::QueueBusy);
        return WifiSubmit::QueueBusy;
    }
    if (error) error->clear();
    return WifiSubmit::Connecting;
}
// Store this separately from the radio snapshot. Radio ready/reconnect events
// must never erase a failed SAVE or the "saved but queue busy" action result.
struct WifiPageStatus {
    std::string action_error;
    std::string storage_error;
    void Submit(const std::string& ssid, const std::string& password) {
        (void)SaveAndConnectDeviceWifi(ssid, password, &action_error);
    }
    std::string Message(const pocket::WifiConnectionView& state) const {
        if (!action_error.empty()) return action_error;
        if (!storage_error.empty()) return storage_error;
        return state.message; // includes reset/auth/association/no-IP + numeric reason
    }
};
// Home, AP list and password page all render Message(snapshot) BEFORE any
// early return for !connected. Do not hide diagnostics behind an online gate.
// Display SSIDs through WifiSsidLabel(raw); pass the original raw bytes to Save
// and Connect. Never put the escaped display label back into storage/SDK calls.
// UI bind:
// Search button -> RequestWifiScan()
// AP row -> selected SSID; password field -> local masked draft; confirm -> above
// View -> WifiConnectionSnapshot(); connected is genuine link+fresh IP, not API success.
// No password/token is in View; never copy credentials into an HTML preview/log.
}


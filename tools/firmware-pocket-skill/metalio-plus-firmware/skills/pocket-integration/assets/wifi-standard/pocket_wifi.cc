#include "pocket_wifi.h"
#include "pocket_core.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <sys/stat.h>
#include <utility>

namespace pocket {
namespace {
// Application-private legacy list subcodec only; NEVER a public protocol format.
constexpr std::array<char, 8> kPrivateListMagic = {'P', 'K', 'W', 'I', 'F', 'I', 1, 0};
constexpr std::array<char, 8> kPrivateMagic = {'P', 'K', 'A', 'W', 'I', 'F', 'I', '1'};
constexpr char kPrivateKey[] = "wifi_local";
struct PrivateState {
    std::vector<WifiNetwork> networks;
    std::string manual_run;
    std::string manual_ssid;
    bool pending = false;
};
std::mutex wifi_mutex;
bool initialized = false;
bool writable = false;
bool integrated_run = false;
std::string private_root;
std::string current_run;
PrivateState local;
std::vector<WifiNetwork> cached;

bool Fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
bool Success(std::string* error) {
    if (error) error->clear();
    return true;
}
bool ValidSsid(const std::string& s) {
    return !s.empty() && s.size() <= kMaximumWifiSsidBytes && s.find('\0') == std::string::npos;
}
bool ValidPassword(const std::string& s) {
    return s.size() <= kMaximumWifiPasswordBytes && s.find('\0') == std::string::npos;
}
bool ValidRun(const std::string& s) {
    return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool ExistingDirectory(const std::string& path) {
    if (path.empty() || path.size() > 512 || path.find('\0') != std::string::npos ||
        path.find('\n') != std::string::npos || path.find('\r') != std::string::npos) return false;
    struct stat info{};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}
std::string WithoutTrailingSlash(std::string path) {
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    return path;
}
void Remove(std::vector<WifiNetwork>* list, const std::string& ssid) {
    list->erase(std::remove_if(list->begin(), list->end(), [&](const auto& n) { return n.ssid == ssid; }), list->end());
}
void Prefer(std::vector<WifiNetwork>* list, const WifiNetwork& item) {
    Remove(list, item.ssid);
    if (list->size() >= kMaximumApplicationWifiNetworks) list->erase(list->begin());
    list->push_back(item);
}
std::string EncodeNetworks(const std::vector<WifiNetwork>& entries) {
    std::string bytes(kPrivateListMagic.begin(), kPrivateListMagic.end());
    bytes.push_back(static_cast<char>(entries.size()));
    for (const auto& n : entries) {
        bytes.push_back(static_cast<char>(n.ssid.size()));
        bytes.push_back(static_cast<char>(n.password.size()));
        bytes += n.ssid; bytes += n.password;
    }
    return bytes;
}
bool DecodeNetworks(const std::string& bytes, std::vector<WifiNetwork>* out) {
    if (bytes.size() < 9 || !std::equal(kPrivateListMagic.begin(), kPrivateListMagic.end(), bytes.begin())) return false;
    const auto count = static_cast<unsigned char>(bytes[8]);
    if (count > kMaximumApplicationWifiNetworks) return false;
    std::size_t pos = 9;
    std::vector<WifiNetwork> entries;
    for (std::size_t i = 0; i < count; ++i) {
        if (bytes.size() - pos < 2) return false;
        const auto ssid_length = static_cast<unsigned char>(bytes[pos++]);
        const auto password_length = static_cast<unsigned char>(bytes[pos++]);
        if (static_cast<std::size_t>(ssid_length) + password_length > bytes.size() - pos) return false;
        WifiNetwork item{bytes.substr(pos, ssid_length), bytes.substr(pos + ssid_length, password_length)};
        pos += ssid_length + password_length;
        if (!ValidSsid(item.ssid) || !ValidPassword(item.password) ||
            std::any_of(entries.begin(), entries.end(), [&](const auto& n) { return n.ssid == item.ssid; })) return false;
        entries.push_back(std::move(item));
    }
    if (pos != bytes.size()) return false;
    *out = std::move(entries);
    return true;
}
std::string EncodePrivate(const PrivateState& state) {
    std::string bytes(kPrivateMagic.begin(), kPrivateMagic.end());
    bytes.push_back(static_cast<char>(state.manual_run.size())); bytes += state.manual_run;
    bytes.push_back(static_cast<char>(state.manual_ssid.size())); bytes += state.manual_ssid;
    bytes.push_back(state.pending ? 1 : 0);
    bytes += EncodeNetworks(state.networks);
    return bytes;
}
bool ReadField(const std::string& bytes, std::size_t* pos, std::string* out) {
    if (*pos >= bytes.size()) return false;
    const auto size = static_cast<unsigned char>(bytes[(*pos)++]);
    if (size > bytes.size() - *pos) return false;
    *out = bytes.substr(*pos, size); *pos += size;
    return true;
}
bool DecodePrivate(const std::string& bytes, PrivateState* out) {
    if (bytes.size() < 11 || !std::equal(kPrivateMagic.begin(), kPrivateMagic.end(), bytes.begin())) return false;
    PrivateState state;
    std::size_t pos = 8;
    if (!ReadField(bytes, &pos, &state.manual_run) || !ReadField(bytes, &pos, &state.manual_ssid) ||
        pos >= bytes.size()) return false;
    const auto pending = static_cast<unsigned char>(bytes[pos++]);
    if (pending > 1 || !DecodeNetworks(bytes.substr(pos), &state.networks)) return false;
    state.pending = pending == 1;
    if (state.manual_run.empty()) {
        if (!state.manual_ssid.empty() || state.pending) return false;
    } else if (!ValidRun(state.manual_run) || !ValidSsid(state.manual_ssid) ||
               std::none_of(state.networks.begin(), state.networks.end(), [&](const auto& n) {
                   return n.ssid == state.manual_ssid;
               })) return false;
    *out = std::move(state);
    return true;
}
bool WritePrivate(const PrivateState& state, std::string* error) {
    if (!writable || !ExistingDirectory(private_root)) return Fail(error, "Application Wi-Fi storage is unavailable");
    const auto payload = EncodePrivate(state);
    const RecordStore store(private_root);
    if (!store.Save(kPrivateKey, 1, payload).ok()) {
        writable = false;
        return Fail(error, "Cannot save application Wi-Fi; reinitialize storage before retrying");
    }
    const auto verified = store.Load(kPrivateKey, 1);
    if (!verified.ok() || verified.payload != payload) {
        writable = false;
        return Fail(error, "Application Wi-Fi readback failed; saved state is uncertain");
    }
    local = state;
    return Success(error);
}
}  // namespace

bool InitializeApplicationWifi(const std::string& shared_directory, const std::string& private_directory,
                               bool integrated, const std::string& run_id, std::string* error) {
    std::lock_guard<std::mutex> lock(wifi_mutex);
    initialized = false; writable = false; integrated_run = false;
    private_root.clear(); current_run.clear(); local = {}; cached.clear();
    if ((integrated && !ValidRun(run_id)) || (!integrated && !run_id.empty()))
        return Fail(error, "Invalid application Wi-Fi run context");
    initialized = true;
    integrated_run = integrated;
    current_run = run_id;
    private_root = private_directory;
    bool local_ok = ExistingDirectory(private_root) &&
        (!integrated || WithoutTrailingSlash(shared_directory) != WithoutTrailingSlash(private_root));
    if (local_ok) {
        const auto record = RecordStore(private_root).Load(kPrivateKey, 1);
        local_ok = record.status == RecordStatus::Missing || (record.ok() && DecodePrivate(record.payload, &local));
    }
    writable = local_ok;
    cached = local.networks;
    // Guide 1.1 shared/wifi_current: exactly two length bytes then raw SSID/password.
    // Host multi-network lists are private extensions and are never read here.
    // Read-only import neither persists private data nor creates a candidate.
    if (integrated && ExistingDirectory(shared_directory)) {
        const auto record = RecordStore(shared_directory).Load("wifi_current", 1);
        if (record.ok() && record.payload.size() >= 2) {
            const auto ssid_length = static_cast<unsigned char>(record.payload[0]);
            const auto password_length = static_cast<unsigned char>(record.payload[1]);
            if (record.payload.size() == 2u + ssid_length + password_length) {
                WifiNetwork item{record.payload.substr(2, ssid_length),
                                 record.payload.substr(2 + ssid_length, password_length)};
                if (ValidSsid(item.ssid) && ValidPassword(item.password)) Prefer(&cached, item);
            }
        }
    }
    // A manual choice made during this very run survives an application reboot.
    // Another run's saved private password cannot supersede current public data.
    if (integrated && local.manual_run == current_run) {
        const auto found = std::find_if(local.networks.begin(), local.networks.end(), [&](const auto& n) {
            return n.ssid == local.manual_ssid;
        });
        if (found != local.networks.end()) Prefer(&cached, *found);
    }
    if (!local_ok) return Fail(error, "Application Wi-Fi storage is unavailable or invalid");
    return Success(error);
}

void DisableApplicationWifiStorage() {
    std::lock_guard<std::mutex> lock(wifi_mutex);
    initialized = false; writable = false; integrated_run = false;
    private_root.clear(); current_run.clear(); local = {}; cached.clear();
}

bool ReadApplicationWifi(std::vector<WifiNetwork>* networks, std::string* error) {
    if (!networks) return Fail(error, "Wi-Fi output list is required");
    std::lock_guard<std::mutex> lock(wifi_mutex);
    if (!initialized) return Fail(error, "Application Wi-Fi has not been initialized");
    *networks = cached;
    return Success(error);
}

bool SaveApplicationWifi(const std::string& ssid, const std::string& password, std::string* error) {
    if (!ValidSsid(ssid) || !ValidPassword(password)) return Fail(error, "Invalid Wi-Fi SSID or password length/NUL");
    std::lock_guard<std::mutex> lock(wifi_mutex);
    PrivateState next = local;
    const auto found = std::find_if(next.networks.begin(), next.networks.end(), [&](const auto& n) { return n.ssid == ssid; });
    if (found == next.networks.end() && next.networks.size() >= kMaximumApplicationWifiNetworks)
        return Fail(error, "Application Wi-Fi list is full; forget a private network before adding one");
    Prefer(&next.networks, {ssid, password});
    next.manual_run = integrated_run ? current_run : "";
    next.manual_ssid = integrated_run ? ssid : "";
    next.pending = integrated_run;
    if (!WritePrivate(next, error)) return false;
    Prefer(&cached, {ssid, password});
    return Success(error);
}

bool ForgetApplicationWifi(const std::string& ssid, std::string* error) {
    if (!ValidSsid(ssid)) return Fail(error, "Invalid Wi-Fi SSID length/NUL");
    std::lock_guard<std::mutex> lock(wifi_mutex);
    PrivateState next = local;
    Remove(&next.networks, ssid);
    if (next.manual_ssid == ssid) { next.manual_run.clear(); next.manual_ssid.clear(); next.pending = false; }
    if (!WritePrivate(next, error)) return false;
    Remove(&cached, ssid);
    return Success(error);
}

bool ReadPendingWifiOffer(WifiNetwork* network, bool* available, std::string* error) {
    if (!network || !available) return Fail(error, "Wi-Fi offer outputs are required");
    std::lock_guard<std::mutex> lock(wifi_mutex);
    *available = false; *network = {};
    if (!initialized) return Fail(error, "Application Wi-Fi has not been initialized");
    if (!integrated_run || !local.pending || local.manual_run != current_run) return Success(error);
    if (!writable) return Fail(error, "Application Wi-Fi saved state is uncertain");
    const auto found = std::find_if(local.networks.begin(), local.networks.end(), [&](const auto& n) {
        return n.ssid == local.manual_ssid;
    });
    if (found == local.networks.end()) return Fail(error, "Application Wi-Fi offer is incomplete");
    *network = *found; *available = true;
    return Success(error);
}

bool ClearPendingWifiOffer(std::string* error) {
    std::lock_guard<std::mutex> lock(wifi_mutex);
    if (!initialized) return Fail(error, "Application Wi-Fi has not been initialized");
    if (!integrated_run || !local.pending || local.manual_run != current_run) return Success(error);
    PrivateState next = local; next.pending = false;
    return WritePrivate(next, error);
}
}  // namespace pocket

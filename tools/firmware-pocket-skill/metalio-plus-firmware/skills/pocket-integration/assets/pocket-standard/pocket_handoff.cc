#include "pocket_handoff.h"
#include <algorithm>
#include <limits>
#include <map>
namespace pocket {
namespace {
bool Fail(std::string *e, const std::string &m) {
    if (e)
        *e = m;
    return false;
}
bool Hex(const std::string &s, size_t n) {
    return s.size() == n && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
bool Number(const std::string &s, std::uint64_t *v) {
    if (s.empty() || s[0] == '0')
        return false;
    std::uint64_t n = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        auto d = unsigned(c - '0');
        if (n > (std::numeric_limits<std::uint64_t>::max() - d) / 10)
            return false;
        n = n * 10 + d;
    }
    *v = n;
    return true;
}
// All defined control keys/values have ASCII-only grammars. Decode escapes
// before validation, including escaped keys; no BOM, types, duplicates or tail.
class Json {
    const std::string &s;
    size_t at = 0;
    void Space() {
        while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n'))
            ++at;
    }
    bool Take(char c) {
        Space();
        if (at == s.size() || s[at] != c)
            return false;
        ++at;
        return true;
    }
    bool String(std::string *out) {
        if (!Take('"'))
            return false;
        std::string v;
        while (at < s.size()) {
            unsigned char c = s[at++];
            if (c == '"') {
                *out = std::move(v);
                return true;
            }
            if (c < 0x20 || c >= 0x80)
                return false;
            if (c == '\\') {
                if (at == s.size())
                    return false;
                c = s[at++];
                if (c == 'u') {
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        if (at == s.size())
                            return false;
                        char h = s[at++];
                        int n = h >= '0' && h <= '9'   ? h - '0'
                                : h >= 'a' && h <= 'f' ? h - 'a' + 10
                                : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                                       : -1;
                        if (n < 0)
                            return false;
                        code = code * 16 + unsigned(n);
                    }
                    if (code < 0x20 || code >= 0x7f)
                        return false;
                    c = static_cast<unsigned char>(code);
                } else if (c != '"' && c != '\\' && c != '/')
                    return false;
            }
            v += static_cast<char>(c);
        }
        return false;
    }

  public:
    explicit Json(const std::string &text) : s(text) {}
    bool Parse(std::initializer_list<const char *> fields, std::map<std::string, std::string> *out) {
        if (s.empty() || s.size() > kMaximumRecordBytes || !Take('{'))
            return false;
        std::map<std::string, std::string> f;
        for (;;) {
            std::string k, v;
            if (!String(&k) || !Take(':') || !String(&v) || !f.emplace(k, v).second)
                return false;
            Space();
            if (at < s.size() && s[at] == '}') {
                ++at;
                break;
            }
            if (!Take(','))
                return false;
        }
        Space();
        if (at != s.size() || f.size() != fields.size())
            return false;
        for (auto *key : fields)
            if (!f.count(key))
                return false;
        *out = std::move(f);
        return true;
    }
};
bool Valid(const RunContext &r) {
    return ValidRunId(r.run_id) && IsSafeIdentifier(r.user_id) && IsAppIdentifier(r.app_id) &&
           r.app_id != "pocket_manager" && IsAppVersion(r.version) && Hex(r.image_sha256, 64) &&
           (r.phase == "prepared" || r.phase == "active" || r.phase == "closed");
}
std::string Receipt(const RunContext &r, bool saved) {
    return "{\"run_id\":\"" + r.run_id + "\",\"user_id\":\"" + r.user_id + "\",\"app_id\":\"" + r.app_id +
           "\",\"phase\":\"" + (saved ? "saved" : "started") + "\"}";
}
bool SavedReceipt(const std::string &payload, const RunContext &r) {
    std::map<std::string, std::string> f;
    return Json(payload).Parse({"run_id", "user_id", "app_id", "phase"}, &f) && f["run_id"] == r.run_id &&
           f["user_id"] == r.user_id && f["app_id"] == r.app_id && f["phase"] == "saved";
}
bool WifiValid(const WifiOffer &o) {
    return ValidRunId(o.run_id) && !o.ssid.empty() && o.ssid.size() <= 32 && o.password.size() <= 64 &&
           o.ssid.find('\0') == std::string::npos && o.password.find('\0') == std::string::npos;
}
bool ResultValid(const std::string &r) {
    return r == "processing" || r == "accepted" || r == "failed" || r == "duplicate" || r == "cancelled";
}
} // namespace
bool ValidRunId(const std::string &s) { return Hex(s, 32); }
bool DecodeIdentity(const std::string &payload, std::string *output) {
    if (!output)
        return false;
    auto pos = payload.find('\n');
    if (pos == std::string::npos)
        return false;
    auto id = payload.substr(0, pos), suffix = payload.substr(pos);
    if (!IsSafeIdentifier(id) || (suffix != "\n" && suffix != "\n0\n" && suffix != "\n1\n"))
        return false;
    *output = id;
    return true;
}
std::string EncodeRun(const RunContext &r) {
    if (!Valid(r))
        return {};
    return "{\"run_id\":\"" + r.run_id + "\",\"user_id\":\"" + r.user_id + "\",\"app_id\":\"" + r.app_id +
           "\",\"version\":\"" + r.version + "\",\"image_sha256\":\"" + r.image_sha256 + "\",\"phase\":\"" +
           r.phase + "\"}";
}
bool DecodeRun(const std::string &payload, RunContext *output) {
    if (!output)
        return false;
    std::map<std::string, std::string> f;
    if (!Json(payload).Parse({"run_id", "user_id", "app_id", "version", "image_sha256", "phase"}, &f))
        return false;
    RunContext r;
    r.run_id = f["run_id"];
    r.user_id = f["user_id"];
    r.app_id = f["app_id"];
    r.version = f["version"];
    r.image_sha256 = f["image_sha256"];
    r.phase = f["phase"];
    if (!Valid(r))
        return false;
    *output = std::move(r);
    return true;
}
bool SaveRun(const std::string &dir, const RunContext &r, std::string *e) {
    auto payload = EncodeRun(r);
    if (payload.empty())
        return Fail(e, "invalid run");
    auto saved = RecordStore(dir).Save("run", 1, payload);
    if (!saved.ok())
        return Fail(e, saved.error);
    if (e)
        e->clear();
    return true;
}
HandoffView InspectHandoff(const std::string &dir, const std::string &user) {
    HandoffView v;
    auto record = RecordStore(dir).Load("run", 1);
    if (record.status == RecordStatus::Missing) {
        v.state = HandoffState::Empty;
        return v;
    }
    if (!record.ok() || record.schema != 1 || !DecodeRun(record.payload, &v.run) || v.run.user_id != user) {
        v.error = "invalid run; preserve application state";
        return v;
    }
    if (v.run.phase == "prepared") {
        v.state = HandoffState::Prepared;
        return v;
    }
    if (v.run.phase == "closed") {
        v.state = HandoffState::Closed;
        return v;
    }
    auto receipt = RecordStore(dir).Load("receipt", 1);
    if (receipt.ok() && receipt.schema == 1 && SavedReceipt(receipt.payload, v.run)) {
        v.state = HandoffState::Returned;
        return v;
    }
    v.state = HandoffState::Unfinished;
    v.error = "no matching saved receipt";
    return v;
}
bool WriteHandoffReceipt(const std::string &dir, const RunContext &r, bool saved, std::string *e) {
    if (!Valid(r) || r.phase != "active")
        return Fail(e, "application context is not active");
    auto actual = RecordStore(dir).Load("run", 1);
    RunContext current;
    if (!actual.ok() || actual.schema != 1 || !DecodeRun(actual.payload, &current) ||
        EncodeRun(current) != EncodeRun(r))
        return Fail(e, "run identity changed or cannot be read");
    auto result = RecordStore(dir).Save("receipt", 1, Receipt(r, saved));
    if (!result.ok())
        return Fail(e, result.error);
    if (e)
        e->clear();
    return true;
}
bool MayResumeOwnFlash(const RunContext &r, const std::string &run, const std::string &user,
                       const std::string &app) {
    return Valid(r) && r.phase == "active" && r.run_id == run && r.user_id == user && r.app_id == app;
}
std::string EncodeWifiOffer(const WifiOffer &o) {
    if (!WifiValid(o))
        return {};
    std::string out;
    out.reserve(114);
    auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i < 32; i += 2)
        out.push_back(static_cast<char>((digit(o.run_id[i]) << 4) | digit(o.run_id[i + 1])));
    out.push_back(static_cast<char>(o.ssid.size()));
    out.push_back(static_cast<char>(o.password.size()));
    return out + o.ssid + o.password;
}
bool DecodeWifiOffer(const RecordResult &r, WifiOffer *out) {
    if (!out || !r.ok() || r.schema != 1 || !r.generation || r.payload.size() < 19)
        return false;
    auto sn = static_cast<unsigned char>(r.payload[16]), pn = static_cast<unsigned char>(r.payload[17]);
    if (r.payload.size() != 18U + sn + pn)
        return false;
    WifiOffer o;
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) {
        auto b = static_cast<unsigned char>(r.payload[i]);
        o.run_id += hex[b >> 4];
        o.run_id += hex[b & 15];
    }
    o.ssid = r.payload.substr(18, sn);
    o.password = r.payload.substr(18 + sn, pn);
    o.generation = r.generation;
    if (!WifiValid(o))
        return false;
    *out = std::move(o);
    return true;
}
bool WriteWifiOffer(const std::string &dir, const WifiOffer &o, std::string *e) {
    auto payload = EncodeWifiOffer(o);
    if (payload.empty())
        return Fail(e, "invalid Wi-Fi candidate");
    auto r = RecordStore(dir).Save("wifi_offer", 1, payload);
    if (!r.ok())
        return Fail(e, r.error);
    if (e)
        e->clear();
    return true;
}
std::string WifiResultPayload(const WifiOffer &o, const std::string &result) {
    if (!ValidRunId(o.run_id) || !o.generation || !ResultValid(result))
        return {};
    return "{\"run_id\":\"" + o.run_id + "\",\"offer_generation\":\"" + std::to_string(o.generation) +
           "\",\"result\":\"" + result + "\"}";
}
bool ReadWifiResult(const std::string &dir, const WifiOffer &o, std::string *result, std::string *e) {
    if (!result || !ValidRunId(o.run_id) || !o.generation)
        return Fail(e, "invalid result arguments");
    auto r = RecordStore(dir).Load("wifi_result", 1);
    if (r.status == RecordStatus::Missing) {
        result->clear();
        if (e)
            e->clear();
        return true;
    }
    if (!r.ok())
        return Fail(e, r.error);
    std::map<std::string, std::string> f;
    std::uint64_t generation = 0;
    if (r.schema != 1 || !Json(r.payload).Parse({"run_id", "offer_generation", "result"}, &f) ||
        !ValidRunId(f["run_id"]) || !Number(f["offer_generation"], &generation) || !ResultValid(f["result"]))
        return Fail(e, "invalid candidate result");
    *result = f["run_id"] == o.run_id && generation == o.generation ? f["result"] : "";
    if (e)
        e->clear();
    return true;
}
bool WriteWifiResult(const std::string &dir, const WifiOffer &o, const std::string &value, std::string *e) {
    auto payload = WifiResultPayload(o, value);
    if (payload.empty())
        return Fail(e, "invalid candidate result");
    auto r = RecordStore(dir).Save("wifi_result", 1, payload);
    if (!r.ok())
        return Fail(e, r.error);
    if (e)
        e->clear();
    return true;
}
} // namespace pocket

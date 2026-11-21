#pragma once
#include "pocket_core.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace plus_services {
inline bool ValidText(const std::string& value, std::size_t maximum) {
    if (value.size() > maximum) return false;
    for (std::size_t i = 0; i < value.size();) {
        const auto c = static_cast<unsigned char>(value[i++]);
        if (c < 0x80) {
            if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7f) return false;
            continue;
        }
        unsigned count = 0, cp = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; cp = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; cp = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; cp = c & 7; minimum = 0x10000; }
        else return false;
        if (count > value.size() - i) return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            (cp >= 0x80 && cp <= 0x9f)) return false;
    }
    return true;
}
// One serialized storage worker; existing trusted directory, already mounted.
// Two .pkr files are the authority, NOT a plain TXT. Optional TXT export must be
// explicit. A new instance has to Load successfully before it can Save.
class TextStore {
public:
    TextStore(std::string directory, std::string key, std::size_t maximum = 3072)
        : store_(std::move(directory)), key_(std::move(key)), maximum_(maximum) {}
    bool Load(std::string* output, std::string* error) {
        ready_ = false;
        if (!output || maximum_ > 4000 || !pocket::IsSafeIdentifier(key_)) return Fail(error, "TEXT_ARGUMENT");
        const auto record = store_.Load(key_, 1);
        if (record.status == pocket::RecordStatus::Missing) {
            saved_.clear(); generation_ = 0; missing_ = true;
        } else if (record.ok() && ValidText(record.payload, maximum_)) {
            saved_ = record.payload; generation_ = record.generation; missing_ = false;
        } else return Fail(error, "TEXT_LOAD_FAILED_KEEP_FILES");
        ready_ = true;
        *output = saved_;
        if (error) error->clear();
        return true;
    }
    bool Save(const std::string& draft, std::string* error) {
        if (!ready_ || !ValidText(draft, maximum_)) return Fail(error, "TEXT_NOT_READY_OR_INVALID");
        const auto prior = store_.Load(key_, 1);
        if ((missing_ && prior.status != pocket::RecordStatus::Missing) ||
            (!missing_ && (!prior.ok() || prior.generation != generation_ || prior.payload != saved_))) {
            ready_ = false;
            return Fail(error, "TEXT_CHANGED_RELOAD_EXPLICITLY");
        }
        const auto written = store_.Save(key_, 1, draft);
        const auto verified = store_.Load(key_, 1);
        if (!written.ok() || !verified.ok() || verified.payload != draft ||
            verified.generation != written.generation) {
            ready_ = false;
            return Fail(error, "TEXT_SAVE_UNCERTAIN_KEEP_DRAFT");
        }
        saved_ = draft; generation_ = verified.generation; missing_ = false;
        if (error) error->clear();
        return true;
    }
    const std::string& Saved() const { return saved_; }
    bool Exists() const { return ready_ && !missing_; }
private:
    static bool Fail(std::string* error, const char* text) { if (error) *error = text; return false; }
    pocket::RecordStore store_;
    std::string key_, saved_;
    std::size_t maximum_;
    std::uint64_t generation_ = 0;
    bool ready_ = false, missing_ = true;
};
} // namespace plus_services


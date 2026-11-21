#pragma once
#include "durable_text.h"
#include <limits>
namespace plus_services {
// Small, app-owned TF state example. Storage choice is explicit: this is
// DIRECT_TF, not a fake Flash CHECKPOINT adapter. Use a stable app business root
// for shared standalone/integrated state; never use run/receipt as the payload.
class CounterState {
public:
    explicit CounterState(std::string directory) : store_(std::move(directory), "counter", 64) {}
    bool Load(std::string* error) {
        ready_ = false;
        std::string text;
        if (!store_.Load(&text, error)) return false;
        if (!store_.Exists()) { value_ = 0; ready_ = true; return true; }
        const std::string prefix = "COUNTER1\n";
        if (text.compare(0, prefix.size(), prefix) != 0 || text.back() != '\n')
            return Fail(error);
        std::uint64_t n = 0;
        const auto digits = text.substr(prefix.size(), text.size() - prefix.size() - 1);
        if (digits.empty() || (digits.size() > 1 && digits[0] == '0')) return Fail(error);
        for (char c : digits) {
            if (c < '0' || c > '9' || n > (UINT64_MAX - static_cast<unsigned>(c - '0')) / 10)
                return Fail(error);
            n = n * 10 + static_cast<unsigned>(c - '0');
        }
        value_ = n; ready_ = true; return true;
    }
    bool Save(std::uint64_t next, std::string* error) {
        if (!ready_) return Fail(error);
        if (!store_.Save("COUNTER1\n" + std::to_string(next) + "\n", error)) return false;
        value_ = next;
        return true;
    }
    std::uint64_t Value() const { return value_; }
private:
    static bool Fail(std::string* error) { if (error) *error = "COUNTER_INVALID_KEEP_STATE"; return false; }
    TextStore store_;
    std::uint64_t value_ = 0;
    bool ready_ = false;
};
}


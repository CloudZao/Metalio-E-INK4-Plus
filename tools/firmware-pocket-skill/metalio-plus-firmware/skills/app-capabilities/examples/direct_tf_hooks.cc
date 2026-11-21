// Compilable DIRECT_TF small-text hook example. This does NOT implement a
// notepad UI or force every application to use TF. Attach Hooks() to the bridge
// instead of the NONE probe's hooks when this storage mode is selected.
#include "durable_text.h"
#include "plus_pocket_bridge.h"
#include "plus_hal.h"
#include "pocket_build.h"
#include <cerrno>
#include <memory>
#include <string>
#include <sys/stat.h>

namespace plus_text_example {
namespace {
std::unique_ptr<plus_services::TextStore> store;
std::string draft;
constexpr char kBusinessRoot[] = "/sdcard/" POCKET_APP_ID; // stable per-app root, never selected by boot mode
bool Restore(const pocket::RunContext*, std::string* error) {
    if (!plus_hal::TfMounted()) {
        if (error) *error = "TF_NOT_MOUNTED";
        return false;
    }
    // Only our own explicit shallow app directory is created. Never identity,
    // public WiFi or run/receipt. The bridge has already checked integrated identity.
    struct stat info{};
    if (stat(kBusinessRoot, &info) != 0) {
        if (errno != ENOENT || mkdir(kBusinessRoot, 0700) != 0) {
            if (error) *error = "APP_DIRECTORY_UNAVAILABLE";
            return false;
        }
    } else if (!S_ISDIR(info.st_mode)) {
        if (error) *error = "APP_DIRECTORY_WRONG_TYPE";
        return false;
    }
    store.reset(new plus_services::TextStore(kBusinessRoot, "note", 3072));
    return store->Load(&draft, error);
}
bool Save(plus_pocket::SaveReason, const pocket::RunContext*, std::string* error) {
    if (!store || !plus_hal::TfMounted()) {
        if (error) *error = "STORAGE_NOT_READY";
        return false;
    }
    return store->Save(draft, error); // confirmed durable readback precedes saved receipt
}
}
plus_pocket::Hooks Hooks() { return {Restore, Save}; }
const std::string& Draft() { return draft; }
// Single application controller only. Check bridge CanEdit() too at caller.
bool ReplaceDraft(std::string next) {
    if (!plus_pocket::CanEdit() || !plus_services::ValidText(next, 3072)) return false;
    draft = std::move(next);
    return true;
}
} // namespace plus_text_example


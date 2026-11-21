#pragma once
#include "pocket/pocket_handoff.h"
#include <cstdint>
#include <string>
// Call on one application task. HAL is initialized first. Callbacks must not
// recursively call this bridge. They own BUSINESS state, never identity/run.
namespace plus_pocket {
enum class Error {
    None,
    InvalidArgument,
    Unsupported,
    Unavailable,
    Corrupt,
    IoError,
    SaveFailed,
    ReturnFailed
};
Error LastError();
const char *ErrorMessage(Error error);
enum class SaveReason { Checkpoint, Exit };
struct Hooks {
    // nullptr run = standalone; active run = integrated. CHECKPOINT must follow
    // the documented same-run/new-run restore policy. Return false on failure.
    bool (*restore)(const pocket::RunContext *run, std::string *error) = nullptr;
    bool (*save)(SaveReason reason, const pocket::RunContext *run, std::string *error) = nullptr;
};
bool Initialize(const Hooks &hooks, std::string *error);
// Call after the first successful physical business frame and restore success.
// Performs system OTA pending-verify acknowledgement BEFORE started receipt.
bool ConfirmStartup(uint32_t first_frame, std::string *error);
bool Save(std::string *error);
// Integrated: save, saved receipt, revalidate manager, switch and restart.
// Standalone: save and return true while remaining in this application.
bool ReturnToPocket(std::string *error);
bool Integrated();
bool CanReturn(); // verified integrated target; ReturnToPocket rechecks it
bool CanEdit();   // false after an uncertain saved/return transition
} // namespace plus_pocket

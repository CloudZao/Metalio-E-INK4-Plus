#pragma once
#include "pocket_core.h"
#include <string>

namespace pocket {
// Physical binding is metalio16m-plus-pocket2: manager OTA0 0x10000,
// app OTA1 0x300000, full 13 MiB; standalone factory app at file 0x10000.
// These records are control data, never a copy of an application's business data.
struct RunContext {
    std::string run_id, user_id, app_id, version, image_sha256;
    // Platform-derived TF path, not a serialized run field.
    std::string app_dir;
    // Derived from verified running image, never present in standard JSON.
    std::uint32_t image_bytes = 0, state_schema = 0;
    std::uint8_t storage_mode = 0;
    std::string phase; // prepared, active, closed
};
enum class HandoffState { Empty, Closed, Prepared, Returned, Unfinished, Invalid };
struct HandoffView {
    HandoffState state = HandoffState::Invalid;
    RunContext run;
    std::string error;
    bool may_replace() const {
        // Closed/prepared require separate verified recovery evidence.
        return state == HandoffState::Empty || state == HandoffState::Returned;
    }
};
bool ValidRunId(const std::string &text);
bool DecodeIdentity(const std::string &payload, std::string *output);
std::string EncodeRun(const RunContext &run);
bool DecodeRun(const std::string &payload, RunContext *output);
// Manager/test helper only; application bridge never writes run.
bool SaveRun(const std::string &status_dir, const RunContext &run, std::string *error);
HandoffView InspectHandoff(const std::string &status_dir, const std::string &user_id);
// App adapter calls started BEFORE permitting new business changes; after its
// own save/flush/readback it calls saved. Failed return must remain paused or
// successfully call started again before resuming. No raw NVS backup is done.
bool WriteHandoffReceipt(const std::string &status_dir, const RunContext &run, bool saved,
                         std::string *error);
// A per-app local Flash marker may hold the last run_id. Same run means resume
// OWN validated Flash; a new run means restore the TF checkpoint. A marker is
// never restored from a business checkpoint and is not a public Wi-Fi value.
bool MayResumeOwnFlash(const RunContext &run, const std::string &local_run_id,
                       const std::string &local_user_id, const std::string &local_app_id);

struct WifiOffer {
    std::string run_id, ssid, password;
    std::uint64_t generation = 0;
};
bool DecodeWifiOffer(const RecordResult &record, WifiOffer *output);
std::string EncodeWifiOffer(const WifiOffer &offer);
bool WriteWifiOffer(const std::string &app_protocol_dir, const WifiOffer &offer, std::string *error);
// The candidate result is matched to the run and offer generation; values are
// processing/accepted/failed/duplicate/cancelled. Payload contains no password.
std::string WifiResultPayload(const WifiOffer &offer, const std::string &result);
bool ReadWifiResult(const std::string &status_dir, const WifiOffer &offer, std::string *result,
                    std::string *error);
bool WriteWifiResult(const std::string &status_dir, const WifiOffer &offer, const std::string &result,
                     std::string *error);
} // namespace pocket

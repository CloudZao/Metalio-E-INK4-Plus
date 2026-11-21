// Reuse synthetic, cryptographically sealed ESP images; execute real bridge.
#define POCKET_TEST_FIXTURES_ONLY
#include "plus_pocket_bridge.h"
#include "pocket/pocket_boot_layout.h"
#include "test_standard.cpp"
#include <esp_ota_ops.h>
#include <ff.h>
namespace {
std::vector<esp_partition_t> parts;
std::vector<unsigned char> app_image = Image(), manager_image = Image(true);
bool mounted = true, pause_ok = true, healthy = true, read_ok = true, save_ok = true, restore_ok = true,
     boot_ok = true, ack_ok = true;
int saves = 0, restores = 0, restarts = 0, acks = 0, leases = 0;
bool callback_lease_violation = false;
unsigned frame = 1;
int ota_state = ESP_OTA_IMG_PENDING_VERIFY;
pocket::RunContext callback_run;
void Layout(bool integrated) {
    using namespace pocket;
    parts = {{1, 2, kNvsAddress, kNvsSize, "nvs"},
             {1, 0, kOtaDataAddress, kOtaDataSize, "otadata"},
             {1, 1, kPhyAddress, kPhySize, "phy_init"}};
    if (integrated) {
        parts.push_back({0, 16, kManagerAddress, kManagerSize, "ota_0"});
        parts.push_back({0, 17, kAppAddress, kAppSize, "ota_1"});
    } else
        parts.push_back({0, 0, kCompactStandaloneOffset, kAppSize, "factory"});
}
bool Restore(const pocket::RunContext *r, std::string *error) {
    ++restores;
    if (leases)
        callback_lease_violation = true;
    if (r)
        callback_run = *r;
    if (!restore_ok) {
        *error = "business restore failed";
        return false;
    }
    return true;
}
bool Save(plus_pocket::SaveReason, const pocket::RunContext *, std::string *error) {
    ++saves;
    if (leases)
        callback_lease_violation = true;
    if (!save_ok) {
        *error = "business save failed";
        return false;
    }
    return true;
}
} // namespace
namespace plus_hal {
bool PauseForFlash(unsigned) {
    if (!pause_ok)
        return false;
    ++leases;
    return true;
}
void ResumeAfterFlash() { --leases; }
bool DisplayHealthy() { return healthy; }
uint32_t DisplayedRevision() { return frame; }
} // namespace plus_hal
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *name) {
    for (const auto &p : parts)
        if (p.type == type && p.subtype == subtype && (!name || !std::strcmp(name, p.label)))
            return &p;
    return nullptr;
}
const esp_partition_t *esp_ota_get_running_partition() { return parts.empty() ? nullptr : &parts.back(); }
int esp_partition_read(const esp_partition_t *p, uint32_t offset, void *out, size_t n) {
    auto &bytes = p->subtype == 16 ? manager_image : app_image;
    return read_ok && Reader(&bytes, offset, out, n) ? ESP_OK : -1;
}
int esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *s) {
    *s = ota_state;
    return ESP_OK;
}
int esp_ota_mark_app_valid_cancel_rollback() {
    ++acks;
    return ack_ok ? ESP_OK : -1;
}
int esp_ota_set_boot_partition(const esp_partition_t *p) {
    auto receipt = pocket::RecordStore("fixture-bridge/firmware-pocket/status").Load("receipt", 1);
    if (!receipt.ok() || receipt.payload.find("\"saved\"") == std::string::npos)
        return -1;
    return boot_ok && p->subtype == 16 ? ESP_OK : -1;
}
void esp_restart() { ++restarts; }
int f_getfree(const char *, DWORD *, FATFS **fs) {
    static FATFS object;
    if (!mounted)
        return -1;
    *fs = &object;
    return FR_OK;
}
int main() {
    using namespace plus_pocket;
    using namespace pocket;
    std::string error;
    Hooks hooks{Restore, Save};
    Layout(false);
    CHECK(Initialize(hooks, &error));
    CHECK(!CanEdit() && !Integrated());
    CHECK(restores == 1);
    frame = 0;
    CHECK(!ConfirmStartup(1, &error));
    CHECK(!CanEdit());
    frame = 1;
    CHECK(ConfirmStartup(1, &error));
    CHECK(CanEdit());
    save_ok = false;
    CHECK(!ReturnToPocket(&error));
    CHECK(LastError() == Error::SaveFailed);
    save_ok = true;
    CHECK(ReturnToPocket(&error));
    CHECK(restarts == 0);
    CHECK(!fs::exists("fixture-bridge"));
    pause_ok = false;
    CHECK(!Initialize(hooks, &error));
    CHECK(LastError() == Error::Unavailable);
    pause_ok = true;
    parts.back().address = 0x12345;
    CHECK(!Initialize(hooks, &error));
    CHECK(LastError() == Error::Unsupported);
    Layout(false);
    read_ok = false;
    CHECK(!Initialize(hooks, &error));
    CHECK(LastError() == Error::IoError);
    read_ok = true;
    app_image[847] ^= 1;
    CHECK(!Initialize(hooks, &error));
    CHECK(LastError() == Error::Corrupt);
    app_image = Image();
    restore_ok = false;
    CHECK(!Initialize(hooks, &error));
    CHECK(LastError() == Error::Corrupt);
    restore_ok = true;
    Layout(true);
    mounted = false;
    CHECK(!Initialize(hooks, &error));
    CHECK(LastError() == Error::Unavailable);
    mounted = true;
    fs::create_directories("fixture-bridge/firmware-pocket/shared");
    fs::create_directories("fixture-bridge/firmware-pocket/status");
    RecordStore shared("fixture-bridge/firmware-pocket/shared"),
        status("fixture-bridge/firmware-pocket/status");
    CHECK(shared.Save("identity", 1, "legacy.user\n1\n").ok());
    CHECK(!Initialize(hooks, &error));
    CHECK(LastError() == Error::Unavailable);
    CompactImageInfo info;
    CHECK(ValidateRunningImage(Reader, &app_image, kAppSize, &info, &error));
    RunContext run;
    run.run_id = "0123456789abcdef0123456789abcdef";
    run.user_id = "legacy.user";
    run.app_id = "test_game";
    run.version = "9.8.7";
    run.image_sha256 = info.manifest.sha256;
    run.phase = "prepared";
    CHECK(SaveRun("fixture-bridge/firmware-pocket/status", run, &error));
    CHECK(!Initialize(hooks, &error));
    CHECK(!CanEdit());
    run.phase = "active";
    CHECK(SaveRun("fixture-bridge/firmware-pocket/status", run, &error));
    CHECK(Initialize(hooks, &error));
    CHECK(Integrated() && !CanEdit());
    CHECK(callback_run.image_bytes == 848);
    CHECK(CanReturn());
    CHECK(callback_run.app_dir == "fixture-bridge/firmware-pocket/users/legacy.user/apps/test_game");
    CHECK(callback_run.storage_mode == 0 && callback_run.state_schema == 0);
    CHECK(status.Load("receipt", 1).status == RecordStatus::Missing);
    ack_ok = false;
    CHECK(!ConfirmStartup(1, &error));
    CHECK(status.Load("receipt", 1).status == RecordStatus::Missing);
    ack_ok = true;
    CHECK(ConfirmStartup(1, &error));
    CHECK(acks == 2);
    CHECK(CanEdit());
    CHECK(status.Load("receipt", 1).payload.find("\"started\"") != std::string::npos);
    CHECK(WriteHandoffReceipt("fixture-bridge/firmware-pocket/status", run, true, &error));
    CHECK(Initialize(hooks, &error));
    CHECK(!CanEdit());
    CHECK(ConfirmStartup(1, &error));
    CHECK(status.Load("receipt", 1).payload.find("\"started\"") != std::string::npos);
    save_ok = false;
    CHECK(!ReturnToPocket(&error));
    CHECK(LastError() == Error::SaveFailed);
    CHECK(restarts == 0);
    save_ok = true;
    boot_ok = false;
    CHECK(!ReturnToPocket(&error));
    CHECK(LastError() == Error::ReturnFailed);
    CHECK(CanEdit());
    CHECK(status.Load("receipt", 1).payload.find("\"started\"") != std::string::npos);
    CHECK(restarts == 0);
    boot_ok = true;
    mounted = false;
    CHECK(!Save(&error));
    CHECK(!CanEdit());
    mounted = true;
    CHECK(Save(&error));
    CHECK(CanEdit());
    manager_image[847] ^= 1;
    CHECK(!ReturnToPocket(&error));
    CHECK(LastError() == Error::ReturnFailed);
    CHECK(restarts == 0);
    manager_image = Image(true);
    CHECK(ReturnToPocket(&error));
    CHECK(restarts == 1);
    CHECK(!CanEdit());
    CHECK(status.Load("receipt", 1).payload.find("\"saved\"") != std::string::npos);
    CHECK(leases == 0 && !callback_lease_violation);
    std::cout << "PASS " << checks << " real bridge checks with mock HAL/Flash/OTA\n";
    return 0;
}

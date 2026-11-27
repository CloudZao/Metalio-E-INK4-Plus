#include "system_state.h"
#include "sysmgmt/power/policy/power_policy.h"

bool SystemState::PublishDevice(DeviceState state) {
    const auto phase = Phase();
    if ((phase == SystemPhase::Updating && state != kDeviceStateUpgrading) || phase == SystemPhase::Restarting ||
        (phase == SystemPhase::Failed && state != kDeviceStateFatalError)) {
        return false;
    }
    device_.store(state, std::memory_order_release);
    PowerPolicy::GetInstance().NotifyDeviceState(state);
    return true;
}

void SystemState::MarkReady() {
    auto expected = SystemPhase::Starting;
    if (!phase_.compare_exchange_strong(expected, SystemPhase::Running, std::memory_order_acq_rel)) {
        return;
    }
    ready_.store(true, std::memory_order_release);
}

void SystemState::FailStartup() {
    auto expected = Phase();
    while (expected == SystemPhase::Starting || expected == SystemPhase::Running) {
        if (phase_.compare_exchange_weak(expected, SystemPhase::Failed, std::memory_order_acq_rel)) {
            PublishDevice(kDeviceStateFatalError);
            break;
        }
    }
    SetStartupBusy(false);
}

bool SystemState::BeginUpdate() {
    auto expected = Phase();
    while (expected == SystemPhase::Starting || expected == SystemPhase::Running) {
        if (phase_.compare_exchange_weak(expected, SystemPhase::Updating, std::memory_order_acq_rel)) {
            PublishDevice(kDeviceStateUpgrading);
            return true;
        }
    }
    return false;
}

void SystemState::EndUpdate() {
    auto expected = SystemPhase::Updating;
    const auto restored = IsReady() ? SystemPhase::Running : SystemPhase::Starting;
    if (phase_.compare_exchange_strong(expected, restored, std::memory_order_acq_rel)) {
        PublishDevice(kDeviceStateIdle);
    }
}

void SystemState::BeginRestart() {
    phase_.store(SystemPhase::Restarting, std::memory_order_release);
}

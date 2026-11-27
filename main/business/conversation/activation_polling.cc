#include "sysmgmt/power/power_lifecycle.h"
#include "business/conversation/conversation_presentation.h"
#include "activation_polling.h"

ActivationPolling& ActivationPolling::GetInstance() {
    static ActivationPolling instance;
    return instance;
}

void ActivationPolling::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++pause_count_;
    paused_.store(true, std::memory_order_release);
}

void ActivationPolling::Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pause_count_ == 0) {
        return;
    }
    --pause_count_;
    paused_.store(pause_count_ != 0, std::memory_order_release);
    if (pause_count_ == 0) {
        RequestCheck();
    }
}

bool ActivationPolling::IsPaused() const {
    return paused_.load(std::memory_order_acquire);
}

bool ActivationPolling::IsBusy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}

void ActivationPolling::RequestCheck() {
    check_requested_.store(true, std::memory_order_release);
}

bool ActivationPolling::TakeCheckRequest() {
    return check_requested_.exchange(false, std::memory_order_acq_rel);
}

bool ActivationPolling::HasCheckRequest() const {
    return check_requested_.load(std::memory_order_acquire);
}

bool ActivationPolling::TryBegin() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pause_count_ != 0 || busy_) {
        return false;
    }
    busy_ = true;
    return true;
}

bool ActivationPolling::SetCode(const std::string& code) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (code_ == code) {
        return false;
    }
    code_ = code;
    return true;
}

std::string ActivationPolling::Code() {
    std::lock_guard<std::mutex> lock(mutex_);
    return code_;
}

bool ActivationPolling::HasCode() {
    std::lock_guard<std::mutex> lock(mutex_);
    return !code_.empty();
}

void ActivationPolling::Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false;
}

ActivationPollGuard::ActivationPollGuard() : acquired_(ActivationPolling::GetInstance().TryBegin()) {}

ActivationPollGuard::~ActivationPollGuard() {
    if (acquired_) {
        ActivationPolling::GetInstance().Finish();
    }
}

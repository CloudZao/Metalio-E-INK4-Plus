#pragma once
#include "display.h"
#include <cstdint>
#include <memory>
#include <string>

/** @brief Emote 表情显示 stub（本板不用 EMOTE 风格） */
namespace emote {

class EmoteDisplay : public Display {
public:
    void AddTextFont(const std::shared_ptr<void>& /*font*/) {}
    void AddEmojiData(const char* /*name*/, void* /*ptr*/, size_t /*size*/, uint8_t /*fps*/, bool /*loop*/,
                      bool /*lack*/) {}
    void AddIconData(const char* /*name*/, void* /*ptr*/, size_t /*size*/) {}
    void AddLayoutData(const char* /*name*/, const char* /*align*/, int /*x*/, int /*y*/, int /*w*/, int /*h*/) {}

protected:
    bool Lock(int /*timeout_ms*/) override {
        return true;
    }
    void Unlock() override {}
};

} // namespace emote

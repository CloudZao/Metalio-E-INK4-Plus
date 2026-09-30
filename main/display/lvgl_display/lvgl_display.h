#pragma once
#include "display.h"
/** @brief lvgl_display stub */
class LvglDisplay : public Display {
protected:
    bool Lock(int /*timeout_ms*/) override {
        return true;
    }
    void Unlock() override {}
};

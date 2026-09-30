#pragma once

/** @brief LVGL 主题 stub */
class LvglTheme {
public:
    static LvglTheme& GetInstance() {
        static LvglTheme inst;
        return inst;
    }
};

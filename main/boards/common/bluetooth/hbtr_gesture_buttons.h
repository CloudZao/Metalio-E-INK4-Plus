#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace HbtrGestureButtons {
constexpr uint8_t kStandbyKey = 3; // 固定待机键，不参与翻页学习
constexpr uint8_t kInvalid = 0xff; // 无完整按键事件

/** @brief 判断 HBTR002-X 和 HBTR003-X 的待机和轨迹输入报告 */
inline bool IsButtonReport(uint8_t id, uint8_t type) {
    return type == 1 && (id == 1 || id == 2);
}

class Decoder {
public:
    /** @brief 断开、切换设备或取消输入时清理未完成轨迹 */
    void Reset() { touching_ = standby_down_ = false; }

    /** @brief 将完整轨迹或待机按下转换为单次事件，其他帧返回 kInvalid */
    uint8_t Decode(uint8_t id, const uint8_t* data, size_t size) {
        if (!data) return kInvalid;
        if (id == 1 && size == 2) {
            if (data[0] == 0 && data[1] == 0) standby_down_ = false;
            else if (data[0] == 0 && data[1] == 0x10 && !standby_down_) {
                standby_down_ = true;
                return kStandbyKey;
            }
            return kInvalid;
        }
        if (id != 2) return kInvalid;
        if (size != 4 || (data[0] != 0 && data[0] != 2 && data[0] != 7)) {
            touching_ = false;
            return kInvalid;
        }
        const int x = data[1] | ((data[2] & 0x0f) << 8);
        const int y = (data[2] >> 4) | (data[3] << 4);
        if (data[0] == 7) {
            if (!touching_) { start_x_ = x; start_y_ = y; }
            touching_ = true;
            return kInvalid;
        }
        if (!touching_) return kInvalid;
        touching_ = false;
        const int dx = x - start_x_;
        const int dy = y - start_y_;
        // 完整横向轨迹才产生事件，忽略轻微抖动和纵向移动。
        if (std::abs(dx) < 100 || std::abs(dy) > std::abs(dx)) return kInvalid;
        return dx > 0 ? 1 : 2;
    }

private:
    bool touching_ = false; // 当前轨迹仍处于接触阶段
    bool standby_down_ = false; // 待机报告按下状态
    int start_x_ = 0; // 轨迹起点横坐标
    int start_y_ = 0; // 轨迹起点纵坐标
};
} // namespace HbtrGestureButtons

#pragma once

#include "lvgl.h"
#include "workbuddy_chat_memory.h"

#include <cstdint>
#include <string>
#include <vector>

namespace workbuddy { struct LocalMessage; }

/** @brief 管理有界对话、气泡排版和消息位置；仅在 LVGL 线程使用。 */
class WorkBuddyChat {
public:
    /** @brief 创建消息区和只读页码，父对象负责控件生命周期。 */
    void Create(lv_obj_t* parent);
    /** @brief 清空当前账号的消息。 */
    void Clear();
    /** @brief 显示居中提示；空字符串恢复对话或空消息提示。 */
    void ShowIntro(const std::string& text);
    /** @brief 按轮次和发送方更新一个气泡，follow 定位到最新的用户消息。 */
    void SetMessage(uint32_t id, bool mine, const std::string& text, bool follow);
    /** @brief 用同步结果替换历史快照并保留角色，排版失败返回 false。 */
    bool ReplaceMessages(const std::vector<workbuddy::LocalMessage>& messages);
    /** @brief 按实际内容页翻页；实际切页返回 true，提示页或触及边界返回 false。 */
    bool TurnPage(int delta);

private:
    struct Line {
        uint16_t begin = 0; // 正文的完整 UTF-8 行首字节
        uint16_t end = 0; // 行尾字节，不包含换行符
        int16_t width = 0; // 本行实际正文宽度
    };
    struct Message {
        uint32_t id; // 页面内唯一轮次，0 留给同步摘要
        bool mine; // 是否为用户消息
        workbuddy_history::String text; // PSRAM 中的纯文本，短串随消息容器存放
        workbuddy_history::Vector<Line> lines; // PSRAM 中的物理行索引
        int32_t layout_width = 0; // 缓存行布局对应的宽度
    };
    struct Fragment {
        size_t message; // 所属消息下标
        size_t begin; // 本片段起始物理行
        size_t end; // 本片段结束物理行
        size_t text_begin; // 重排前后保持阅读位置的正文字节
        size_t page; // 所属内容页
        int32_t y; // 气泡行起始纵坐标
        int32_t height; // 气泡高度，包含正文内边距
    };
    void Layout(Message& message, int32_t width);
    bool StoreMessage(uint32_t id, bool mine, const std::string& text);
    bool DropOldest();
    bool ReflowWithTrim(bool follow);
    void Reflow(bool follow);
    void Paint();
    lv_obj_t* body_ = nullptr; // 当前页气泡容器
    lv_obj_t* pager_ = nullptr; // 固定底部页码
    workbuddy_history::Vector<Message> messages_; // PSRAM 对话容器，容量计入历史预算
    workbuddy_history::Vector<Fragment> fragments_; // PSRAM 分页索引，不复制消息正文
    size_t page_ = 0; // 当前内容页
    size_t page_count_ = 1; // 总页数
    uint32_t anchor_id_ = 0; // 回收分页缓存前记录当前阅读消息
    size_t anchor_byte_ = 0; // 当前阅读位置的正文偏移
    bool anchor_mine_ = false; // 阅读锚点所属角色
    bool anchor_valid_ = false; // 排版重试期间保留锚点
    std::string intro_; // 授权或连接提示，空时显示消息
    uint8_t welcome_index_ = 0; // 本次进入选定的欢迎语，刷新时保持不变
};

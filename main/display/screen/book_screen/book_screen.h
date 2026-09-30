#pragma once

#include "lvgl.h"

// 阅读应用：首页（继续阅读 / 时长 / 已看完 / 我的书架 / 最近阅读两本）→ 书架网格 → 详情 → 正文。
// 封面：同步只读 .a2i1；无旁路则占位，后台抽内嵌并回写 .a2i1 后刷槽（列表串行，避免卡顿）。
// 正文「正在打开」统一承担 BookSession::Open；成功后回写书名/作者，并可补写 EPUB/.ebook 旁路。
// 正文：默认无顶栏；点九宫格「菜单」分区唤出顶部悬浮排版卡片（盖正文，不挤开内容）；
// 卡片：字体列表点选 / 长按多选批量移除（多选时标题下多出操作行，▲▼ 仍翻页）、边距行距 ±、底栏信息多选（章节名/时间/进度/电量）、下划线关/实/虚、前光色温/亮度、底栏「目录」「触摸分区」；点正文空白或 vk 收起；
// 目录：卡片「目录」进整页（封面+书名顶下移 12px 最多三行省略+作者贴底上移 12px 单行省略，未知显示「未知」+章节列表，TXT 带页码）；
// 改排版：先重算当前页即时预览（卡片不关）；TXT 再后台重建页表对齐锚点；章节书同步重排当前章。
// 正文打开成功后累计阅读秒（墙钟），退出与约 45s checkpoint 写入同目录 .pos（POS3：终身+当日）；
// 进待机停钟，退出待机若仍在正文则恢复。
// 书籍推送同步在「传输」应用；本应用仅本地书库阅读。
// 导航约定：阅读首页为应用根；短按 vk_home：正文→详情→（书架或首页）→系统首页；
// 长按 vk_home：交默认策略一键回系统首页（不进百问）；vk_prev 子系统内退一级；
// 首页 fallthrough 回系统首页；书架长按 prev/next 翻网格页；
// 目录/正文长按 prev/next 自按下起每 1s ±10 页；正文音量+=上一页、音量-=下一页（含长按连翻）。
// 字体 / 行距 / 边距 / 底栏字段 / 下划线偏好由正文悬浮排版卡片写入 NVS；设置卡 vk_prev 在字体多选时先取消批量。
class BookScreen {
public:
    /** @brief 创建阅读应用根页 */
    static lv_obj_t* Create();

    /** @brief 正文沉浸阅读中（目录/设置浮层除外）；音量键作翻页 */
    static bool IsReadingActive();

    /** @brief 在正文屏（含目录/设置浮层）；阅读灰 AA 刷屏用 */
    static bool IsReadScreenActive();

    /** @brief 虚拟键覆盖：注册到 VkKey_AttachScreen.on_key，返回 true 表示已消费 */
    static bool OnVkKey(const char* key_name);

    /** @brief BOOT 短按：正文切换悬浮排版卡片；其余页面不消费 */
    static bool OnBootClick();
    /**
     * @brief BOOT 长按：进百问（正文先落盘并释放 session）；盖板 vk_home 长按不进百问
     * @note 盖板 prev/next 长按：书库/目录/正文自按下起每 1s ±10 页，松手停止
     */
    static bool OnBootLongPress();
    /** @brief 虚拟键长按；返回 true 表示已消费 */
    static bool OnVkKeyLongPress(const char* key_name);
    /** @brief 虚拟键抬起；返回 true 表示已消费 */
    static bool OnVkKeyPressUp(const char* key_name);

    /** @brief 进待机：若正在累计阅读时长则停钟（不落盘） */
    static void OnEnterStandby();
    /** @brief 退出待机：若因待机停钟且 session 仍开则恢复累计 */
    static void OnResumeFromStandby();
};

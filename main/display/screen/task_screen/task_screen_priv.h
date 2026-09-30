#pragma once

#include "task_screen.h"
#include "ui_scale.h"

#include <lvgl.h>

constexpr const char* TAG = "TaskScreen";
constexpr const char* kScreenId = "task";

// 源值对齐 397 task_screen；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kPad = UiSx(12);
constexpr lv_coord_t kBorderW = UiSx(2);
constexpr lv_coord_t kRowPad = UiSx(10); // row pad_all，与 RebuildListPage 一致
constexpr lv_coord_t kRowGap = UiSy(8);
constexpr lv_coord_t kRowLineGap = UiSy(6);
constexpr lv_coord_t kFooterH = UiSy(36); // 框外页码行高
constexpr lv_coord_t kActionH = UiSy(48);
constexpr lv_coord_t kTabH = UiSy(48);
constexpr lv_coord_t kTabW = UiSx(280); // 顶部分段条宽度（居中）
constexpr lv_coord_t kTabBorderW = UiSx(3); // 外圈黑边（略宽于列表描边）
constexpr lv_coord_t kTabInset = UiSx(3); // 内缩，让选中胶囊两侧圆弧可见
constexpr lv_coord_t kHeaderH = UiSy(36); // 「日程待办」标题行高
constexpr lv_coord_t kHeaderIcon = UiSx(32); // 星标圆直径
constexpr lv_coord_t kHeaderGap = UiSx(8); // 星标与标题间距
constexpr lv_coord_t kSyncBtnH = UiSy(44);
constexpr lv_coord_t kSyncDividerW = 2; // 底栏顶部分割线
constexpr lv_coord_t kSyncUnderlineH = 2; // 「刷新」/多选动作下划线
constexpr lv_coord_t kSyncUnderlinePadHor = UiSx(5); // 下划线两端各外延；共比字宽加长 10px
constexpr lv_coord_t kMultiDotSize = UiSx(6); // 分隔圆点直径（约 · 的 2 倍）
constexpr lv_coord_t kListFrameBorderW = UiSx(2);
constexpr lv_coord_t kListFrameRadius = UiSx(10);
constexpr lv_coord_t kListFramePad = UiSx(10);
constexpr lv_coord_t kActionRowH = kSyncBtnH; // 框内同步/批量行高（同传输 kFooterBlockH）
constexpr lv_coord_t kFooterOutsideH = kFooterH; // 框外仅页码；与框间距靠 body pad_row
constexpr lv_coord_t kCheckSize = UiSx(28); // 多选行尾勾选框
constexpr lv_coord_t kCheckGap = UiSx(10);
constexpr lv_coord_t kStatusGap = UiSx(12); // 标题与提示间距
constexpr lv_coord_t kRowRadius = UiSx(8);
constexpr lv_coord_t kCheckRadius = UiSx(4);
constexpr lv_coord_t kDialogPad = UiSx(16);
constexpr lv_coord_t kDialogRowGap = UiSy(12);
constexpr lv_coord_t kDialogMsgInset = UiSx(40);
constexpr lv_coord_t kSyncUnderlinePadBottom = UiSy(2);
constexpr lv_coord_t kMultiBarGap = UiSx(10);
constexpr lv_coord_t kMultiActionPadHor = UiSx(4);
constexpr lv_coord_t kListPerPageOffset = UiSy(28);

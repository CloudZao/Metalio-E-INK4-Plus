#pragma once

#include <cstdint>
#include <string>

#include <lvgl.h>

#include "reader/reader.h"

// 书架列表 / 删除 / 多选（实现分 book_shelf_*.cc）

/** @brief 当前书架目录是否为书库根 */
bool ShelfDirIsRoot();
/** @brief 取父目录路径 */
std::string ShelfParentDir(const std::string& dir);
/** @brief 按路径查找书库下标；未找到返回 -1 */
int FindBookIndexByPath(const std::string& path);
/** @brief 刷新封面/列表视图切换按钮态 */
void RefreshShelfViewToggleUi();
/** @brief 刷新书架标题（当前目录名） */
void UpdateShelfTitleLabel();
/** @brief 重新加载当前目录条目 */
void ReloadShelfDirListing();
/** @brief 扫盘 + 刷列表；须在 LVGL/异步任务跑，禁止 touch_feed 同步调用 */
void ShelfReloadAndRenderAsync(void* user_data);
/** @brief 按视图模式应用列表分页大小 */
void ApplyShelfListPageSize();
/** @brief 文件夹行点击进入子目录 */
void OnShelfFolderClicked(lv_event_t* e);
/** @brief 列表条目长按（进多选） */
void OnShelfListEntryLongPressed(lv_event_t* e);
/** @brief 列表书籍点击打开详情 */
void OnShelfListBookClicked(lv_event_t* e);
/** @brief 切换到封面视图 */
void OnShelfViewCoverClicked(lv_event_t* e);
/** @brief 切换到列表视图 */
void OnShelfViewListClicked(lv_event_t* e);
/** @brief 渲染书架列表视图当前页 */
void RenderBookshelfListPage();

/** @brief 删除书库中的单本书及进度旁路 */
bool DeleteLibraryBookFile(const reader::BookInfo& info, std::string& err_out);
/** @brief 递归删除书库文件夹 */
bool DeleteLibraryFolder(const std::string& dir, std::string& err_out);

/** @brief 请求刷书架勾选 */
void RequestShelfCheckMarksPaint();
/** @brief 同步书架多选集合容量 */
void SyncShelfSelectedSize();
/** @brief 书架已选数量 */
int ShelfSelectedCount();
/** @brief 书架项是否已选 */
bool ShelfItemSelected(int idx);
/** @brief 切换书架项选中 */
void ToggleShelfItemSelected(int idx);
/** @brief 刷新书架底栏（多选/普通） */
void RefreshShelfFooterMode();
/** @brief 退出书架多选 */
void ExitShelfMultiMode(bool rebuild);
/** @brief 进入书架多选 */
void EnterShelfMultiModeSelect(int idx);
/** @brief 刷新单行勾选标记 */
void PatchShelfRowCheckMark(int index);
/** @brief 刷新可见行勾选标记 */
void PatchShelfVisibleCheckMarks();
/** @brief 书架多选取消 */
void OnShelfMultiCancel(lv_event_t* e);
/** @brief 书架多选全选 */
void OnShelfMultiSelectAll(lv_event_t* e);
/** @brief 书架多选删除 */
void OnShelfMultiRemove(lv_event_t* e);
/** @brief 书架行长按 */
void OnBookRowLongPressed(lv_event_t* e);
/** @brief 书架行点击 */
void OnBookRowClicked(lv_event_t* e);

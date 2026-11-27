#pragma once

#include <cstdint>
#include <string>
#include <lvgl.h>
#include "business/reader/reader.h"

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

/** @brief 确保当前目录页已请求，未就绪时返回 false */
bool EnsureShelfDirectoryPage();

/** @brief 返回当前目录已知页数 */
int ShelfDirectoryPages();

/** @brief 释放目录分页任务和句柄 */
void CancelShelfDirectory();

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

/** @brief 后台删除当前已选书籍和文件夹，不阻塞 UI */
void StartShelfDelete();

/** @brief 请求在当前单书清理完成后停止删除 */
void CancelShelfDelete();

/** @brief 查询书架删除任务是否尚未完成 */
bool ShelfDeleteBusy();

/** @brief 请求刷书架勾选 */
void RequestShelfCheckMarksPaint();

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

/** @brief 将当前页后台读取的文件属性和进度更新到对应控件 */
void ApplyShelfEntryMetadata(const BookInfo& info, lv_obj_t* host, bool progress_ready);

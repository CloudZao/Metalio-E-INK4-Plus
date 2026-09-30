#pragma once

#include <lvgl.h>

#include "reader_types.h"

/** @brief 复位预览方向/镜像状态 */
void ResetOrientState();
/** @brief 调整是否有未保存改动 */
bool AdjustDirty();
/** @brief 若对边都有明显白边（旧版 90° 烤进去的留白），裁到墨迹包围盒，便于再次旋转铺满 */
/** @brief 若对边都有明显白边则裁到墨迹包围盒 */
void TrimBakedLetterbox(reader::RasterImage& img);
/** @brief 按旋转/镜像变换壁纸栅格 */
void ApplyWallpaperOrient(const reader::RasterImage& src, uint8_t rot_cw, bool mirror_h,
                          reader::RasterImage& out);
/** @brief 把栅格绑定到预览 Image */
void BindPreviewRasterToImg();
/** @brief 按当前变换刷新预览 */
void RefreshPreviewFromBase();
/** @brief 刷新调整模式按钮态 */
void UpdateAdjustUi();
/** @brief 关闭覆盖保存对话框 */
void CloseOverwriteDialog();
/** @brief 退出调整模式 */
void ExitAdjustMode(bool restore_preview);
/** @brief 进入调整模式 */
void EnterAdjustMode();
/** @brief 点击「调整」 */
void OnAdjustClicked(lv_event_t* e);
/** @brief 调整取消 */
void OnAdjustCancelClicked(lv_event_t* e);
/** @brief 逆时针旋转 */
void OnRotateLeftClicked(lv_event_t* e);
/** @brief 顺时针旋转 */
void OnRotateRightClicked(lv_event_t* e);
/** @brief 水平镜像 */
void OnMirrorHClicked(lv_event_t* e);
/** @brief 另存为 */
void OnSaveAsClicked(lv_event_t* e);
/** @brief 覆盖原图 */
void OnOverwriteClicked(lv_event_t* e);
/** @brief 取消保存对话框 */
void OnSaveCancelClicked(lv_event_t* e);
/** @brief 显示覆盖确认框 */
void ShowOverwriteDialog();
/** @brief 保存（覆盖或另存入口） */
void OnSaveClicked(lv_event_t* e);
/** @brief 调度把变换烤回文件 */
void ScheduleSaveTransform(bool save_as);

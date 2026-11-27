#pragma once
#include <memory>
#include <lvgl.h>
#include "business/cloud/cloud_resource_service.h"

class CloudPreview {
public:
    CloudPreview() = default;
    CloudPreview(const CloudPreview&) = delete;
    CloudPreview& operator=(const CloudPreview&) = delete;

    /** @brief 在UI线程解除控件借用并归还仍持有的请求和像素。 */
    ~CloudPreview();

    /** @brief 创建预览控件并绑定保存意图；所有接口均在UI线程调用。 */
    void Build(lv_obj_t* root, lv_coord_t status_height, lv_coord_t width, void (*save)(int));

    /** @brief 从独立输入快照打开预览；等待传输时暂不发起图片工作。 */
    bool Open(int index, const CloudPushResource& resource, std::shared_ptr<HttpTempFile> cache, bool waiting);

    /** @brief 关闭预览，取消加载并解除控件对图像缓冲的借用。 */
    void Close();

    /** @brief 取消当前加载，保留已显示图像。 */
    void CancelPending();

    /** @brief 暂停图片接收并显示传输等待提示，保留当前输入供恢复。 */
    void PauseForTransfer();

    /** @brief 从持有的输入快照重新加载当前预览。 */
    void Resume();

    /** @brief 接收当前请求结果；旧请求由工作对象自行收尾。 */
    void Poll();

    /** @brief 页面释放前取消请求、释放像素并解除控件登记；可重复调用。 */
    void Reset();

    /** @brief 列表删除后调整当前条目标识，不接管列表数据。 */
    void OnItemErased(int index);

    /** @brief 查询预览是否打开。 */
    bool IsOpen() const { return open_; }

    /** @brief 获取预览的条目标识；失效时返回负值。 */
    int Index() const { return index_; }

private:
    static void OnSave(lv_event_t* event);
    static void OnDeleted(lv_event_t* event);
    void ClearImage();
    void StartLoad();
    void SetStatus(const char* text);
    void FillMeta(const CloudPushResource* item);
    void StyleSaveButton();

    struct Widgets {
        lv_obj_t* body = nullptr;
        lv_obj_t* title = nullptr;
        lv_obj_t* meta = nullptr;
        lv_obj_t* status = nullptr;
        lv_obj_t* img = nullptr;
        lv_obj_t* img_host = nullptr;
        lv_obj_t* download_btn = nullptr;
        lv_obj_t* download_lbl = nullptr;
    } widgets_;
    CloudImageRequestPtr request_;
    CloudImageItem input_;
    std::unique_ptr<RasterImage> image_;
    void (*save_)(int) = nullptr;
    lv_coord_t width_ = 0;
    int index_ = -1;
    bool open_ = false;
};

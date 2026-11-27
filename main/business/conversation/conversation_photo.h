#pragma once
#include <atomic>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include "infra/image/raster_image.h"
#include "infra/network/http/download_gate.h"

class Camera;
struct ConversationPhoto {
    uint8_t* jpeg = nullptr;     // 原始 JPEG，后续附件上传使用。
    size_t length = 0;           // JPEG 字节数。
    std::string image_id;        // 本地生成、由上传响应确认的图片标识。
    std::atomic<int64_t> uploaded_at{0}; // 最近上传完成时间，使用单调时钟。
    bool history_added = false;  // 防止提问重试重复加入聊天记录。
    RasterImage image;           // 适屏大图。
    RasterImage thumbnail;       // 附件缩略图。

    ~ConversationPhoto() {
        std::free(jpeg);
    }
};


struct ConversationPhotoResult {
    std::shared_ptr<ConversationPhoto> photo;
    DownloadGate gate;
    std::atomic<bool> completed{false};
    std::string image_id;
    int64_t uploaded_at = 0;
};
using ConversationPhotoRequest = std::shared_ptr<ConversationPhotoResult>;
using ConversationPhotoCompleted = std::function<void(const ConversationPhotoRequest&)>;

/** @brief 采集并解码照片，图像和原始JPEG由结果持有；完成回调在后台执行。 */
ConversationPhotoRequest ConversationCapturePhoto(Camera* camera, int width, int height,
                                                  int thumbnail_width, int thumbnail_height,
                                                  ConversationPhotoCompleted completed);

/** @brief 上传并校验图片标识，过期缓存重新上传；未接纳时不调用完成回调。 */
ConversationPhotoRequest ConversationUploadPhoto(const std::shared_ptr<ConversationPhoto>& photo,
                                                 ConversationPhotoCompleted completed);

/** @brief 请求取消照片工作，网络关闭与缓冲回收由业务完成。 */
void ConversationCancelPhoto(const ConversationPhotoRequest& request);

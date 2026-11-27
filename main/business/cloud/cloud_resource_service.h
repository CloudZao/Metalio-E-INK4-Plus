#pragma once
#include <atomic>
#include <memory>
#include <vector>
#include "business/cloud/push_resources_library.h"
#include "infra/network/http/download_gate.h"
#include "infra/network/http/http_download_file.h"
#include "infra/image/image_decode.h"

enum class CloudResourceOperation : uint8_t { Fetch, Delete, Download };
enum class CloudResourceStage : uint8_t { Queued, Running, Conflict, Finished };
enum class CloudSaveChoice : uint8_t { kAsk, kReplace, kKeepBoth, kSkip };
enum class CloudResourceOutcome : uint8_t { Pending, Saved, Acknowledged, Deleted, Skipped, Failed };

struct CloudDownloadItem {
    CloudPushResource resource;
    std::shared_ptr<HttpTempFile> cover; // 下载书籍期间持有封面工作文件
};

struct CloudResourceRequest {
    CloudResourceOperation operation = CloudResourceOperation::Fetch;
    std::atomic<CloudResourceStage> stage{CloudResourceStage::Queued};
    std::atomic<bool> completed{false}; // 发布后才能读取结果列表、outcomes和error
    std::atomic<size_t> current{0}; // 下载输入索引；inputs在接纳后不再修改
    std::atomic<int> percent{0};
    std::atomic<int> succeeded{0};
    std::atomic<int> skipped{0};
    std::vector<CloudDownloadItem> inputs;
    std::vector<CloudPushResource> items; // 拉取结果
    std::vector<CloudResourceOutcome> outcomes; // 与输入一一对应
    int failed = 0;
    char error[96] = {};
    DownloadGate gate; // 能力持有请求和取消，不借用页面状态
};

using CloudResourceRequestPtr = std::shared_ptr<CloudResourceRequest>;

/** @brief 提交列表请求；忙碌或执行通道未接纳时返回空。 */
CloudResourceRequestPtr CloudFetchResources();

/** @brief 提交远端删除；逐项保留一次失败重试，不依赖页面存活。 */
CloudResourceRequestPtr CloudDeleteResources(std::vector<CloudPushResource> items);

/** @brief 提交单项或批量保存；冲突时归还执行通道，等待用户选择。 */
CloudResourceRequestPtr CloudDownloadResources(std::vector<CloudDownloadItem> items,
                                             CloudSaveChoice choice = CloudSaveChoice::kAsk);

/** @brief 提交当前冲突的处理选择；投递失败保留冲突及原请求责任。 */
bool CloudResolveSaveConflict(const CloudResourceRequestPtr& request, CloudSaveChoice choice);

/** @brief 请求取消；已提交的文件及成功回执保留，阻塞HTTP由清理通道中断。 */
void CloudCancelResourceRequest(const CloudResourceRequestPtr& request);

/** @brief 查询云资源能力是否仍持有在途作业或冲突等待。 */
bool CloudResourcesBusy();

struct CloudImageItem {
    int index = -1; // 接收者的条目标识，后台不解引用页面
    std::string url;
    std::string path;
    std::shared_ptr<HttpTempFile> cache;
    int width = 0;
    int height = 0;
    bool binary = false;
    HttpCacheApp cache_app = HttpCacheApp::Transfer;
    std::unique_ptr<RasterImage> image;
    ImageDecodeError error = ImageDecodeError::kNone;
};

struct CloudImageRequest {
    std::vector<CloudImageItem> items; // 接纳后输入不变，completed发布后读取图片与缓存
    DownloadGate gate;
    std::atomic<bool> completed{false};
};

using CloudImageRequestPtr = std::shared_ptr<CloudImageRequest>;

/** @brief 提交封面或预览，工作对象持有缓存与像素；失败返回空。 */
CloudImageRequestPtr CloudLoadImages(std::vector<CloudImageItem> items);

/** @brief 取消图片加载，解码检查取消令牌，HTTP由清理通道中断。 */
void CloudCancelImages(const CloudImageRequestPtr& request);

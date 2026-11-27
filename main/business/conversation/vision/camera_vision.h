#pragma once

#include <mutex>
#include <string>

class Camera;

/** @brief MCP视觉业务，负责服务配置、一次性照片所有权及请求，不控制摄像头供电。 */
class CameraVision {
public:
    /** @brief 替换当前会话的视觉服务配置，空地址表示未配置。 */
    void Configure(const std::string& url, const std::string& token);

    /** @brief 通过硬件接口拍照并识别，照片在本次调用结束时释放。 */
    std::string TakePhoto(Camera& camera, const std::string& question);

private:
    std::mutex mutex_;   // 仅保护配置快照，不跨采集或网络等待持有
    std::string url_;    // 当前MCP会话的服务地址，不输出日志
    std::string token_;  // 可选凭据，不输出日志
};

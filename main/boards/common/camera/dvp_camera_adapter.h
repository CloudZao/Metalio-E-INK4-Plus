#pragma once

#include "camera.h"

#include <cstdlib>
#include <memory>
#include <mutex>

/** @brief 将板级 DVP 静拍接入 MCP 视觉服务 */
class DvpCamera : public Camera {
public:
    /** @brief 设置 MCP 初始化下发的视觉服务地址与可选凭据 */
    void SetExplainUrl(const std::string& url, const std::string& token) override;
    /** @brief 采集彩色 JPEG，替换尚未上传的照片 */
    bool Capture() override;
    /** @brief 水平镜像暂不支持 */
    bool SetHMirror(bool /*enabled*/) override { return false; }
    /** @brief 垂直翻转暂不支持 */
    bool SetVFlip(bool /*enabled*/) override { return false; }
    /** @brief 上传已拍照片和问题，返回视觉服务结果并释放照片 */
    std::string Explain(const std::string& question) override;
    /** @brief 静拍 JPEG 并移交所有权，调用方使用 free 释放 */
    bool CaptureStillJpeg(uint8_t** out_data, size_t* out_len) override;

private:
    std::mutex mutex_; // 保护服务配置与待上传照片
    std::string explain_url_; // MCP 视觉服务地址
    std::string explain_token_; // 可选 Bearer 凭据
    std::unique_ptr<uint8_t, decltype(&std::free)> jpeg_{nullptr, std::free}; // 待上传 JPEG
    size_t jpeg_len_ = 0; // 待上传 JPEG 的有效长度
};

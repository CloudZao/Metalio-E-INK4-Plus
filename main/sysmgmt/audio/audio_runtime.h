#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>
#include "infra/audio/audio_service.h"

enum class AudioClient : uint32_t {
    Assistant = 1,    // 对话语音
    FactoryTest = 2,  // 录音测试
    AgingTest = 4,    // 老化测试
    WorkBuddy = 8,    // WorkBuddy 独占录音
    AppPlayback = 16, // ELF 本地播放，由宿主工作任务持有
};

class AudioRuntime {
public:
    /** @brief 获取共享音频资源的唯一所有者 */
    static AudioRuntime& GetInstance();

    /** @brief 在启动阶段绑定语音队列通知，运行期间不得更换 */
    void Configure(AudioServiceCallbacks callbacks);

    /** @brief 获取采集和播放接口；使用前须取得调用方占用 */
    AudioService& Service() {
        return service_;
    }

    /** @brief 申请音频占用，等待期间失效的请求不保留资源 */
    bool Acquire(AudioClient client, std::function<bool()> still_needed = {});

    /** @brief 归还音频占用，最后一个调用方退出后停止服务 */
    void Release(AudioClient client);

    /** @brief 非阻塞归还占用；投递失败时保留请求，由系统节拍重试 */
    void RequestRelease(AudioClient client);

    /** @brief 申请本地播放独占权，仅供内部 RAM 栈任务调用；其他音频调用方优先 */
    bool AcquireLocalPlayback(void* owner);

    /** @brief 申请固定 16kHz 录音独占权，复用本地音频所有权管理 */
    bool AcquireLocalRecording(void* owner);

    /** @brief 读取一帧麦克风 PCM 并剔除回采声道；失去所有权时返回 false */
    bool ReadLocalRecording(void* owner, std::vector<int16_t>& pcm);

    /** @brief 在仍持有独占权时写入单声道 PCM；系统音频申请后返回 false */
    bool WriteLocalPlayback(void* owner, std::vector<int16_t>& pcm);

    /** @brief 释放本地播放独占权；不会释放其他调用方已经接管的硬件 */
    void ReleaseLocalPlayback(void* owner);

    /** @brief 查询本地播放或录音是否仍占有音频硬件，供待机收尾等待 */
    bool HasLocalPlayback() const {
        return local_playback_owner_.load(std::memory_order_acquire) != nullptr;
    }

    /** @brief 尝试交付尚未完成的释放请求 */
    void RetryPendingReleases();

    /** @brief 在音频模块就绪后启动共享服务 */
    void EnsureStarted();

    /**
     * @brief 升级、关机或重启前停止服务
     * @param force 为 true 时忽略老化占用，并清理未完全启动的硬件；普通停用保留占用以便失败后恢复
     */
    void StopIfRunning(bool force = false);

    /** @brief 启动收尾时仅清理没有调用方持有的服务 */
    void StopIfUnused();

    /** @brief 查询服务是否已完成启动 */
    bool IsRunning() const {
        return running_.load(std::memory_order_acquire);
    }

    /** @brief 查询联网业务是否仍在占用或归还音频，供待机等待收尾 */
    bool HasNetworkWork() const;

    /** @brief 查询全部音频占用、工作线程及在途释放，供待机排空 */
    bool HasWork() const;

private:
    AudioRuntime() = default;
    void ReleaseClients(uint32_t clients);

    void DropLocalPlayback(bool release_hardware);

    AudioService service_;                      // 共享采集、播放和编解码服务
    std::recursive_mutex mutex_;                // 串行化服务启动、占用和释放
    std::atomic<uint32_t> clients_{0};          // 调用方占用位，收尾前保留联网调用方标记
    std::atomic<void*> local_playback_owner_{nullptr};  // 受音频服务锁保护的本地播放归属
    std::atomic<bool> running_{false};          // 启动完成状态
    bool initialized_ = false;                  // 编解码器与硬件接口是否已绑定
    std::atomic<uint32_t> release_pending_{0};  // 尚未交付的释放请求
    std::atomic<bool> release_queued_{false};   // 合并释放任务
};

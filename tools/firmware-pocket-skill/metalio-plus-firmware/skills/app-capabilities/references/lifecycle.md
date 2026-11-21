# 取消、真正停止、释放与保存

`assets/plus_services/task_fence.h` 提供TaskFence与可移动不可复制Lease，不替代FreeRTOS工作任务。只在实际有异步操作时使用；保留宿主自行调度能力。

- TryStart取得lease并增加活跃数。RequestStop关闭新任务并作废当前代次；不杀线程。
- worker定期查lease.Cancelled()，SDK IO必须有超时；取消同步网络调用后仍须等它真正返回才能释放使用中的PCM。
- 必须先构造lease，再构造请求资源；析构顺序使缓冲和临时对象先释放，再发布任务结束。推荐 RunTracked(fence, lambda)，请求资源都建在lambda内。跨worker转交缓冲时，接收方的lease与所有权必须在发送方结束前建立。
- CommitIfCurrent在与取消相同的锁下发布结果，避免“检查未取消→被取消→仍插入旧文字”的竞态。发布回调只做有界内存操作/非阻塞投递，不重入fence。
- 控制器RequestStop→取消/停止硬件→WaitStopped(timeout)→保存→交接。不是等待UI不显示“工作中”，也不能从被等待的worker自身等待。
- StopThenSave封装该顺序；失败保持关闭，不假报保存或清理完成。恢复编辑时先恢复真实驱动与口袋CanEdit条件，再显式Resume。
- TaskFence必须活得比所有lease长。例子只管理归入fence的任务；I2S DMA、无线driver、文件同步、显示物理完成仍须各自确认，不能仅靠active=0推断。
- 定时Wi-Fi刷新不持续重置用户空闲计时；用户主动录音/播放/长计算可报告长任务状态并依产品规范暂停自动休眠，任务结束重置计时。普通锁屏与真正深休分别处理。

可复用接线：
```cpp
plus_services::TaskFence tasks;
// controller启动一个真实worker后，该worker内：
plus_services::RunTracked(tasks, [&](const plus_services::TaskFence::Lease& lease) {
    RequestResources resources; // 应用实际的RAII资源类型；在此作用域内销毁
    // 有界工作、取消检查、结果身份检查……
    tasks.CommitIfCurrent(lease, [&] { /*有界发布到匹配文档/页面*/ });
});
// controller在返回/深休请求时：
bool saved = plus_services::StopThenSave(tasks, std::chrono::milliseconds(5000),
    [&] { return StopActualDevices(); },
    [&] { return SaveActualBusiness(); });
```
以上两处应用回调表示应用自己的业务/驱动，不能用恒true代替；实际可运行的TaskFence竞态检查在tests/test_services.cpp。

服务失败隔离：未配置、后台离线以状态提示；用户实际请求失败提示一次。关闭提示不重置去重触发无尽弹窗；保留键盘、保存和返回。缺后端不上传、不伪造STT。主机取消/所有权检查不等于STT端到端成功。


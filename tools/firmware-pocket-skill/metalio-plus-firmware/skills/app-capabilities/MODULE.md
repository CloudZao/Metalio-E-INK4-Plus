# 按需组合的应用能力

适用：Windows 原生 Codex、Metalio Plus 4.7 / ESP32-S31。它是应用能力箱，不是固定记事本框架；不必全读或全部编译。默认仍由主流程完成需求、UI适配、双模式与交付。

先按需求选一行，只读对应页及接口头：

| 用户需求或架构触发点 | 读取与可调用内容 | 何时接线 |
|---|---|---|
| Wi-Fi、天气、网页、网络服务 | [设备端配网](references/network.md)，包内真实公共配置/无线服务；[网络小例](examples/network_device_setup.h) | 架构时明确独立配网与口袋只读配置；页面实现时接扫描/输入/连接 |
| 少量文本、游戏状态、设置 | [持久化](references/storage.md)，TextStore、CounterState、[实际桥接钩子](examples/direct_tf_hooks.cc) | 选定存储模式后、业务可编辑前 |
| 后台网络、音频、计算，需要取消/返回/深休 | [任务生命周期](references/lifecycle.md)，TaskFence、StopThenSave | 第一个后台任务创建前；不为同步小应用加线程 |
| 音频叠加Wi-Fi、编码或大画面 | [资源预算](references/resources.md)，DmaBytes/ReadMemorySnapshot、[共享轨示例](examples/shared_audio_reset.cc) | 分配前与组合变化时 |
| 录音/声音/动画/线稿 | [声画窄模块](references/media.md)，PCM、部分写入、真实帧回执、[转换例](examples/media_pipeline.h) | 只在相应需求中；业务完整后再做小范围声画标定 |

这些头文件位于 `assets/plus_services`，无 SDK/工具链大包。按需复制到工程、加入头目录；`pocket_core.cc` 全工程仅编译一次。联网还编译包内 `../pocket-integration/assets/wifi-standard` 两个 .cc，不能把此目录当成已配好的用户界面。

- DIRECT_TF 示例使用同一业务根实现独立/集成共档；CHECKPOINT 仍按 [平台桥接](../pocket-integration/references/platform-bridge.md) 实现本机持久化和 run 归属，不能把 TF 示例改个枚举冒充 CHECKPOINT。
- 停止、保存、返回是不同成功点；任何失败都保留草稿/旧档，不伪造 saved。普通锁屏可按需求保留长任务，深休及返回先协调退出；自动休眠例外由产品规范和应用的长任务状态决定。
- 本地小例是实现与启发，不包含任何用户答案。用户自由需求可组合/替换，约束的是设备合同、实际资源和可靠性。
- [来源、许可与证据](references/source-license.md) 区分继承经验、此次实现及实机待验收。源项目路径仅为制作端来源记录，不是运行依赖。

主机验证：用已安装 Windows MSVC 运行 `scripts/run_host_tests.py --output <工程外部验证目录>`。需要 Python 标准库和 MSVC C++17；不自动下载编译器。该检查不接USB、不连接Wi-Fi、不调用外部服务，不证明新固件真机成功。


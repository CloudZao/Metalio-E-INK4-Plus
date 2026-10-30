# Metalio E-Ink4 Plus：低功耗与蓝牙音频

## 低功耗 / Low power

进入待机与退出待机的软件操作、硬件断电与恢复顺序、定时唤醒、失败保护及 S31 SDK 兼容处理统一维护在[低功耗策略（中文）](../common/power_policy/README.md)及其[英文版](../common/power_policy/README.en.md)。国内版和国外版复用该硬件流程；引脚与电源控制定义仍以板型源码为准。

Software actions on standby entry/exit, hardware power removal and restoration, timer wakes, failure protection, and S31 SDK compatibility are documented in the [low-power policy](../common/power_policy/README.en.md) ([中文](../common/power_policy/README.md)). Domestic and global variants share this hardware sequence; board sources remain authoritative for pins and power controls.

## 蓝牙音频初始化

开机由独立后台任务先复位 BH，再下发模式1配置；复位、电源稳定等待、应答等待和重试均不阻塞主事件循环。
只有收到明确的 `SET MODE 1` 应答才判定就绪，串口写入成功或收到任意数据不代表初始化成功。
首次失败后最多重试3次，每次间隔1秒；同一电源周期重试耗尽后保持失败，不因页面轮询无限重试。
重新上电会使上次确认结果失效。AI、录音测试和老化测试均在后台等待模式确认，再按需申请 I2S 资源。
离开音频功能后释放 I2S 通道与 DMA；蓝牙页面替换数据回调不会移除初始化流程的应答观察者。

初始化失败仅保留失败阶段和尝试次数等必要错误信息。

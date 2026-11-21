# 单 BIN 双模式

本模块实现 [公开指南 1.1](references/public-guide-1.1.md) 的 FPOCKET ABI 1.0；设备绑定为 Plus / ESP32-S31 / `metalio16m-plus-pocket2`，构建检查 profile 为 `public-plus-pocket2`。不按产品版本准入；不同物理绑定仍必须单独适配。旧 local107 控制记录不属于本模板支持范围。

指南快照保持上游原文，其中“上层原则”的相对链接属于上游仓库，并非本包文件。接入所需接口已完整包含在快照中；查阅上层原则时使用[固定版本的上游原件](https://github.com/Changkai9888/CloudZao-Projects/blob/59e523c9ea5c3f0064a506f06c84a526f08b4379/Skill生成固件的最小集成兼容协议_v0.3.txt)，不必为构建额外下载它。来源身份见 `references/sources.json`。

同一 BIN 独立从 0x0 烧录，bootloader 0x2000、分区表 0x8000、factory app 0x10000；口袋仅把应用载荷写到 OTA1 0x300000，最大 13 MiB。资源内嵌，不覆盖管理器、公共身份或引导。

- 标准 run/receipt 为严格 JSON；image_bytes/storage_mode/state_schema 仅由实际镜像派生，不能额外塞入 JSON。
- 初始化核对实际布局、完整镜像与 metadata、管理器身份、公共用户及 active run；业务恢复、必要初始化和系统启动确认成功后写 started。物理首屏是本设备模板的附加初始化门。
- 返回先暂停业务、保存/同步/校验，提交 saved 后再复核管理器并跳转。失败继续编辑前必须提交 started；独立模式保存后留在应用。
- NONE 不造业务占位档；CHECKPOINT 实现身份归属、恢复与退出导出；DIRECT_TF 同步原文件即可。需要业务存储时，应用必须自行检查并按需创建合法业务目录，不得依赖固件口袋预先创建。首次无存档与真实存储错误应分别处理。坏档、缺失、IO、未知版本分开处理。具体遵循[标准接入实现](references/standard-integration.md)及[应用存储经验](../app-capabilities/references/storage.md)。
- Wi-Fi 可选。标准共享数据采用原始 SSID/密码，候选绑定当前 run，正常返回后的采用由宿主负责。应用不写公共身份、wifi_current、run、wifi_result。

先读 [接入说明](references/standard-integration.md)、[平台桥接](references/platform-bridge.md) 和 [CMake 接线](references/cmake-wiring.md)。可复用 [核心](assets/pocket-standard/pocket_handoff.h)、[桥接](assets/plus_pocket_bridge.h) 和 `assets/wifi-standard/`。后台业务先按 [生命周期](../app-capabilities/references/lifecycle.md) 停止修改再保存；桥接 mutex 不替代业务同步。

构建及主机检查与真机验收分开。最终用同一 SHA256 BIN 分别验证独立及口袋导入、启动、保存、返回、再进入；不能把规范声明、历史宿主验收或模板编译当成新应用真机通过。

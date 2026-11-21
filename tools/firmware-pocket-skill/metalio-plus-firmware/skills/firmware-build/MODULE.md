# Plus 工程与构建

输入：已确认需求/UI、运维模块返回的 Python/SDK 激活文件与原始缓存路径、独立 WORK。输出：用户业务源码、实际构建日志、`flasher_args.json` 和分段 BIN。

## 先读这些结论

- ESP32-S31 / `esp32s31`，SDK 6.1.0 固定提交 `fff9895c82d744c7237be8847347bdd1b07c6643`。Windows 原生构建，不以 WSL/Bash 为前提。所有中间产物在 WORK，短 ASCII 构建路径避免工具链路径问题。
- 默认目标是同一份自包含的独立/口袋 BIN。**官方整机工程是驱动、接口与官方玩法的来源，不是直接可导入口袋的应用**：其外置 `font_data/resources` 和 5MiB OTA 布局不同。先读 [双模式接入](../pocket-integration/MODULE.md)，再选工程形态。
- 已有合适 Plus 工程优先保留其业务。新应用默认先按 [可复用 Plus 工程底座](references/plus-app-base.md) 用 `scripts/create_plus_project.py` 建立独立 WORK，再实现用户业务。底座含实际显示、触摸、按键/震动、电源和口袋接线，避免重新拆解整套官方 HAL；默认硬件探针不是用户应用。例外及官方接口索引见 [工程路线](references/project-routes.md)。不要复制无关桌面、云语音、旧应用存档或作者路径。
- 显示/输入/电源读 [设备模块](../device-interaction/MODULE.md)，按需求读文字、联网、存储等模块。实现方式可灵活，目标芯片、启动/存储/返回约束及产物校验不省略。

## 推荐推进

1. 前置确定存储模式和双模式约束；生成底座，检查 generic 与 S31 默认值及最终生效配置。有USB及本次授权时先做基础真机验证；没有USB也继续实现已确认UI和真实业务，保留硬件待验项。探针成功不能替代业务、字库或双模式验收。
2. 在运维返回的环境中执行目标构建；命令与关键差异见 [原生 Windows 构建](references/windows-build.md)。先定位第一个真实错误，不把后续连锁错误当多个故障。
3. 扩展需求并复测相关行为；卡在编译时按 [已知故障](references/build-failures.md) 找触发条件，不一次载入完整历史经验。
4. 将真实分段交给 [验证与交付](../verify-delivery/MODULE.md)。完成必要业务检查，不以空壳工程、官方原版改名或仅元数据通过代替用户固件。

需要固定官方工程时才读 [官方工程准备](references/official-preparation.md)，执行 `scripts/prepare_official_project.py`。该脚本会选择 798 个核验后的官方文件并加入明确标记的开发补全；原始归档不变。此选材与补全是本 Skill 的经验，不属于基线环境。

无法执行命令、缺目标所需凭据、材料损坏或确认的外部阻塞应及时报告；普通实现/日志定位由 Agent 持续推进。新 SDK、加密或签名固件超出本版固定构建/检查工具的范围，不能通过删校验伪装支持。

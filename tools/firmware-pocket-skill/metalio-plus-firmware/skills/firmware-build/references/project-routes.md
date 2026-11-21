# 工程形态与必要材料

## 默认：自包含的 Plus 应用

适用于记事本、天气、游戏、浏览器、录音等单应用需求。默认执行 [Plus 底座生成器](plus-app-base.md)，获得不依赖作者工作区的实际显示/输入/电源/口袋基础，再按需增加业务。它不固定记事本，默认接入Noto中文字库与中文交互骨架；联网/录音等业务按路由组合，不预置整套应用答案。对已有合适工程保留其业务与已验证接法。仅当底座没有所需接口、用户明确改官方玩法或要核验版本差异时再准备官方工程。固定归档的板级代码在 `main/boards/metalio-e-ink4-plus/`：`display/` 为 EPD/LCD/波形，`bus/` 为 I2C/PCA9555/TPS65185，`input/` 为按键/震动，`periph/` 为前光/音频/SD/电源；共同电源和存储在 `main/boards/common/`。配套读取根 CMake、main CMake、Kconfig、sdkconfig 与 `sdkconfig.defaults.esp32s31`。不要误用历史应用重组后的 `components/epdiy` 路径作为官方归档路径。

- 原生屏幕 1216×684；竖屏逻辑 684×1216。驱动坐标、UI 坐标、触摸变换明确区分。
- 从固定官方配置保留 S31、16MB Flash、PSRAM、LCD/EPD、I2C/SD 的板级配置，再去掉未用业务。用组件 CMake 的 `REQUIRES/PRIV_REQUIRES` 明确依赖。
- 字体/图像/音效链接进 app。引入资源应检查 CMake 的 `EMBED_FILES/EMBED_TXTFILES` 与符号、真实生成顺序；不能遗留对官方外部分区的隐含读取。
- 配置 [独立分区](../../pocket-integration/assets/standalone-partitions.csv)，应用 payload 最大 13MiB；最终 BIN 只有 64KiB 启动前缀加实际 app，不填充整个槽位。
- 元数据 `app_id` 同工程名，`version` 同 `PROJECT_VER`，UI 退出入口同声明。按双模式桥接绑定启动确认、存档和返回；单独嵌入声明不够。
- 业务真实不需要 LVGL、音频或联网时，不为凑“全能底座”引入。图形栈选择应满足已确认 UI，而非强制每个应用共用一种框架。

## 官方整机玩法路线

用户改官方整机玩法或需审查现有驱动接口时，可执行官方准备脚本。固定官方布局包含 bootloader `0x2000`、分区表 `0x8000`、应用 `0x80000`、资源与字体等段，以构建生成的 `flasher_args.json` 为准。

该布局与口袋紧凑布局不同；若仍要求双模式交付，要将必要玩法/服务移入自包含 app。不要把官方完整 BIN、删除字体后的残缺 BIN、或仅一个 app 段冒充双模式最终文件。

## 源码来源与边界

官方项目：[CloudZao / Metalio-E-INK4-Plus](https://github.com/CloudZao/Metalio-E-INK4-Plus/tree/ad8edf4150a688737c0d7c00cae4413079b262ef)。SDK：[ESP-IDF 固定提交](https://github.com/espressif/esp-idf/tree/fff9895c82d744c7237be8847347bdd1b07c6643)。大材料从运维模块的同源注册表取得，必要时查对应版本官方文档；不依赖本 Skill 作者的工作区。

本包提供接口经验和有限复用组件，不承诺任意需求零修改即可编译。Agent 应根据业务组合实现并验证；编译、结构、真机三层证据分开。

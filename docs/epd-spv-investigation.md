# SPV 85 Hz 尝试记录

## 当前实现与实测结果

当前版本在清晰档的 48 相完整刷新和 49 相黑白切页中重复测得约 85 Hz，用户确认显示正常。波形、相数、192 行预填与 20 MHz 像素时钟保持不变；连续操作仍可能出现较长调度间隔，不能将目标节拍解释为任意负载下的硬实时保证。

- 两个供数线程完成当前相后，更新任务才重建下一相 LUT，并将下一相数据追加到原有 FIFO；当前扫描只读取已转换的数据。扫描计数与供数相位分开维护。
- 满队列通过信号量阻塞与按需唤醒；预填与供数结束分别确认，取消后等待全部已启动线程退出再复用队列和 LUT。
- 正常完成在通知更新任务前屏蔽 DMA EOF，下一相重置并清除旧中断状态后重新启用。LCD 与 DMA 中断保持同核；取消等待中断处理退出后才清回调，避免旧 EOF 跨相重填。
- 渲染器从驱动读取扫描行数。适用 S31 配置扫描原生面板高度，末尾八行 bounce 块中超出高度的部分填无驱动数据。
- LCD 与 CKV 采用同一精确周期，当前为 16.9 µs；数据窗口与 CKV 低电平下限不缩短。扫描参数以 [`lcd_driver.c`](../main/boards/metalio-e-ink4-plus/display/output_lcd/lcd_driver.c) 和板级配置为准。
- S31 差分转换复用既有四段 256 字节预移位 LUT，总容量仍为 1 KB；对齐热循环、前后尾段及自检使用同一布局。转换与掩码处理保持分离。
- 公共重绘调度在物理更新期间合并保留请求，结束后绘制最新目标；适配层统一协调实际绘制与更新的开始时机，输入、取消和结果回收继续执行。资源准备与生命周期约束见[显示说明](../main/display/README.md#绘制调度与资源准备--rendering-and-resource-preparation)。
- 每次刷新在首相预填完成后确定固定起点，按 `ceil(k × 1000000 / 85)` 计算各相目标时间。单次迟到不再推迟后续全部目标；仍须先等待上一相扫描完成和本相两份预填信号，不跳相。

### 设备结果与范围

2026-11-02，分段 LUT 与固定节拍版本的 ELF SHA 前缀为 `3ece356a5`，写入哈希校验和启动像素自检通过。清晰档（`profile=0`）两次启动测量：

- 完整刷新：85.016 / 85.013 Hz，48 相全部完成，每次 47 个间隔样本。
- 黑白切页：85.005 / 85.004 Hz，49 相全部完成，每次 48 个间隔样本；后续切页另测到 85.002 Hz。
- 普通启动刷新：重启复测为 85.000 / 85.001 Hz，20 相全部完成。其他普通刷新随并发状态有所波动。
- LCD 扫描约 11.620 ms；上述启动采样中的刷新均为 `recover=0`、`first_err=0x0`、`err=0x0`，未捕获欠载、超时或栈余量告警。用户确认最终版本显示正常；均衡档、灰阶专项、引脚测量和长期音频并发没有在此版本单独验收。

持续操作期间，普通 20 相刷新还捕获到 70.786、52.507、58.991 Hz，最长相位启动间隔 150.176 ms；对应扫描仍约 11.62 ms 且相数完整。该调度尖峰尚未定位，不能因完整刷新达到 85 Hz 而宣称所有入口和并发场景均稳定达到 85 Hz。后续应单独追踪更新任务、供数线程的唤醒及系统临界区、缓存与业务任务协作。

### 连续操作日志补充

后续用户提供的运行日志包含 176 条 SPV 记录，其中四条为 53～57 Hz，另有一条 61.350 Hz。这五次均完整完成且未恢复：`seq=180` 普通 20 相为 53.054 Hz；`seq=254/430/485/493` 黑白 49 相分别为 56.929/56.595/61.350/56.351 Hz。扫描仍约 11.621 ms，变长的是相邻启动间隔，最大分别为 30.002/127.836/129.038/128.551/130.786 ms。以 `seq=493` 为例，48 个间隔累计比 85 Hz 预算多约 287 ms，不是一个尖峰即可解释。

四次低频黑白切页窗口内均伴随连续主页切换。慢 UI 回调按当时 ELF 解析为 `ScreenPaintCoalesceAsync`，其中两次记录到 83 ms、241 ms，UI 整轮耗时可达 508 ms。代码中 `epd_refresh` 与 `lv_epdiy_tick` 同为核心 1、基础优先级 2；当时主页重绘会清理并重建控件，且未开启已有的忙时延后选项。这些是 UI 与刷新竞争的明确线索，但不能仅据日志断定具体阻塞者或优先级继承已经发生。CPU 日志保持 320 MHz、自动轻睡关闭；频率配置日志和正常 SD 日志 flush 本身不能解释这些相间间隔。

另有独立的真实欠载：`t=395709 ms`、`seq=396` 的第 17/49 相在第 407 行触发 `reason=2`；队列 1 为 `3/3`（空），队列 0 为 `116/4`（尚有 112 行），EOF 间隔仍为 136 µs。原波形只完成 16 相，随后清屏及完整刷新恢复，`recover=1 first_err=0x400 err=0x0`。这不是正常低频记录，必须保留首次错误；同期存在配置 HTTP 返回及 RTC 校时，但尚未证明是网络或 NVS 写入导致。

### 自动切页硬件对照

已在同一设备烧录限频诊断版（ELF `7cf0c7a6f`），在 UI 线程每 160 ms 请求翻页，依次运行四段：原行为、忙时延后、忙时延后、原行为。每段约 12 s，结束恢复原页码和开关；统计排除跨段及边界刷新，只取完整无恢复的 49 相黑白切页。各段加权平均分别为 76.12、84.81、84.79、80.43 Hz，最低分别为 69.591、84.697、84.561、71.956 Hz。两段延后的最长间隔约 13.069/12.789 ms；回到原行为后再次出现较长间隔。上述频率来自带计时固件，包含诊断开销，不替代清理后版本的启动测量。

本轮未复现五十几 Hz 的整组均值，但复现了同类 130.305 ms 相间停顿：扫描完成后 owner 推进延迟 118.501 ms，当前相预填等待仅 6 µs，两供数线程均已进入满队列等待，数据早已就绪。另有 128.670 ms 间隔，owner 在供数确认阶段仍未推进，不能把该阶段全部算成纯唤醒等待。UI 持锁时观察到有效优先级 5、释放后回到基础优先级 2；owner 快照有 5/6/22/23，不能仅用基础优先级解释全部调度。

该对照支持在物理更新期间延后重建。当前由公共重绘调度统一合并请求，适配层协调实际绘制与更新的开始时机；资源准备和生命周期约束见[显示说明](../main/display/README.md#绘制调度与资源准备--rendering-and-resource-preparation)。

NVS 诊断仅捕获两次 `open_rw`（150/126 µs），没有证据将本轮长间隔归为 Flash 写入。前者发生于该次扫描结束后的汇总输出期间；后者对应刷新最大间隔约 12.277 ms。只包 Settings、短读阈值和限频均有漏记范围，不能排除其他 Flash 路径。本轮没有发生真实欠载，故原日志 `seq=396` 单侧队列耗尽仍未复现、未认定修复；后续需独立保留其 feeder 提交进度及跨核/cache 事件证据。

验证后已清理阶段计时、NVS/UI 诊断、自动翻页入口及全部专用辅助文件，仅保留主页忙时延后这一项业务修正。交付版 ELF `f3a73761c` 已完成整包构建、写入校验和启动自检，两次启动检查的完整刷新为 85.009/85.001 Hz、黑白切页为 84.851/85.002 Hz，分别完成 48/49 相且无恢复或错误；普通刷新仍捕获到 70.594 Hz、最大间隔 27.091 ms，说明主页修正没有覆盖所有调度波动，不能宣称所有入口均已稳定。

### 分阶段证据与保留选择

1. 流水线和精确扫描、原填充 LUT（`bc0d36f98`）：完整刷新 76.676 Hz，黑白切页 74.350 Hz；普通小范围刷新约 84.687～84.957 Hz。
2. 仅加最小计时的原 LUT（`31d51346f`）：完整刷新 76.244 Hz，预填等待平均 7 µs，扫描完成至更新任务运行平均 1344 µs，下一相通知领先扫描结束平均 2900 µs。说明预填完成后继续供数到队列满，会妨碍低优先级更新任务及时启动下一相。38.13 µs/行/核等周期换算包含等待和调度，不能作为像素转换计时。
3. 单独改四分段 LUT（`2b1fc95c1`）：完整刷新 84.906 Hz、黑白切页 84.959 Hz，更新任务唤醒平均约 10～11 µs，预填等待约 7～9 µs。此次流水线条件下，转换提速已带来端到端收益。
4. 保留分段 LUT，清理详细计时并改固定起点节拍（`3ece356a5`）：结果见上。未增加整相缓冲或改变相数。

各阶段使用相同启动路径，但启动并发和少量界面内容仍有变化，不属于完全相同输入的严格基准。预填后阻塞至开扫的门闩方案未上板，因分段 LUT 已消除主要唤醒延迟而撤回，未遗留额外信号量。12 行 bounce 仍只是未实测候选，当前无需为约 85 Hz 的启动结果继续叠加该调整。

验证覆盖新旧实际函数的普通、changed、DU 构建链、全灰阶、四分段、对齐与尾部、掩码及输出边界；已有启动自检按新 LUT 布局维护。绝对节拍的实际表达式和等待函数通过临时时钟模型校验。此前跨相队列、取消和 IRQ 生命周期验证继续适用。整包构建、烧录和串口验证已完成；临时计时、门闩代码、测试脚本及采样文件均已清理。

## 上一轮范围与结论

2026-11-02，基于 `c81e3f6` 排查 E0470A14-AF 的 SPV 相位频率，目标为 85 Hz，相数不可减少。均衡对应 `profile=1`，清晰对应 `profile=0`。

85 Hz 未实现。黑白切页在最近的均衡与清晰日志中均约为 67.3 Hz；普通刷新的频率随实际参与转换的行、列及调度延迟变化，不能直接用不同画面的最高值比较档位或优化收益。各次采样并非严格控制输入和并发状态的 A/B 测试。

已回退提速尝试和详细计时，恢复基线的 12 ms 相位周期下限、原 LUT 布局、分离的转换与掩码处理、逐相行归属初始化。保留队列等待误报超时的修复，以及按实际 SPV 发出位置统计的最小频率日志。相数、波形、192 行预填和硬件扫描时钟均未调整。尝试代码未提交，回退参照为上述基线提交。

## 测量口径与关键证据

- 每个波形相位发出一次 SPV；频率由同一次刷新中相邻 SPV 下降沿附近的软件时间戳计算。它不是整幅画面的刷新频率，也不是引脚电平捕获结果。
- `scan_us` 从 LCD 启动计至扫描结束，不包含 SPV 到 LCD 启动之间的控制时序。不能把它单独倒数当成实际 SPV 频率。
- 面板参数入口为 [`E0470A14_AF`](../main/boards/metalio-e-ink4-plus/display/displays.c)，扫描实现为 [`lcd_driver.c`](../main/boards/metalio-e-ink4-plus/display/output_lcd/lcd_driver.c)。日志中的行周期为 17 µs，684 行补齐至 688 行；扫描耗时约 11.759 ms。
- 85 Hz 的相位预算为约 11.765 ms，当前扫描已几乎用满预算。若相间准备仍占约 3 ms，仅去掉节拍等待无法达到目标。
- 大范围普通刷新曾出现 `prefill≈2620 µs`、`gap≈2780 µs`、SPV 约 68.4 Hz；黑白推白出现 `prefill≈3208 µs`、`gap≈3388 µs`、SPV 65.740 Hz。LUT 构建约 5～7 µs，节拍等待约 2 µs。额外耗时主要落在预填阶段，而非 LUT 构建或主动限速。
- 合并转换后，黑白推白仍约 64.9 Hz，抽样像素处理约 35～36 µs/行、队列等待约 1 µs、预填约 3.408 ms。改用分段 LUT 后，像素处理约 29～32 µs/行、预填约 2.87～2.90 ms，但黑白切页仍约 67.2～67.3 Hz，未带来明显的最终频率提升。
- 小范围刷新可达约 81～83 Hz，其预填通常仅需数百微秒，不能代表全画面切页。`rows0/1=0` 仅表示预填区域没有抽到需转换的行，不表示整次刷新没有绘制。
- 启动时曾测到供数线程唤醒延迟约 233 ms，以及更新任务唤醒延迟约 70 ms。长间隔与稳定的单相扫描耗时是两个问题；调度延迟的上游来源尚未定位。

## 已回退的尝试

1. **把目标节拍改成 85 Hz，并在预填后按上次 SPV 等待。** 恢复原 12 ms 相位准备入口节拍。设置目标数值不能消除真实扫描和准备耗时。
2. **逐阶段计时。** `EpdPrep` 区分唤醒、供数收尾、LUT、复位、预填、启动锁、节拍和启动；`EpdFeed` 每线程每相最多抽样四条预填有效行。定位结果保存在此处，详细统计结构、时间戳接口和日志已移除。
3. **行归属表只初始化一次、分区 LUT 单次解包。** 减少了重复处理，但未单独证明对实际 SPV 的收益，随提速尝试回退。
4. **合并差分转换与列掩码、按字输出、跳过被掩码屏蔽的数据。** 软件输出等价，不足以证明端到端提速，回退。
5. **灰阶分区跳过未驱动区域。** 仅命中 `MODE_SPATIAL_WIPE_GL`。`wipe-bw` 虽然名称包含推白，实际走 `EpdPackBinaryReveal` → `MODE_DU | MODE_PACKING_1PPB_DIFFERENCE`，不能把灰阶分区优化的收益归到黑白路径。入口见 [`Draw`](../main/display/epd_refresh.cc)。
6. **S31 的 1 KB LUT 改为四个 256 字节预位移分段。** 复用 `build_2ppB_lut_1k`，减少每像素索引缩放和位移，缓存总容量不变；LUT 构建耗时也从约 5～7 µs 增至约 23～25 µs。局部转换变快，最终 SPV 仍未明显提升，恢复原四字节填充布局。

这些尝试曾通过原输出对比，覆盖普通、跳过未变化像素及 DU 构建、灰阶组合、掩码、方向、对齐、尾部与输出边界；其中相应版本完成过整包构建、烧录和串口采样。软件等价与串口时序验证不能替代光学效果或长期并发稳定性验证。临时测试脚本、生成程序和采样文件未保留。

## 保留的明确修复

复测曾出现第 38/49 相超时。随后诊断捕获到另一例：

```text
LCD frame 9/48 timeout err=0x400 consumed=0/688 ready=0 launch=0
feed stop=1/5 q0=64/64 q1=63/64
```

诊断状态 `1` 表示线程进入时已存在错误，`5` 表示队列满后空轮询超过 30000 次。线程 1 已填满其 127 个可用槽，线程 0 尚未开始，预填和 LCD 启动均未发生。这是等待循环次数导致的误报，不是扫描耗时超过硬件能力。

修复位于 [`render_lcd.c`](../main/boards/metalio-e-ink4-plus/display/output_lcd/render_lcd.c)：

- 在本核调度暂停期间发布两份供数通知，缩小启动竞争窗口；不关闭中断，不在该区间等待扫描。
- 两条取队列缓冲路径共用实际时间超时逻辑。仅在队列满时开始计时，等待达到 1 s 才置错，早于更新任务的 2 s 扫描等待上限；取消检查和错误退出保留。
- 错误仍需停机并等待供数线程确认后才能复用队列和 LUT。真实扫描失败不能被延长等待或忽略错误掩盖。

模拟验证覆盖超过 30000 次短等待、真实超时、取消及两核通知发布。包含该修复的实验固件复测未再出现误报，但通知批量发布不能消除所有跨核调度延迟，也不构成长期稳定性保证。临时退出位置标记已移除，保留必要超时错误信息。

## 回退版本验证

回退版本已完成整包构建、共享队列等待测试及渲染生命周期故障注入，并烧录至同一设备，写入哈希校验通过。启动自检通过；清晰档启动全刷为 66.147 Hz（48 相），黑白推白为 66.170 Hz（49 相），对应刷新均完整结束且未触发恢复。实验版本黑白切页约 67.3 Hz，与回退值相差约 1 Hz；这仍不是同等输入的严格 A/B 结果，不能将差值全部归因于某一算法变化。

回退固件的 `EpdTiming` 已恢复为最小 SPV 行及原有相位节奏行，不再输出 `target=85Hz`、`EpdPrep` 或 `EpdFeed`。后续性能判断应从这个回退版本重新采样，不沿用实验版本数字作为承诺值。

## 下次排查顺序

1. 从回退后的代码重新建立基线。固定画面、刷新路径、档位、方向、温度、CPU 频率和并发业务，区分冷启动、后台预热与稳定运行。每次只改变一个因素，比较同等输入下的 SPV 间隔及异常间隔。
2. 保留最小 SPV 日志，需要时再临时恢复阶段计时。先分清 `wipe-bw`、`wipe-gray`、普通差分和完整波形，避免优化未命中的分支；区分每行抽样耗时、双核总工作量与预填墙钟耗时。
3. 追踪长时间线程唤醒延迟的调度、临界区和缓存/Flash 协作来源；当前日志只定位了延迟所在阶段，尚未证明具体阻塞者。不可只提高线程优先级或扩大队列就声称解决。
4. 核对 E0470A14-AF 原厂时序并实测 XCL、CKV、XLE、SPV 的关系。仓库没有找到该面板对应手册，公开检索也未取得可核对资料。PCLK、CKV、数据有效窗口、门极高低时间及尾部时钟必须一起检查，不能只修改像素时钟。
5. 若准备仍串行占用约 3 ms，85 Hz 要求扫描进一步缩短并留出启动余量。另一方向是让下一相 LUT/预填与当前扫描重叠，但现有 LUT、行队列和计数器会复用，必须先明确独立缓冲及所有权；不可覆盖正在扫描的数据，也不可默认增加整相双缓冲。
6. 相数和原波形继续保持不变。只有同等负载下实际 SPV 改善且未引入欠载、超时和显示问题，才保留下一项提速修改。

## English

### Current implementation and measurements

The current firmware repeatedly measured approximately 85 Hz for Clear-profile 48-phase full updates and 49-phase black/white page transitions. The user confirmed normal display appearance. Waveforms, phase counts, the 192-row prefill and the 20 MHz pixel clock are unchanged. Longer scheduling gaps can still occur during continuous interaction; this is not a hard real-time guarantee under every workload.

Both feeders must finish a phase before the owner rebuilds the next LUT and appends data to the existing FIFOs. Scan counters and producer phase are separate. Full queues block on semaphores; cancellation waits for every launched feeder before reusing live data. Normal completion masks DMA EOF before notifying the owner, and the next start clears old pending events. LCD and DMA interrupts remain on the same core; cancellation drains processing before clearing callbacks.

The renderer obtains scan height from the driver. Applicable S31 configurations scan the native height and pad unused rows in the final bounce block with neutral data. LCD and CKV share an exact 16.9 µs period without shortening data-window coverage or the CKV low-time bound; the driver and board configuration are authoritative.

Common repaint scheduling now defers rebuilding during physical updates and retains the latest coalesced target. Adapter rendering admission preserves input, cancellation and result disposal; see the [display documentation](../main/display/README.md#绘制调度与资源准备--rendering-and-resource-preparation). S31 difference conversion reuses four 256-byte pre-shifted LUT banks, retaining the 1 KB allocation. Aligned conversion, front/back tails and self-test share the layout; masking remains separate. The first ready prefill establishes a fixed origin for deadlines at `ceil(k × 1000000 / 85)`. Late starts no longer move all later deadlines. Every phase still waits for the previous scan and both prefill acknowledgements, without skipping phases.

#### Hardware results and limits

On 2026-11-02, the final image had ELF SHA prefix `3ece356a5`. Flash write-hash verification and startup pixel self-test passed. Two Clear-profile (`profile=0`) startup measurements showed:

- Full update: 85.016 / 85.013 Hz, completing all 48 phases with 47 interval samples each.
- Black/white transition: 85.005 / 85.004 Hz, completing all 49 phases with 48 samples each. A later transition measured 85.002 Hz.
- Ordinary startup updates: 85.000 / 85.001 Hz on the repeated boot, completing all 20 phases. Other ordinary updates varied with concurrent work.
- LCD scanning remained near 11.620 ms. Updates in the startup captures above had `recover=0`, `first_err=0x0` and `err=0x0`, with no captured underrun, timeout or stack-reserve warning. The user confirmed normal final display appearance. Balanced-profile checks, dedicated gray tests, pin measurements and sustained concurrent audio were not separately verified for this version.

During continuous interaction, ordinary 20-phase updates also measured 70.786, 52.507 and 58.991 Hz, with a longest phase-start interval of 150.176 ms. Scanning stayed near 11.62 ms and all phases completed. This scheduling spike remains unidentified. Full updates reaching 85 Hz do not establish stable 85 Hz for every entry point and concurrent workload. Investigate owner/feeder wake delays, critical sections, cache access and competing tasks separately.

#### Additional continuous-interaction log

A subsequent user log contains 176 SPV records: four at 53–57 Hz and another at 61.350 Hz. All five completed without recovery: ordinary 20-phase `seq=180` measured 53.054 Hz; 49-phase black/white sequences 254/430/485/493 measured 56.929/56.595/61.350/56.351 Hz. Scanning stayed near 11.621 ms, while maximum phase-start intervals were 30.002/127.836/129.038/128.551/130.786 ms. Sequence 493 accumulated about 287 ms beyond the 85 Hz budget across 48 intervals, requiring multiple delays rather than one spike.

The four black/white windows coincided with repeated home-page switching. The ELF used for that analysis mapped the slow callback to `ScreenPaintCoalesceAsync`; logged executions included 83 ms and 241 ms, with a UI round reaching 508 ms. The refresh and UI tasks share core 1 and base priority 2. In that version, home-page painting destroyed/rebuilt widgets without enabling the available busy-defer option. These are concrete contention clues, not proof of a particular blocking task or priority inheritance. Logs retain 320 MHz with automatic light sleep disabled; frequency-configuration messages and normal SD log flushing do not by themselves explain the gaps.

A separate real underrun occurred at 395709 ms in sequence 396, phase 17/49, row 407: reason 2, queue 1 empty at `3/3`, queue 0 holding 112 rows at `116/4`, and EOF interval 136 µs. Only 16 phases completed before clear/full recovery; the result was `recover=1 first_err=0x400 err=0x0`. Preserve the first error. HTTP configuration completion and RTC synchronization occurred nearby, but a network or NVS cause is not established.

#### Automatic page-switch hardware comparison

A temporary diagnostic image (`7cf0c7a6f`) requested a home-page change every 160 ms on the UI thread. Four approximately 12-second blocks used original/deferred/deferred/original painting, restoring the original page and flag afterward. Excluding mixed/boundary updates and selecting complete unrecovered 49-phase transitions, weighted SPV rates were 76.12/84.81/84.79/80.43 Hz, with minima 69.591/84.697/84.561/71.956 Hz. The deferred blocks had maximum intervals near 13.069/12.789 ms; restoring original behavior brought longer intervals back. These rates include profiling overhead and do not replace measurements from the cleaned firmware.

This run did not reproduce a whole-update average in the fifties, but did reproduce a 130.305 ms phase interval: the owner advanced 118.501 ms after scan completion, prefill waiting was only 6 µs, and both producers were already waiting on full queues. Data was ready. Another 128.670 ms interval found the owner still in feeder-acknowledgement processing, so not all of that duration is pure semaphore wake delay. UI effective priority was observed at 5 while locked and returned to base priority 2 after unlocking; owner snapshots included 5/6/22/23.

The within-image switch and reverse comparison support deferring rebuilds during physical updates. The common repaint policy now applies this behavior across callers. Adapter rendering admission, background preparation and lifetime rules are described in the [display documentation](../main/display/README.md#绘制调度与资源准备--rendering-and-resource-preparation).

NVS profiling captured only two `open_rw` operations (150/126 µs), providing no evidence that a Flash write caused this run's long gaps. One occurred during reporting after scanning; the other update's maximum interval was about 12.277 ms. Settings-only coverage, short-read thresholds and rate limiting leave blind spots. No underrun occurred, so the earlier sequence 396 single-producer starvation remains unreproduced and is not claimed fixed. Investigate its producer progress and cross-core/cache events separately.

Temporary stage, NVS and UI instrumentation, automatic switching and dedicated helper files were removed after validation. Only home-page busy deferral is retained. Delivery image `f3a73761c` completed a full build, write-hash verification and startup self-test. Two startup checks measured full updates at 85.009/85.001 Hz and black/white updates at 84.851/85.002 Hz, completing 48/49 phases without recovery or errors. An ordinary update still measured 70.594 Hz with a 27.091 ms maximum interval, so this home-page fix does not establish stable timing for every entry point.

#### Evidence and retained changes

1. Pipeline and exact scan timing with padded LUT (`bc0d36f98`): full 76.676 Hz, black/white transition 74.350 Hz, small updates about 84.687–84.957 Hz.
2. Minimal profiling with padded LUT (`31d51346f`): full 76.244 Hz, mean prefill wait 7 µs, owner wake after scan completion 1344 µs, and next-phase notification 2900 µs before completion. Continuing to produce beyond the prefill threshold until queues filled delayed the low-priority owner. Effective per-row periods include waiting and scheduling and are not pixel-conversion timings.
3. Four-bank LUT alone (`2b1fc95c1`): full 84.906 Hz and black/white transition 84.959 Hz. Mean owner wake fell to about 10–11 µs and prefill waiting to 7–9 µs, demonstrating end-to-end benefit under the pipeline.
4. Retained banked LUT, removed detailed profiling and adopted fixed-origin pacing (`3ece356a5`): results above, with no full-phase buffer or phase-count change.

These used the same startup paths, but concurrency and small UI details varied, so they were not perfectly controlled identical-input benchmarks. A prefill-to-scan gate was withdrawn before hardware testing because the banked LUT had already removed the main wake delay; no extra semaphore remains. A 12-row bounce block remains an unmeasured candidate and was not added.

Verification covered actual old/new normal, changed and DU builders, all gray codes, four banks, alignment/tails, masks and output guards; the existing startup self-test was updated for the LUT format. Actual deadline expressions and waiting functions passed temporary clock-model checks. Earlier queue, cancellation and IRQ lifecycle checks remain applicable. Full builds, flashing and serial checks completed. Temporary instrumentation, gate code, scripts and captures were removed.

### Previous scope and decision

This investigation started on 2026-11-02 from `c81e3f6`, targeting an 85 Hz SPV phase rate on E0470A14-AF without reducing phase counts. Balanced is `profile=1`; Clear is `profile=0`.

85 Hz was not achieved. Recent black/white page transitions remained near 67.3 Hz in both profiles. Ordinary updates vary with the rows and columns converted and with scheduling delays. Different images and concurrent workloads were not a controlled A/B benchmark.

The speed experiments and detailed profiling have been rolled back: the original 12 ms phase-start pacing, padded LUT format, separate conversion/masking and per-phase row ownership initialization are restored. The queue false-timeout fix and minimal SPV measurement remain. Waveforms, phase counts, 192-row prefill and hardware scan clocks were unchanged. No experimental commit was created; the rollback reference is the baseline above.

### Measurements and interpretation

SPV frequency uses software timestamps immediately before successive falling edges within one update. It is a waveform phase rate, not a full-image rate or a captured pin measurement. `scan_us` measures LCD start to scan completion and excludes the preceding SPV-to-LCD control sequence.

The configured panel entry is `E0470A14_AF` in `displays.c`; scan timing is in `lcd_driver.c`. Logs showed a 17 µs line period, 684 rows padded to 688, and approximately 11.759 ms scanning. This nearly consumes the entire 11.765 ms budget for 85 Hz before preparation is included.

Large ordinary updates showed about 2.620 ms prefill, a 2.780 ms inter-scan gap and 68.4 Hz SPV. Black/white reveal showed 3.208 ms prefill, a 3.388 ms gap and 65.740 Hz. LUT building took about 5–7 µs and cadence waiting about 2 µs, identifying prefill as the main extra interval.

After fused conversion, black/white reveal still ran near 64.9 Hz: sampled pixels took 35–36 µs per row, queue waiting about 1 µs and prefill about 3.408 ms. Banked LUTs reduced sampled pixel work to about 29–32 µs and prefill to 2.87–2.90 ms, but page transitions remained near 67.2–67.3 Hz. Small updates could reach 81–83 Hz and are not representative of full-page transitions. Zero sampled rows means no convertible rows were sampled in the prefill prefix, not that the update drew nothing.

Startup also produced approximately 233 ms feeder wake delay and 70 ms update-task wake delay. These are distinct from the stable hardware scan duration; their upstream scheduling causes remain unidentified.

### Rolled-back experiments

1. Set an 85 Hz cadence and wait after prefill relative to the previous SPV. Restored the original 12 ms phase-start pacing; changing a target does not remove work.
2. Added `EpdPrep` stage profiling and `EpdFeed` sampling of at most four drawn prefill rows per feeder per phase. Findings are retained here; detailed counters, timestamp interfaces and logs were removed.
3. Initialized row ownership once and unpacked spatial LUT bytes once. No isolated end-to-end frequency gain was demonstrated.
4. Fused conversion and masking, wrote output words once and skipped masked source data. Output equivalence did not establish sufficient end-to-end gain.
5. Skipped inactive gray spatial bands. This only applies to `MODE_SPATIAL_WIPE_GL`. Black/white reveal uses `EpdPackBinaryReveal` and `MODE_DU | MODE_PACKING_1PPB_DIFFERENCE` in `Draw`, so the gray optimization did not address that path.
6. Reused `build_2ppB_lut_1k` for four 256-byte pre-shifted S31 LUT banks without increasing LUT storage. Pixel work decreased while LUT building rose to about 23–25 µs. The original padded format is restored because final SPV improvement remained limited.

Relevant versions underwent software output comparison across builders, gray pairs, masks, orientations, alignment, tails and output guards, plus firmware builds, flashing and serial timing checks. These do not establish optical quality or long-term concurrent stability. Temporary scripts, executables and captures were removed.

### Retained correctness fix

A reproduced timeout showed `consumed=0/688`, `ready=0`, `launch=0`, feeder stop codes `1/5` and queue positions `64/64` and `63/64`. Code 1 meant an error already existed on entry; code 5 meant the 30000-poll limit expired. One feeder filled its 127 usable slots before the other began. No LCD scan had started, establishing a polling-count false timeout.

`render_lcd.c` now publishes both feeder notifications while local scheduling is suspended, with interrupts enabled and no scan wait in that interval. Both buffer-acquisition paths share a real-time timeout that starts only when the queue is full and fails after 1 s, before the owner's 2 s scan wait. Cancellation, stopping the scan and waiting for feeder acknowledgements before reusing live resources remain required.

Simulation covered more than 30000 short polls, actual timeout, cancellation and notification batching on both cores. Experimental firmware containing the fix did not reproduce the false timeout during follow-up startup checks. This does not eliminate cross-core scheduling delays or establish long-term stability. Temporary feeder exit markers were removed; necessary timeout error information remains.

### Rollback verification

The rollback firmware completed a full build, shared-queue wait tests and render lifecycle fault injection. It was flashed to the same device with hash verification and passed startup self-test. Clear-profile startup measurements were 66.147 Hz for the 48-phase full waveform and 66.170 Hz for the 49-phase black/white reveal, both completing without recovery. The experimental page rate near 67.3 Hz differs by about 1 Hz, but these are not controlled equivalent-input A/B results and cannot isolate a particular algorithm's contribution.

The rollback firmware retains a minimal SPV line and the original phase-cadence line. It no longer prints `target=85Hz`, `EpdPrep` or `EpdFeed`. Establish future measurements from this rollback rather than treating experimental numbers as promised performance.

### Next investigation

1. Establish a fresh baseline from the rolled-back code. Fix image, path, profile, orientation, temperature, CPU frequency and workload; separate cold startup, background warmup and steady operation. Change one factor at a time.
2. Start with the minimal SPV log and temporarily add stage measurements only as needed. Distinguish black/white reveal, gray reveal, ordinary difference and complete waveforms, as well as per-row samples, summed dual-core work and prefill wall time.
3. Trace scheduler, critical-section and cache/Flash coordination behind long wake delays. The blocking component has not been identified; priority or queue changes alone are not proof of a solution.
4. Obtain the E0470A14-AF manufacturer timing specification and measure XCL, CKV, XLE and SPV together. No matching manual was found in the repository or public search. Check PCLK, CKV, data windows, gate high/low times and tail clocks together before changing hardware timing.
5. If preparation remains about 3 ms and serial, scanning must shrink further to fit 85 Hz with startup margin. Overlapping next-phase preparation with scanning is another option, but the shared LUT, queues and counters require explicit independent storage and ownership. Do not overwrite active data or assume full-phase double buffering is affordable.
6. Preserve phase counts and waveforms. Retain a future speed change only when equivalent-load SPV measurements improve without underrun, timeout or display regressions.

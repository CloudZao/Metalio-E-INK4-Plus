# 屏幕显示与刷新设置

ESP32-S31 的 LCD 输出仅启用数据阶段，每行 DMA 数据从首字节开始承载有效像素，不添加行首占位字节；有效数据窗口按实际行宽配置。面板数据结束后的空时钟仍由板级时序配置控制。每相在 SPV 拉高后按 CKV 高电平配置等待，再启动 LCD 同步输出 XCL、XLE 和 XSTL，不额外等待一个完整行周期。

ESP32-S31 LCD output uses only the data phase. Each DMA row carries active pixels from its first byte, with no leading dummy byte; the active data window matches the row width. Post-data clocks remain controlled by the board timing configuration. After SPV rises, each phase waits according to the CKV high-time configuration, then starts the LCD output for XCL, XLE and XSTL together, without an additional full-line delay.

待机或关机壁纸解码达到 1 秒且仍未完成时，在原有画面上显示顶部“壁纸正在加载中...”提示；解码在 1 秒内完成时直接显示壁纸。加载期间保留原有方向及提示区域外的像素。待机壁纸直接绘制到面板帧缓冲，保留底层页面的方向和尺寸；定格后暂停 LVGL 绘制，恢复页面资源后再开放绘制。加载提示出现前保存底图，解码失败时直接恢复底图；阅读字体在待机内容就绪后才释放。唤醒重绘前将物理目标帧缓冲置白，避免全屏壁纸像素留在普通 UI 的内缩范围之外。

If a standby or shutdown wallpaper is still decoding after one second, a loading notice appears at the top of the existing frame. Faster decodes display the wallpaper directly. Loading preserves the current orientation and pixels outside the notice. Standby wallpapers draw directly into the panel framebuffer while retaining the underlying page's orientation and dimensions. LVGL rendering pauses after parking and resumes after page resources are restored. The frame is saved before showing a notice and restored directly if decoding fails. Reader fonts remain available until standby content is ready. Before wake redraws the page, the physical target framebuffer is filled with white so fullscreen wallpaper pixels cannot remain outside the normal UI bounds.

待机、关机和阅读背景解码只从内存缓存解析壁纸选择，不在使用 PSRAM 栈的解码任务中读取 NVS。启动时先灌入选择；SD 尚未就绪时保留该选择，路径暂不可用则沿用各入口的失败回退。

Standby, shutdown, and reading-background decoders resolve wallpaper selections from the in-memory cache without reading NVS from tasks with PSRAM stacks. Startup hydrates the selections first. A selection is retained while SD is unavailable; an unavailable path uses the existing fallback for that entry point.

开机首页准备完成后，先对全白帧执行一次完整 48 相 GC16 全刷，再推白显现首页。全刷清理阶段不绘制首页；保留边框在此次全刷中只推白，成功后恢复不驱动边框。刷新失败仍执行原有清屏恢复，后续时钟、电量和网络状态更新沿用普通刷新策略。

Once the home screen is ready, startup first performs one complete 48-phase GC16 refresh to white, then reveals the home screen with a wipe. The home screen is not drawn during cleaning. The reserved border receives only whitening pulses during the full refresh and remains undriven after success. Refresh failures retain the existing clear-and-recover path. Later clock, battery and network updates use the ordinary refresh policy.

入口：**设置 → 屏幕**。刷新模式与黑色深度独立保存，重启后恢复。刷新选项分两行：清晰 / 均衡、快速 / 极速；电压和波形相数只在本说明中列出，设备界面不显示。

普通 App 页面切换使用从左向右的 10 条窄条推进（随屏幕旋转），一次连续驱动完成擦白和新页显示。清晰 / 均衡的擦白和黑白重绘使用完整 20 相 DU，快速使用 8 相短 DU；极速恢复8相短DU擦白和8相短DU重绘；10 个条带各错开 1 相启动。纯黑白应用切页：清晰 / 均衡为 20+20+9=49 相，快速为 8+8+9=25 相，极速为8+8+9=25相。任一页面含灰阶时，条带擦白后立即使用当前档位 GL16 绘制新灰度：清晰 20+48+9=77 相、均衡 20+37+9=66 相、快速 8+37+9=54 相、极速8+30+9=47相。白到白像素保持不驱动，成功后才提交新页作为面板基线；灰度与条带延迟分别保存。擦白只属于应用切页动画，普通 DU / GL 更新不必预先擦白。缺少动画缓冲或动画失败时回退完整 GC16。实际耗时和残影需上机观察。

### 四种刷新模式

进入待机、退出待机和关机共用开机的清理顺序：先用完整 48 相 GC16 将旧画面全刷至白底，再推白显示目标画面。白屏清理中的白到白像素也执行完整调理脉冲，不因像素相同而跳过。两个阶段由同一刷新任务顺序完成，复用已有动画缓冲，不增加全屏缓存；清理失败时停止后续绘制，待机和关机继续沿用原有失败重试。待机定格或断电须等待目标画面完成，唤醒前光在目标画面完成后恢复。该流程增加等待时间并产生可见闪屏，普通页面切换和壁纸加载提示沿用原有策略。

Entering or leaving standby and shutting down use the same cleaning sequence as startup: refresh the old frame to white with the complete 48-phase GC16 waveform, then reveal the target with a wipe. Equal white pixels retain conditioning pulses during cleaning. One refresh worker runs both stages using the existing animation buffer, with no additional full-screen allocation. A cleaning failure stops subsequent drawing; standby and shutdown retain their existing retry behavior. Parking or power-off waits for the target frame, and wake restores the frontlight after that frame completes. These transitions take longer and visibly flash; ordinary navigation and wallpaper loading notices keep their existing policy.

- **清晰**：优先显示细节与擦除余量。图片和灰阶界面使用完整 16 级灰阶表，GC16 / GL16 各 48 相；黑白局部刷新使用 20 相 DU。适合重视图片层次、字体边缘和残影控制的场景，刷新相对较慢。
- **均衡（新设备默认）**：保留 16 级灰阶，GC16 / GL16 裁剪为 36 / 37 相，黑白局部刷新仍为 20 相 DU。兼顾显示效果和速度，建议作为日常起点。
- **快速**：GC16 / GL16 使用 36 / 37 相裁剪表，保留 16 级灰阶；普通 DU 在按钮、滚动和阅读中使用 14 相表，黑向推 13 相、白向推 14 相；切页擦白和黑白重绘仍使用 8 相短表。速度优先，可能增加残影。
- **极速**：GC16仍使用16级、36相裁剪表；GL16使用8级、30相表；DU各场景恢复8相短表，包括按钮、滚动、阅读、切页擦白和黑白重绘。黑向推7相、白向推8相。灰阶层次减少，可能增加残影。

**完整 GC 清理是四档共同的例外。** 开机、进出待机和关机前的白屏清理、模式切换后的首帧、手动全刷、周期清残影、故障恢复均使用完整 48 相 GC。普通波形测试遵循所选档位，明确要求 GC 深度清理时也使用完整表。刷新相数不等于实际耗时，驱动准备、供电和扫描也占用时间。

升级时保留旧档位的对应行为：原“均衡（完整表）”映射到清晰，原“快速（裁剪表）”映射到均衡；已经保存的新档位不变。当前不使用文字专用 GL16 波形。

### 行供数不足与 GC 恢复

连续切页的波形源表放在 PSRAM，灰阶合并表按档位首次使用时分配并缓存；转换 LUT、扫描行队列及 DMA 缓冲仍使用内部 RAM。这样避免波形大表占用音频 I²S DMA 所需内存；波形分配失败时回退完整 GC，不把空指针交给渲染器。

书架灰底会产生原生灰阶，进入或退出时都可能走灰阶连续切页。该路径的供数热循环强制内联到 IRAM 中的 O3 生产者，使用对齐32位读、批量紧凑LUT查表；纵向屏幕整行复用同一条带表，横向屏幕按连续条带批量转换，仅跨条带边界分别查表。不增加行队列或内部缓冲。若 `reason=2`、所属队列写/读索引相等且DMA回填间隔正常，表示真实供数速度不足，不能当作 ready 标记误报而取消恢复。

扫描中断以行队列的 release/acquire 提交状态判断数据可读，`line_ready` 仅用于开扫前的预填等待，避免生产者在提交队列后、标记 ready 前被中断时误报。真正欠载仍会使面板基线失效，下一帧完整 GC 恢复。失败日志 `stream fault` 记录首个故障：reason=1 为行所有者无效，2 为队列确实为空，3 为扫描提前结束；同时记录相位、行号、队列写/读索引、ready、预填行数及最大 DMA EOF 间隔/回填耗时。`frames` 是目标相数，`completed` 才是成功完成的相数。日志在扫描停止后输出，不在 ISR 内打印，也不扩大行缓冲。

### 绘制与刷新前准备

准备阶段只转换和分析像素，不决定刷新策略。`lv_adapter_epdiy.cc` 调用 `epd_prepare.h` 完成 I1 / L8 到面板 4bpp 的转换；刷新任务负责差异检查、波形选择、扫描与成功后的面板基线提交。按 32 位检查变化、成对写入像素和按条带打包均复用已有缓冲；小块横屏 I1 更新保留实测更合适的标量路径。L8 按面板 X 轴成对量化，直接写入完整字节，奇数边缘保留相邻半字节；灰度量化规则不变，不增加转换缓存。

Preparation converts and analyzes I1 / L8 pixels without selecting refresh policy. L8 conversion pairs pixels along the panel's X axis and writes complete 4bpp bytes, preserving adjacent nibbles at odd edges. Quantization remains unchanged and uses the existing buffers. Small landscape I1 updates retain the scalar path. The refresh worker handles change detection, waveform selection, scanning and baseline updates after successful draws.

默认 I1 软件渲染。`epd_content_policy` 按可见内容的角色申请 L8：书架底座、书籍封面、书内插图、壁纸缩略图/详情预览、照片、开启抗锯齿的正文、助手内容图片。未登记的 UI、应用/操作/天气图标、书签装饰插画保持黑白，与 PNG/L8 等源格式无关。纯黑白旧缓存不会单独触发 L8。对象隐藏/删除、父级裁剪、同页不透明遮罩和 top/sys 遮罩参与模式判断；显示切换在刷新开始、布局完成后执行，重新绑定同一绘制缓冲并使页面重绘。模式策略集中在 `GrayRole()`；可用 `FastMonochrome` 在任意列表/阅读入口容器上强制整个子树走黑白，而不改动原图、缓存或详情预览。

灰阶场景中，普通 UI 在 native blend 前进行源侧黑白转换，内容图片采用统一的显示色调；已合成图层交由 LVGL 继续处理透明混合，flush 不按业务对象补画。原有封面右侧透明裁剪保留。关机/待机壁纸和屏幕测试保留独立输出路径。绘制缓冲至少容纳 I1 全屏及 L8 最大逻辑宽度 × 64 行，I1/L8 与横竖屏共用同一 PSRAM 分配，无全屏覆盖度旁路缓冲。

图片显示色调在 L8 主图层和 ARGB8888 圆角中间图层中采用相同处理，仅在图片绘入时应用；后续图层合成保留透明度，不重复压暗。原图和缓存像素保持不变。

Image tone mapping is applied when an image is drawn into either the L8 main layer or an ARGB8888 rounded-corner layer. Subsequent layer composition preserves transparency without darkening the image again. Source images and cached pixels remain unchanged.

图片解码和 ZIP 流式解码保留原始亮度。新生成的封面旁路/壁纸另存结果/助手图片缓存使用 A2L8（8 字节头 + 原生 L8），原始传输文件直接保留。旧 A2I1 继续读取，不重建迁移；已丢失的灰度不恢复。壁纸覆盖保存保留原路径及启用记录，内容按 magic 识别，另存为采用 `.a2l8`。新缓存有独立大小上限；后台图片只有绑定到可见对象时才影响显示模式。

阅读抗锯齿在正文栅格化时控制字形覆盖度，灰边向实黑加深约 80%；字形重叠合成后按 32 位检查像素块，跳过仅含纯黑和纯白的块，仅处理其余块中的中间灰度，不重复写入纯黑像素。关闭时正文仍为黑白，图片灰度不受影响。`epd_reader_aa` 仅保存刷新波形选择所需的阅读状态，由提交队列取得快照。原有灰阶差分、清屏、滚动收尾和阅读波形策略保留。

Reading text antialiasing preserves glyph coverage during rasterization and darkens gray edges by approximately 80% toward black. After glyph overlaps are blended, 32-bit block checks skip blocks containing only pure black and white; only intermediate grays in the remaining blocks are processed, without rewriting pure-black pixels. Disabling antialiasing renders text in black and white without affecting image grayscale. `epd_reader_aa` stores only the reading state needed for waveform selection; the submission queue captures a snapshot. Existing grayscale differential updates, clearing, scroll completion and reading waveform policies remain unchanged.

清晰 / 均衡的普通 DU 局刷在第 17～20 相对活动脏行与脏列交集中的黑→黑像素补黑四相，总相数仍为 20；快速档普通 DU 在第 11～14 相补黑四相，总相数为 14；极速及 OTA 进度使用的短 DU 在第 5～8 相补黑四相，总相数为 8。应用切页动画保持原有波形。补黑对残影、黑色深度及连续刷新后擦除效果的影响需实屏确认。普通差分刷新在公共 highlevel 局刷入口启用 `MODE_SKIP_UNCHANGED`：逐相构建 LUT 时，DU 保留黑→黑补黑；GL16 对变化后的纯黑目标仅保留原表推黑段，不先推白，黑→黑则复用该表白→黑的最后四次推黑补黑。两者仅在活动脏行与脏列交集内驱动，未变化的灰色和白色像素保持不驱动；GL16 其他变化的灰阶目标沿用原表。源波形表保持只读，不增加相数，也不在逐像素输出热路径增加判断。提交前按实际旧/新缓冲核对普通 DU：只要变化涉及任一中间灰，就改用灰阶波形；静止灰底不影响黑白转换继续使用 DU。全刷、清理恢复、诊断图案、滚动收尾的整帧 GL 和主动擦除动画保留原有调理脉冲。

Ordinary differential updates retain DU black-to-black reinforcement. For changed pixels targeting pure black, GL16 keeps the source table's black-driving pulses and suppresses whitening. Unchanged black pixels receive reinforcement during the selected table's last four white-to-black driving pulses. Both modes operate within the intersection of active dirty rows and columns. Unchanged gray and white pixels remain undriven; other changed GL16 targets use the original table. Source tables remain read-only, with no extra phases or per-pixel output checks. Full refreshes, recovery, diagnostic patterns, full-frame GL scroll settling and erase animations retain their original conditioning. Effects on ghosting, black depth and subsequent erasure require panel verification.

收起文字阅读中已显示的设置卡片时，下一次提交对整页纯黑目标统一使用当前波形白→黑的推黑序列，包括未遮挡正文；开启或关闭抗锯齿均适用。此请求与卡片收起的画面合并，不额外执行整页擦白、增加扫描相数或分配画面缓存。未变化的白色和灰色保持不驱动，其他变化像素正常更新；故障恢复、周期清理等完整 GC 刷新仍优先执行。隐藏已收起的卡片不重复请求。纯黑驱动的一致性不代表灰阶历史状态已统一，残影和区域色差仍需实屏确认。

When a visible settings card closes during text reading, the next submission drives every pure-black target with the selected waveform's white-to-black black-driving sequence, including text outside the former card area. This applies with text antialiasing enabled or disabled. The request merges into the closing frame without an extra full-screen whitening pass, scan phases or framebuffer allocation. Unchanged white and gray pixels remain undriven; other changed pixels update normally. Recovery and scheduled full GC cleaning take precedence. Hiding an already hidden card does not repeat the request. Equal black-driving pulses do not equalize grayscale history; ghosting and regional tone differences require panel verification.

快速波形初始化分两遍处理灰阶转换：先测量最长裁剪序列，再逐组重算并输出。工作区只保存一组转换，初始化完成后不保留整张中间序列表；波形参数与输出动作保持不变。

Fast waveform initialization processes gray transitions in two passes: it measures the longest trimmed sequence, then recomputes and writes each transition. Working storage holds one transition at a time, and no full intermediate sequence table remains after initialization. Waveform parameters and output actions are unchanged.

首页图标使用 LVGL 解码器接口，保留 SPNG 的 16 行分块边界。`tools/generate_home_icon_pixels.py` 从首页 PNG 生成独立分块的 zlib 无损数据，保留 ARGB8888 或 A8 像素；ARGB8888 解码会话在 PSRAM 按需分配一个 16 行缓冲；A8 图标使用临时整图缓冲，保持原生缩放边界。两者都在关闭会话时释放，不保留图像缓存。未收录资源继续使用原路径。中英文默认关机图通过 `tools/compress_binary.py` 保存原始长度和 zlib 数据，使用时临时解压并释放；原图、完整字库与双 OTA 分区布局保留。

Home icons use the LVGL decoder interface and retain SPNG’s 16-row tile boundaries. `tools/generate_home_icon_pixels.py` packs the source PNGs into independent lossless zlib tiles, preserving ARGB8888 or A8 pixels. ARGB8888 sessions allocate a 16-row buffer in PSRAM; A8 icons use a temporary full-image buffer to preserve native scaling boundaries. Both free their buffers on close without retaining an image cache. Other resources retain their original paths. Both built-in shutdown images are packed by `tools/compress_binary.py` as their original length followed by zlib data, with temporary decode storage freed after use. The artwork, complete font pack and dual OTA partition layout are preserved.

### 黑色深度（黑色强度）

- **默认**：VCOM −1.2V，推荐优先使用。
- **中**：VCOM −1.6V。
- **强**：VCOM −2.0V。

选择后自动保存，由屏幕刷新任务在两次扫描之间应用；不会在扫描中途改电压。此选项调整驱动电压，不改变字体粗细、灰阶数量或刷新模式。更高强度不保证每块屏幕都更黑，需要结合屏幕标定值、残影及底色观察。

选择中或强时，界面提示：**提高黑色深度可能影响屏幕寿命，并增加底色变化风险，建议优先使用默认档。** 此功能未针对每块屏自动标定。若出现底色变化、残影加重或深浅异常，恢复默认，并执行全刷对比。

30 相图片表参考来自 [read-pico-reader](https://github.com/wegooo-cell/read-pico-reader/tree/main/components/e0470_epaper_waveform)，保留 Apache-2.0 许可证。我们保留 S31 / E0470A14-AF 的扫描时序与 TPS65185 电源驱动，显示效果须在本机验收。

### 绘制调度与资源准备 / Rendering and resource preparation

`ScreenPaintCoalesce` 统一合并重绘请求，在物理更新期间保留最新目标。通用导航延后实际建屏，并在执行前校验来源屏幕与待机状态；返回栈在导航执行时更新。业务无需单独判断刷新忙状态。底层 `ScreenLoadReplace` 只负责装载已创建的屏幕，耗时准备应在调用前移到后台。

建屏、控件重建与 LVGL 实际绘制共用同一屏障，协调物理更新的开始时机，并支持同一 UI 任务的嵌套绘制。等待绘制期间，输入、取消、短 UI 提交和其他 LVGL 定时器继续执行。待提交帧不等同于物理更新忙，不能据此暂停整个 UI。同步绘制区内不得调用 `EpdWait`；需要等待落墨的流程应先结束绘制，再提交或等待刷新。

更新任务的优先级高于 UI、低于供数任务；等待扫描和供数时通过信号量阻塞。空间灰阶推图逐相检查分带查表：可按旧灰阶或目标灰阶分解时合并两个像素查表，混合映射保留原表，当前无驱动的分带直接填零。模式复用现有 LUT 缓冲，保持原波形的逐像素输出。

书架及其他共用封面单元先创建占位控件，读取、解码和缩放由单个后台任务完成。待处理任务只保留最新目标，逐张等待 UI 接收结果，避免图片结果积压。页面切换通过序号使旧结果失效，控件销毁后不再回填；加载失败保留占位，不将一般 I/O 或内存错误记为永久缺图。

S31 板型将 LVGL 异步调用及取消统一转入 UI 队列，后台不再操作异步定时器链表。取消按回调和参数匹配所有尚未开始的调用，不释放参数；调度记录由 UI 消费后回收。队列满或记录分配失败时返回 `LV_RESULT_INVALID`，调用方须按原接口处理失败。后台仍不得直接操作其他 LVGL 对象或定时器。

`ScreenPaintCoalesce` merges repaint requests and retains the latest target during physical updates. Common navigation delays screen construction and validates the originating screen and standby state before execution; the back stack changes only when navigation executes. Pages do not need individual refresh-busy checks. The low-level `ScreenLoadReplace` loads an already-created screen; expensive preparation belongs in background work before that call.

Screen construction, widget rebuilding and LVGL rendering share one admission gate with physical updates, including nested rendering on the same UI task. Input, cancellation, short UI submissions and other LVGL timers continue while rendering waits. A queued frame is distinct from an active physical update and must not pause the entire UI. `EpdWait` is not allowed inside synchronous rendering; operations that wait for the panel must finish rendering before submitting or waiting for an update.

The update task runs above the UI and below the refill tasks, blocking on semaphores while waiting for scans or refill. Spatial gray transitions inspect each band's LUT on every phase. Mappings that depend only on the old or target gray value use paired-pixel lookup; mixed mappings retain the original table, and inactive bands output zero. Mode data reuses the existing LUT buffer and preserves the waveform's per-pixel output.

Shared book-cover cells create placeholders first. One background worker reads, decodes and scales images, retains only the latest pending target and waits for UI acknowledgement after each image to bound pending results. Sequence checks discard stale results after navigation, and deleted controls are never updated. Load failures retain placeholders; ordinary I/O or allocation failures are not cached as permanent missing images.

On S31 boards, LVGL asynchronous calls and cancellation use the shared UI queue instead of modifying asynchronous timers from background tasks. Cancellation matches all calls with the same callback and context that have not started; it does not free the context. The UI consumer releases dispatch records. A full queue or allocation failure returns `LV_RESULT_INVALID`, which callers must handle as with the original API. Other LVGL objects and timers must still be accessed on the UI thread or under its lock.

S31 板型通过链接包装，将可能访问 Flash 的 NVS C 接口与完整面板波形串行化，包括只读打开和读取。最外层 NVS 调用排空并持有共享 I2C，锁顺序为 Flash → I2C → NVS。日志设置在文件锁外持久化；持有普通 I2C 锁时，以及活动波形、供数回调内，不得调用 NVS。包装保留原同步行为和返回值；直接 NVS C++ 接口及其他裸 Flash 操作不在此保护范围内。

普通 I2C 准入等待不将受控 Flash 独占窗口计入争用超时；实际传输使用独立的超时限制。非阻塞尝试和无限等待保留原有语义。

On S31 boards, linker wrappers serialize NVS C APIs that can access Flash, including read-only open and reads, against complete panel waveforms. The outer NVS call drains and holds shared I2C; lock order is Flash → I2C → NVS. Log settings are persisted outside the file lock. Do not call NVS while holding I2C or from active waveform or refill callbacks. Return values and synchronous behavior remain unchanged. Direct NVS C++ access and other raw Flash operations are outside this protection.

Ordinary I2C admission excludes controlled Flash ownership from its contention timeout. Actual transfers retain a separate timeout. Nonblocking attempts and indefinite waits preserve their original behavior.

崩溃镜像转存先取得 Flash 门闩，再取得文件锁；仅在 SDK 的 Flash 读写、擦除期间排空并持有 I2C，SD 文件操作不持有 I2C。普通 I2C 持锁区不得取得 Flash 门闩或日志文件锁。镜像成功同步到 SD 后才允许擦除 Flash；等待擦除时，换卡、卸载或清空日志会撤销旧副本的擦除请求。

Crash-image export takes the Flash gate before the file lock. Only SDK Flash operations drain and hold I2C; SD file operations do not hold I2C. Ordinary I2C lock holders must not acquire the Flash gate or log file lock. Flash is erased only after the SD copy is synced successfully. Mount changes, unmounting or clearing logs cancel pending erasure of a previous copy.

### 通用短提示

页面包含 `screen_common.h` 后，调用 `ScreenShowToast("提示文本")` 即可显示短提示。提示位于顶部状态栏下方，采用黑底白字和圆角，尺寸随文本调整，长文本自动换行，3 秒后自动消失。连续调用会替换上一条并重新计时；空指针或空字符串不显示。

接口在调用时复制文本，须在 LVGL 线程或持有 UI 锁时使用。后台任务可通过 `ScreenLvAsync` 投递，并确保传入文本在回调执行前仍有效。

SD 卡数据读写发生错误时显示顶部短提示，同一次启动仅提示一次；连续错误不重复弹出。待机时暂缓显示，恢复后提示。该提示表示读写链路异常，不代表已判定内存卡损坏。

### 阅读触摸分区

自定义触摸分区时，必须保留至少一个“菜单”区域。尝试替换最后一个菜单区域时，配置保持不变，并显示短提示；需要调整菜单位置时，应先在其他分区设置“菜单”。加载到无菜单区域或格式无效的历史配置时，使用默认分区。

书签装饰和快捷面板图标复用内嵌无损图像解码器，保留 A8/AL88 像素及原始尺寸。绘制会话按需分配 PSRAM 图像缓冲，关闭时释放，不保留解压缓存。文件传输的中英文页面分别以 gzip 存储，通过设备的 `/cloudzao-logo.png` 路径共用一份标识图片，无需外网或 SD 卡。

Bookmark decorations and quick-panel icons reuse the embedded lossless image decoder, preserving A8/AL88 pixels and original dimensions. Drawing sessions allocate image buffers in PSRAM on demand and release them on close without retaining decoded caches. The Chinese and English file-transfer pages are stored separately as gzip data and share one logo served by the device at `/cloudzao-logo.png`, without internet access or an SD card.

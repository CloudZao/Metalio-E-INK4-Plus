# 渐进式 JPEG 解码 / Progressive JPEG decoding

`stb_image.h` 来自 https://github.com/nothings/stb ，固定版本为
`31c1ad37456438565541f4919958214b6e762fb4`（stb_image 2.30），第三方原文件不作修改。
`stb_jpeg_core.h` 和 `progressive_entropy.h` 是同版本 JPEG 熵解码及整数 IDCT 的派生代码，保留 MIT / Public Domain 双授权文本。

- 共享图片入口将 SOF2 渐进式 JPEG 交给本模块，支持常见 8 位灰度／三分量 YCbCr。12 位、算术编码和四分量不在支持范围内。
- 小图保留 stb 完整解码路径；根据原图尺寸、采样和可用 PSRAM 预估峰值。超预算或分配失败时，改用扫描游标及 MCU 行系数条带，不申请完整原图系数、颜色平面或灰度中间图。
- 条带与格式工作区最多占用 3 MiB PSRAM。扫描快照或单行放不下时使用有界分页；按需在 `metalio/e-ink/temp/wallpaper/` 创建独占的 `pj-XXXXXXXX.tmp` 系数后备文件，速度受 SD 随机读写影响。可容纳的条带不写系数文件。
- 成功、失败、取消均关闭并删除工作文件；首次使用后备目录时只清理符合上述命名的自有遗留文件。源图、正式壁纸和其他文件不参与清理。SD 事务覆盖创建至清理，不绕过待机及介质切换保护。
- 最终结果仍为现有 `RasterImage` L8，按原图坐标最近邻缩放。完整 8×8 IDCT 与原 stb 路径保持相同；并发渐进式请求串行处理。
- 工作区、系数缓存和最终 L8 合计最多 8 MiB，每次新增大块分配前保留至少 2 MiB PSRAM。大块结果若落入内部 RAM 则释放并失败；内存栅格接口的原尺寸输出超预算时仍会失败。
- 大渐进式文件按需读取，不缓存完整压缩输入。文件入口不再按 4 MiB 拒绝；受文件偏移范围、格式和实际资源约束。内存入口借用的输入仍由调用方持有，不计入上述工作区预算。
- 等待互斥量、读取、扫描、系数换页和输出时检查取消。禁止强删任务。壁纸页面继续使用现有 epoch / generation 作废过期结果；已有预览及缩略图缓存沿用界面生命周期。
- 普通 JPEG 不进入本后备算法：小文件使用既有 `esp_new_jpeg` 缩放路径；超过 4 MiB 或输入缓冲申请失败时复用 ROM TJpgDec 流式路径，直接写入目标 L8。PNG 保持既有按行实现。
- `esp_new_jpeg` 的内部工作区分配复用固件内部 RAM 安全余量，小工作区可留在内部 RAM，余量不足时转用 PSRAM。超过 4 KiB 的 RGB565 输出不向内部 RAM 回退。待机、关机及叠图按最终可绘区域解码；原尺寸接口仅用于确需保留原图分辨率的处理。


- 壁纸调整在解码阶段使用屏幕边界，再复用旋转／镜像及 A2L8 编码。覆盖保存和另存为均等比输出至竖图 684×1216 或横图 1216×684，不放大小图；另存为保留源文件，覆盖保存替换原文件。完整 A2L8 不超过面板像素量及 8 字节头，不生成原尺寸灰度中间文件。
- 完整结果先写入 `metalio/e-ink/temp/wallpaper/ws-XXXXXXXX.tmp`，关闭后再提交。成功、失败及取消均清理工作文件；覆盖时先将原文件移入该目录的 `backup/`，保留原文件名，提交失败则恢复。断电或恢复失败留下的备份需先恢复或确认处理，后续保存不会覆盖它。另存为遇到同名目标时失败。
- 保存与删除串行执行，并回收断电遗留的自有 `ws-XXXXXXXX.tmp`。单张、批量及云壁纸删除均清理对应原图、元数据、下载临时文件和同名恢复备份；正在解码的系数工作文件由所属请求清理，避免误删其他任务的数据。
- A2L8、旧 EBGR 和 A2I1 按行读取及缩放，避免将整份原始栅格加载为压缩输入。旋转预览重新适配预览框；保存成功后重新读取输出，取消保存后等待后台收尾再允许新保存。

The unmodified `stb_image.h` is pinned to stb_image 2.30 at the commit above. The derived JPEG entropy and integer IDCT helpers retain the upstream MIT / Public Domain license texts.

- Common 8-bit grayscale and three-component YCbCr progressive JPEGs are supported. Twelve-bit, arithmetic-coded and four-component images are outside the supported scope.
- Small images retain the existing buffered stb path. Images whose estimated peak exceeds the budget, or whose allocation fails, use scan cursors and bounded MCU-row coefficient bands instead of full-image coefficients, color planes and an intermediate grayscale image.
- Format state and coefficient bands use at most 3 MiB of PSRAM. If scan snapshots or one MCU row cannot fit, bounded paging uses an exclusive `pj-XXXXXXXX.tmp` file under `metalio/e-ink/temp/wallpaper/`. Bands that fit do not write coefficient files. Paging speed depends on SD random I/O.
- Work files are closed and removed after success, failure or cancellation. First use removes only abandoned files matching the module's naming pattern. Source images, saved wallpapers and unrelated files are retained. Existing SD transactions protect creation through cleanup, including standby and media ownership transitions.
- Results remain `RasterImage` L8 images, using nearest-neighbor sampling in original-image coordinates after the same full 8×8 integer IDCT as the stb path. Progressive requests are serialized.
- Format state, coefficients and final L8 output share an 8 MiB limit and preserve at least 2 MiB of free PSRAM before large allocations. Large output allocated in internal RAM is rejected. Original-size output through the in-memory raster API may still exceed this budget.
- Large progressive files are read on demand without buffering the complete compressed input. File decoding has no 4 MiB rejection threshold; file offsets, format and available resources still impose limits. Borrowed memory input remains owned by the caller and is outside the workspace budget.
- Cancellation is checked while waiting, reading, scanning, paging and producing output. Tasks must finish cooperatively. Existing wallpaper generations and UI cache lifetimes remain in use.
- Ordinary JPEGs keep the existing small-file `esp_new_jpeg` path. Files over 4 MiB, or those whose input buffer cannot be allocated, use the ROM TJpgDec streaming path to produce target L8 pixels. PNG retains its existing row decoder.
- The `esp_new_jpeg` workspace allocator preserves the firmware's internal RAM reserve. Small workspaces may use internal RAM; allocations that would consume the reserve use PSRAM. RGB565 output larger than 4 KiB does not fall back to internal RAM. Standby, power-off and overlay decoding use their final display bounds; original-size decoding is reserved for operations that need the source resolution.

- Wallpaper adjustment decodes within screen bounds and reuses the existing orientation and A2L8 encoder. Both Save as and Overwrite fit portrait images within 684×1216 or landscape images within 1216×684, without upscaling. Save as retains the source; Overwrite replaces it. Output is bounded by the panel pixel count plus an 8-byte header; no original-size grayscale intermediate is produced.
- Completed output is written to `metalio/e-ink/temp/wallpaper/ws-XXXXXXXX.tmp`, closed and then committed. Work files are removed on completion, failure or cancellation. Overwrites first move the original into `backup/` within that directory, retaining its filename, and restore it if commit fails. A backup left by power loss or failed restoration is retained and blocks another overwrite until recovered or reviewed. Save-as rejects an existing destination.
- Saving and deletion are serialized and reclaim abandoned, owned `ws-XXXXXXXX.tmp` files. Single, batch and cloud wallpaper deletion remove the image, metadata, download temporary file and matching recovery backup. Each decoding request removes its own coefficient work file, leaving other active requests intact.
- A2L8, legacy EBGR and A2I1 files are sampled by row. Rotated previews fit their bounds, successful saves reload the output, and cancelled saves finish cleanup before a new save starts.

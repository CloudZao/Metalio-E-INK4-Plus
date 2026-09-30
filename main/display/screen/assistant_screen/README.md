# 百问AI（AssistantScreen）与 A2UI 会话分页

## 职责

- 进页准备语音 / 离页停语音（`StartXiaozhiVoice` / `StopXiaozhiVoice`）
- **空会话**：中文全屏 `ic_s_assistant_hint.a2i1`（resources）；英文全屏嵌入固件
  `assets/embedded/ic_s_assistant_hint_en.a2i1`（随 app OTA，不进资源 bin）；有消息或历史回放后隐藏
- **Push-to-Talk（多源）**：BOOT / **屏内任意区域** 长按满 `kBootLongPressMs`（500ms）开听，松开结束；任一源仍按住则保持聆听
- Listening 后状态栏居中显示代码绘制的录音波形（隐藏时钟）
- 对话区解析 A2UI，**保留原组件样式**（Text / RichText / **Math** / Card / Image / Header 等）追加到会话
- 按视口**字形精确分页**后，用 `a2ui_handle_json` 渲染当前页（禁止滚动）

## PTT 多源

| 源 | 臂听 | 松手 | 备注 |
|----|------|------|------|
| BOOT | `OnBootLongPress` | `OnBootPressUp` → `StopListeningIfNoPttHeld` | 短按打断说话；**双击**有摄像头则拍照并以 A2UI `Image` 插入对话 |
| 屏触 hit 层 | one-shot timer 500ms | `RELEASED` / `PRESS_LOST` → 同上 | `HapticAttachClick` 按下早震；短按 CLICKED 投递翻页（不二次震）；长按臂听不二次震；二者互斥；盖板 VK 仍走 touch_feed |

离页：`ResetTouchPttState` 删臂听 timer + 清标志，避免泄漏与迟到开听。

## 分页与虚拟键

| 项 | 行为 |
|----|------|
| 追加 | 每次 `updateComponents` 解析追加；assistant **400ms 合并**再尾部重排（不持 LVGL 锁）；影响页 **≥500ms** 才排队刷墨水；翻页立即画当前页 |
| 内存 | `s_flow` / `s_pages` 与其中字符串走 SPIRAM；UI 最多 **1500 页**，超出从头部裁 flow 后重分页 |
| 样式 | 翻页时重建 A2UI JSON，经 a2ui 渲染，视觉与原组件一致 |
| 换行/换页 | 按字形宽度折行；整行高度装不下则先翻页，不半行截断 |
| 页码 | 底部 `当前 / 总页` |
| `vk_prev` / `vk_next` | 短按翻一页（页码同步累加、绘制异步合并）；长按自按下起每 **1s ±10 页**；第 1 页短按 `vk_prev` 返回上一屏 |
| `vk_home` | 短按出栈回上一屏；长按交 VkKey 默认一键回系统首页（不作 PTT） |
| 离页 | 清空 RAM 会话（SD 历史保留至关机） |
| 再进页 | 有 SD 则回放 `chat_log`；无卡则空会话 |
| 开机 | 清空 `chat_log` 全部 JSON |

## AddMessage

| `content` | 行为 |
|-----------|------|
| `nullptr` / `""` | 忽略（不清屏） |
| A2UI `updateComponents` | 追加样式流并重分页渲染 |
| A2UI `deleteSurface` | 清空 RAM 会话，并 wipe `chat_log` |
| 其它纯文本 | 跳过 |

## 文件

| 路径 | 说明 |
|------|------|
| `assistant_screen.h` / `.cc` | 追加流 + 分页 + 调 a2ui 渲染当前页 |
| `assistant_chat_store.*` | 会话 JSON 落盘（`SD_PATH_CHAT_LOG`，开机清空；无 SD 禁用） |
| `../a2ui/` | 组件样式渲染（root 高度随内容）；**Math 公式**见 [`../a2ui/README.md`](../a2ui/README.md#math设备端-latex-公式) |
| `../a2ui/a2ui_img_cache.*` | Image A2I1 会话缓存（`SD_PATH_A2UI_CACHE`，开机清空；无 SD 则禁用） |
| `../a2ui/a2ui_math.*` | LaTeX 子集解析 + 盒模型排版（Latin Modern Math） |

Image 缓存挂在 a2ui 加载管线，本屏只需继续下发带 `url` 的 Image 组件即可。  
公式下发 `Math`/`Formula`（`latex` 字段），分页时整块占位；样例 [`../a2ui/examples/math_formulas.json`](../a2ui/examples/math_formulas.json)。

## 会话 JSON（SD）

路径：`/sdcard/metalio/e-ink/chat_log/mNNNNN.json`（一条有效 `updateComponents` 一文件）。

| 项 | 行为 |
|----|------|
| 无 SD | 不写不读，纯 RAM（与原来一致） |
| 开机 | `assistant_chat_store_init` 清空目录 |
| 上限 | 单条 24KB，最多 40 条，合计 192KB；超出丢最旧再写 |
| 回放 | 按文件流式解析，不把 192KB JSON 一次性拷进 RAM |

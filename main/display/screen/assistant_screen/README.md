# 百问AI（AssistantScreen）与 A2UI 会话分页

## 职责

- 进页准备语音 / 离页停语音（`StartXiaozhiVoice` / `StopXiaozhiVoice`）
- **云服务**：设置 → 对话中选择百问 AI / 小智 AI / 网易 AI，保存后重启生效；协议配置和小智/网易本地语音开关分别保存。固件更新与 AI 激活使用独立请求。
- **空会话**：中英文欢迎页使用 LVGL Label、线条和圆弧绘制，不加载默认位图；标题跟随云服务，小智/网易不显示待办介绍。有消息或历史回放后隐藏。
- **Push-to-Talk（多源）**：BOOT / **底栏录音按钮** 长按满 `kBootLongPressMs`（500ms）开听，松开结束；任一源仍按住则保持聆听。正文仅点击翻页。
- **待发送照片**：仅百问开放，进页即预留拍照入口位置，检测完成后填入图标，不移动录音区域；无摄像头硬件保留空位，不显示按钮。拍照按钮或 BOOT 双击取得一张待发送照片，重拍成功后替换旧图，失败保留旧图。缩略图悬浮于正文左下方，不改变正文高度、页码或欢迎页布局；支持删除和窗口查看。普通待机保留，离页清理。
- **照片上传与提问**：每张照片使用当前启动内自增的 `image_id`，通过 `api::VisionUploadUrl()` 的 HTTPS 地址上传，URL 参数为 `image_id`，multipart 二进制字段为 `image`，请求头 `Device-Id` 与 WebSocket 一致。必须同时检查 HTTP 状态、`success` 和响应编号。单图发送先准备 WebSocket，上传后提交 `listen/detect` 与单元素 `image_ids`，省略问题文字以使用默认描述。带图录音在开始时固定附件，松手后上传，再发送 `listen/stop` 与对应 `image_ids`。照片加入本地聊天记录后不重复插入，提问成功提交后清除待发送照片。上传或提问提交失败保留照片；暂存时间接近服务端默认有效期时重新上传。服务端返回识图关闭或分析错误时恢复当前照片供重试。
- **底部布局**：圆角操作栏左侧为拍照，中间为按住说话；右侧无照片时显示静态猫爪，有照片时显示发送按钮。页码位于操作栏上方的正文区域居中。照片区仅显示可点击的缩略图与角标删除按钮，不显示状态文案和照片数量。
- **Bottom layout**: the rounded toolbar places capture on the left and hold-to-talk in the middle. Its right side shows a static paw without a photo and a Send button with a photo. Page numbers are centered in the content area above the toolbar. The attachment area contains only a tappable thumbnail and a corner delete button, with no status or photo count.
- **照片查看**：悬浮缩略图带细外框，右上角删除按钮及触摸区域同步放大。点击缩略图在顶层打开透明背景的居中窗口，原页面保持可见，图片等比适配；点击窗口外空白区域或下方“关闭”按钮收起。
- **图片尺寸与位置**：聊天记录中的照片复用拍摄缩略图的像素和尺寸，左侧同行显示当前语言的“用户：”或“User:”。悬浮缩略图与底栏保持小间距，不为页码额外预留空间；页码仍固定在正文区域居中。
- **Image size and placement**: chat photos reuse the captured thumbnail pixels and dimensions, with the localized User label on the left in the same row. The floating thumbnail has a small gap above the toolbar without extra space for page numbers; page numbers remain centered in the content area.
- **Photo viewer**: the floating thumbnail has a thin border and a larger delete button and touch target at its upper-right corner. Tapping it opens a centered window on a transparent top-layer overlay, keeping the original page visible. The image fits the window; tap outside or use Close beneath it to dismiss.
- **Pending photo (Baiwen only)**: the toolbar reserves its camera slot from entry; detection reveals the button without moving hold-to-talk. Hardware without a camera keeps the slot empty. A detected camera enables capture by button or BOOT double-click. One photo is retained; a successful retake replaces it, while a failed retake keeps it. Its floating thumbnail does not resize or repaginate the conversation or rearrange the welcome page. Standby retains it; leaving the page clears it.
- **Photo upload and questions**: each capture has an ID incremented within the current boot. Upload uses the HTTPS `api::VisionUploadUrl()`, an `image_id` query parameter, a multipart binary field named `image`, and the same `Device-Id` as WebSocket. Validate the HTTP status, success flag and returned ID. Image-only Send prepares WebSocket, uploads, then submits `listen/detect` with a single-element `image_ids` array and no text for the default description. Recording snapshots its attachment at start; release uploads it before sending `listen/stop` with its image ID. Local history insertion is not repeated on retry. Successful question submission clears the pending attachment. Failed upload or submission retains it; uploads nearing the default temporary-cache expiry are repeated. Vision-disabled or analysis errors restore the current photo for retry.
- **轮次与生命周期**：上传中允许开始新录音。新录音替代旧提问，旧 HTTP 上传继续完成；迟到回调仅保存上传及本地图片结果，不发送旧轮次的结束消息。普通录音不自动携带已上传图片或照片组。待机和离页取消对应页面的未完成提问；图片历史保留在现有 RAM/SD 缓存与会话存储中。Wi-Fi 关闭时，录音和发送显示顶部提示。本地拍照及预览不依赖联网。当前页面仅提供单图提问，不提供照片组追问、追加或多图入口。
- **Turns and lifecycle**: a new recording can start during upload and replaces the previous question while its HTTP upload continues. Late callbacks retain upload and local-image results without sending the old turn's stop message. Ordinary recordings never automatically attach earlier images or groups. Standby and page exit cancel unfinished questions for that page. Image history uses the existing RAM/SD caches and session store. With Wi-Fi disabled, recording and Send show a top notice; local capture and preview remain available offline. The current page provides single-image questions, with no group follow-up, append or multi-image controls.
- Listening 后状态栏居中显示代码绘制的录音波形（隐藏时钟）
- 对话区解析 A2UI，**保留原组件样式**（Text / RichText / **Math** / Card / Image / Header 等）追加到会话
- 按视口**字形精确分页**后，用 `a2ui_handle_json` 渲染当前页（禁止滚动）

## PTT 多源

| 源 | 臂听 | 松手 | 备注 |
|----|------|------|------|
| BOOT | `OnBootLongPress` | `OnBootPressUp` → `StopListeningIfNoPttHeld` | 短按打断说话；双击仅在百问且有摄像头时准备待发送照片 |
| 底栏录音 hit 层 | one-shot timer 500ms | `RELEASED` / `PRESS_LOST` → 同上 | `HapticAttachClick` 按下早震，长按开听不二次震；正文点击独立处理翻页；盖板 VK 仍走 touch_feed |

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
| 小智/网易纯文本 | 转为 RichText，参与分页与历史保存 |
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

## 网易流式文字与刷新

网易累积文字先去重、按轮次合并，再更新当前回复。只重新折行最后一条物理行、重排受影响的末页；旧页保持不变。字体、视口或文本前缀变化时回退到全量分页。当前可见页内容与几何未变化时跳过控件重建。格式化文字、行范围与页面缓存使用 PSRAM，分配失败保留恢复路径。

核心启动先开放激活检查和 PTT，再执行天气、清单等附加网络预热。服务端确认激活后清除旧激活码；TLS 分配策略与 397 一致，使用 PSRAM。

设置页查询 TTS 不阻止云服务切换；切换服务或重建页面会使旧请求结果失效，避免更新新的界面。

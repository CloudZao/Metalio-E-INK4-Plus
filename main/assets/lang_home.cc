#include "assets/lang_workbuddy.h"
#include "assets/lang_reader.h"
#include "assets/lang_config.h"
#include "assets/lang_screen.h"
#include "assets/lang_pixel.h"
#include "assets/lang_log.h"
#include "assets/lang_haptic.h"
#include "assets/lang_network.h"
#include "assets/lang_charging.h"
#include "assets/lang_bluetooth.h"

#include "settings.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Lang"

// X(name, zh, en)
#define LANG_STRING_LIST(X)                                                                                    \
    X(SETTINGS_TEST_FRONTLIGHT_START, "点击开启", "Tap to turn on") \
    X(DEVICE_ACTIVATE_FIRST, "请先激活设备", "Activate the device first") \
    X(STORAGE_IO_ERROR, "SD 卡读写异常", "SD card read/write error") \
    X(AUDIO_SERVICE_NOT_READY, "音频服务尚未就绪，请稍后重试", "Audio service is not ready. Try again shortly.") \
    X(AUDIO_CALLBACK_UNAVAILABLE, "录音回调不可用，请重新进入应用", "Recording is unavailable. Reopen the app.") \
    X(AUDIO_RECORDING_BUSY, "上一轮录音尚未结束，请稍后重试", "The previous recording is still finishing. Try again shortly.") \
    X(AUDIO_MIC_BUSY, "麦克风正被其他功能使用，请结束后重试", "The microphone is in use. Stop the other recording and retry.") \
    X(AUDIO_RECORDING_MEMORY, "录音内存不足，请稍后重试", "Not enough memory to record. Try again shortly.") \
    X(AUDIO_PROCESSOR_FAILED, "录音处理器初始化失败，请重新进入应用", "Recording initialization failed. Reopen the app.") \
    X(TTF_SIZE_FAILED_FMT, "无法设置字号 %d", "Cannot set font size %d") \
    X(TTF_SD_IO_FAILED, "存储卡读写失败，请检查存储卡", "SD card read/write failed. Check the card.") \
    X(TTF_NO_GLYPHS_FMT, "字号 %d 无可用字形", "No glyphs available at size %d") \
    X(TTF_WRITE_FAILED, "字体写入失败，请检查存储卡", "Cannot write font. Check the SD card.") \
    X(TTF_VERIFY_FAILED, "字体校验失败，请重新转换", "Font verification failed. Convert again.") \
    X(TTF_TEMPLATE_INVALID, "模板字体无效，请重新导入", "Invalid template font. Import it again.") \
    X(TTF_ENGINE_INIT_FAILED, "字体引擎初始化失败", "Font engine initialization failed") \
    X(TTF_SOURCE_OPEN_FAILED, "无法打开源字体，请检查字体文件", "Cannot open source font. Check the file.") \
    X(TTF_JOB_INVALID, "转换任务无效，请重新转换", "Invalid conversion job. Convert again.") \
    X(TTF_START_FAILED, "无法启动转换，请稍后重试", "Cannot start conversion. Try again shortly.") \
    X(TTF_CANCELLED, "转换已取消", "Conversion cancelled") \
    X(TTF_GLYPH_MEMORY, "字形缓存内存不足，请减小字号", "Not enough memory for glyphs. Reduce the font size.") \
    X(TTF_CANCELLED_OR_NO_SD, "转换已取消或 SD 卡不可用", "Conversion cancelled or SD card unavailable") \
    X(ASSISTANT_USER_PREFIX, "用户：", "You: ") \
    X(ASSISTANT_CONNECTING_HOLD, "正在连接语音服务，请继续按住", "Connecting to voice service. Keep holding.") \
    X(BOOK_IMPORT_TTF, "导入 TTF", "Import TTF") \
    X(BOOK_ORIENT_AUTO, "自动", "Auto") \
    X(BOOK_ORIENT_TITLE, "阅读方向", "Orientation") \
    X(BOOK_WRITING_HORIZONTAL, "横排", "Horizontal") \
    X(BOOK_WRITING_VERTICAL, "竖排", "Vertical") \
    X(BOOK_WRITING_TITLE, "文字排版", "Text layout") \
    X(BOOK_TEXT_DIRECTION, "文字方向", "Text direction") \
    X(BOOK_TEXT_LEFT_TO_RIGHT, "从左往右", "Left to right") \
    X(BOOK_TEXT_RIGHT_TO_LEFT, "从右往左", "Right to left") \
    X(BOOK_TURN_SLIDE, "左右滑动", "Slide") \
    X(BOOK_TURN_PUSH, "推白翻页", "Wipe") \
    X(BOOK_TURN_TITLE, "翻页方式", "Page turn") \
    X(BOOK_AUTO_TURN, "定时翻页", "Auto turn") \
    X(BOOK_PRESET_SMALL, "小", "Small") \
    X(BOOK_PRESET_LARGE, "大", "Large") \
    X(BOOK_PRESET_XLARGE, "很大", "X-large") \
    X(BOOK_MARGIN_TITLE, "边距设置", "Margins") \
    X(BOOK_SPACING_TITLE, "行距设置", "Line spacing") \
    X(BOOK_BACKGROUND_GRID, "方格", "Grid") \
    X(BOOK_BACKGROUND_DOTS, "点阵", "Dots") \
    X(BOOK_BACKGROUND_TITLE, "阅读背景", "Background") \
    X(BOOK_CUSTOM, "自定义", "Custom") \
    X(BOOK_FONT_LOADING, "字体加载中…", "Loading fonts...") \
    X(BOOK_FONT_REOPEN, "字体加载失败，请重新打开字体页签", "Font loading failed. Reopen the Fonts tab.") \
    X(BOOK_FONT_IMPORT_EMPTY, "暂无字体，请导入 TTF", "No fonts. Import a TTF font.") \
    X(BOOK_BOOKMARKS, "书签", "Bookmarks") \
    X(BOOK_LAYOUT_WAIT, "正文正在编排，请稍后操作", "Laying out text. Try again shortly.") \
    X(COMMON_MEMORY_RETRY, "内存不足，请稍后操作", "Not enough memory. Try again shortly.") \
    X(BOOK_BOOKMARKS_EMPTY, "暂无书签", "No bookmarks") \
    X(BOOK_DURATION_HM_FMT, "%u小时%u分钟", "%uh %um") \
    X(BOOK_READING_PROGRESS, "阅读进度", "Reading progress") \
    X(BOOK_CHAPTER_PREV, "上一章", "Prev chapter") \
    X(BOOK_CHAPTER_NEXT, "下一章", "Next chapter") \
    X(BOOK_PAGE_PREV, "上一页", "Previous page") \
    X(BOOK_PAGE_NEXT, "下一页", "Next page") \
    X(BOOK_PAGE_FMT, "第%d页", "Page %d") \
    X(BOOK_COMIC_SETTING_SAVED, "设置已保存，漫画暂不支持应用此项设置", "Saved. This setting does not apply to comics.") \
    X(BOOK_FONT_SELECT, "选择字体", "Choose font") \
    X(BOOK_READING_SETTINGS, "阅读设置", "Reading settings") \
    X(BOOK_SELECTED_ITEMS_FMT, "已选 %u 项", "Selected %u") \
    X(BOOK_TAP_MENU_REQUIRED, "至少保留一个菜单唤醒区域", "Keep at least one tap zone assigned to Menu.") \
    X(TTF_FINISHING, "转换任务正在收尾，请稍后重试", "Conversion is finishing. Try again shortly.") \
    X(TTF_SOURCE, "源字体", "Source font") \
    X(TTF_OUTPUT, "输出", "Output") \
    X(TTF_SUCCESS, "字体转换成功，已加入字体列表", "Font converted and added to the font list.") \
    X(TTF_CONVERTING, "转换中", "Converting") \
    X(TTF_FAILED, "转换失败", "Conversion failed") \
    X(TTF_EMPTY, "未找到 TTF/OTF 字体\n请放入 SD 卡 metalio/e-ink/fonts_ttf 目录", "No TTF/OTF fonts found\nPlace fonts in metalio/e-ink/fonts_ttf on the SD card") \
    X(TTF_SIZE_RANGE, "字号（20–80px）", "Font size (20–80px)") \
    X(TTF_OVERWRITE_HINT, "同名 .ef 文件将被覆盖。离开页面或待机将取消转换，请勿拔卡。\n直接应用会降低翻页速度。", "Existing .ef files with the same name will be replaced. Leaving this page or entering standby cancels conversion. Keep the SD card inserted.\nApplying directly slows page turns.") \
    X(TTF_START, "开始转换", "Convert") \
    X(TTF_APPLY, "直接应用", "Apply directly") \
    X(TTF_APPLY_FAILED, "字体加载失败，请检查字体文件或减小字号", "Cannot load font. Check the file or reduce the size.") \
    X(TTF_OUTPUT_NAME_TOO_LONG, "输出名称过长或无效，请缩短源字体文件名后重试", "Output name too long or invalid. Shorten the source filename and retry.") \
    X(TTF_GENERATING, "正在生成字库", "Generating font") \
    X(TTF_PROGRESS_HINT, "转换完成后，字库将加入字体列表。离开页面或待机将取消转换，请勿拔卡。", "The converted font will appear in the font list. Leaving this page or entering standby cancels conversion. Keep the SD card inserted.") \
    X(TTF_FAILED_FMT, "转换失败：%.60s", "Conversion failed: %.60s") \
    X(TTF_BACK_TO_LIST, "返回导入列表", "Back to imports") \
    X(BOOK_VIEW_COVER, "封面", "Covers") \
    X(BOOK_VIEW_LIST, "列表", "List") \
    X(BOOK_WEEK_READING, "本周阅读", "This week") \
    X(BOOK_MINUTES_FMT, "%.1f 分钟", "%.1f min") \
    X(BOOK_WEEK_MON, "一", "M") \
    X(BOOK_WEEK_TUE, "二", "T") \
    X(BOOK_WEEK_WED, "三", "W") \
    X(BOOK_WEEK_THU, "四", "T") \
    X(BOOK_WEEK_FRI, "五", "F") \
    X(BOOK_WEEK_SAT, "六", "S") \
    X(BOOK_WEEK_SUN, "日", "S") \
    X(BOOK_WEEK_DURATION, "本周阅读时长", "This week") \
    X(BOOK_TOTAL_DURATION, "总阅读时长", "Total reading time") \
    X(TRANSFER_SD_CAPACITY_FAILED, "SD 卡容量读取失败", "Cannot read SD card capacity") \
    X(TRANSFER_RECOVERY_FAILED, "目录读取或上传恢复失败", "Cannot read directory or recover uploads") \
    X(TRANSFER_DIRECTORY_MISSING, "目录不存在", "Directory not found") \
    X(TRANSFER_DIRECTORY_FAILED, "目录读取失败，请检查 SD 卡后刷新", "Cannot read directory. Check the SD card and refresh.") \
    X(TRANSFER_DOWNLOAD_BUSY, "上传期间暂不支持下载，请完成后重试", "Wait for the upload to finish before downloading.") \
    X(TRANSFER_READ_FAILED, "无法读取", "Cannot read file") \
    X(TRANSFER_SD_FULL, "SD 卡空间不足，原文件保留", "SD card is full. The original file is preserved.") \
    X(TRANSFER_MEMORY, "设备内存不足，上传已停止，原文件保留；请稍后重试", "Not enough memory. Upload stopped; the original file is preserved. Try again shortly.") \
    X(STORAGE_RESTART_REQUIRED, "内存卡状态异常，请重启设备后再试", "SD card error. Restart the device and try again.") \
    X(TRANSFER_WRITE_FAILED, "SD 卡写入失败，原文件保留；请检查内存卡后重试", "SD card write failed. The original file is preserved. Check the card and retry.") \
    X(TRANSFER_DISCONNECTED, "连接中断，未提交的文件已清理，原文件保留", "Disconnected. Uncommitted files were removed; the original file is preserved.") \
    X(TRANSFER_UPLOAD_BUSY, "已有上传在进行，请稍后重试", "An upload is in progress. Try again shortly.") \
    X(TRANSFER_INVALID_ID, "上传任务标识无效", "Invalid upload ID") \
    X(TRANSFER_ALREADY_HANDLED, "此上传任务已处理，请刷新目录确认结果", "Upload already processed. Refresh the directory to check the result.") \
    X(TRANSFER_ENCODING_UNSUPPORTED, "不支持此上传编码，请刷新页面重试", "Unsupported upload encoding. Refresh the page and retry.") \
    X(TRANSFER_INVALID_FILE, "文件名、路径或文件大小无效", "Invalid file name, path or size") \
    X(TRANSFER_INVALID_SIZE, "文件大小无效", "Invalid file size") \
    X(TRANSFER_SD_CAPACITY_UNAVAILABLE, "无法读取 SD 卡容量", "Cannot read SD card capacity") \
    X(TRANSFER_EXISTS_CHOOSE, "已存在同名项目，请重新选择处理方式", "An item with this name already exists. Choose how to proceed.") \
    X(TRANSFER_TARGET_CHECK_FAILED, "无法检查目标文件", "Cannot check the destination file") \
    X(TRANSFER_TEMP_FAILED, "无法创建临时文件，原文件保留", "Cannot create temporary file. The original file is preserved.") \
    X(TRANSFER_INCOMPLETE, "上传不完整，原文件保留", "Incomplete upload. The original file is preserved.") \
    X(TRANSFER_CANCELLED, "上传已取消，原文件保留", "Upload cancelled. The original file is preserved.") \
    X(TRANSFER_TARGET_CHANGED, "同名项目已变化，请重新上传", "The destination item has changed. Upload again.") \
    X(TRANSFER_TARGET_CHECK_PRESERVED, "无法检查目标文件，原文件保留", "Cannot check the destination file. The original file is preserved.") \
    X(TRANSFER_BACKUP_FAILED, "无法保护原文件，已取消覆盖", "Cannot back up the original file. Replacement cancelled.") \
    X(TRANSFER_COMMIT_FAILED, "提交失败，原文件保留", "Save failed. The original file is preserved.") \
    X(TRANSFER_RESTORE_FAILED, "提交失败，原文件备份已保留，恢复 SD 卡后刷新目录重试", "Save failed. The original backup is preserved. Restore the SD card, refresh and retry.") \
    X(TRANSFER_INVALID_STREAM, "压缩流或数据长度错误，原文件保留", "Invalid compressed data or length. The original file is preserved.") \
    X(TRANSFER_UPLOAD_MISSING, "未找到此连接的上传任务", "No upload found for this connection") \
    X(TRANSFER_INVALID_COMMAND, "上传命令无效", "Invalid upload command") \
    X(TRANSFER_PATH_BUSY, "此路径正在上传，请完成后重试", "An upload is using this path. Wait for it to finish.") \
    X(TRANSFER_CREATE_FAILED, "创建失败", "Creation failed") \
    X(TRANSFER_DELETE_ROOT_INVALID, "路径无效或不允许删除根目录", "Invalid path or root directory deletion is not allowed") \
    X(TRANSFER_DIRECTORY_BUSY, "此路径包含正在上传的文件，请完成后重试", "This directory contains an active upload. Wait for it to finish.") \
    X(TRANSFER_DELETE_PARTIAL, "部分项目未删除，可能是文件使用中、路径过长或 SD 卡异常；已删除内容无法恢复，请刷新确认", "Some items could not be deleted. Files may be in use, paths too long, or the SD card faulty. Deleted items cannot be restored. Refresh to check.") \
    X(TRANSFER_PATH_UNAVAILABLE, "路径过长或目录暂不可用", "Path is too long or directory is unavailable") \
    X(TRANSFER_EXISTS, "已存在同名项目", "An item with this name already exists") \
    X(TRANSFER_ITEM_CHECK_FAILED, "无法检查目标项目", "Cannot check the destination item") \
    X(TRANSFER_RENAME_FAILED, "改名失败", "Rename failed") \
    X(TRANSFER_MOVE_SELF, "不能移动到自身或子目录", "Cannot move into itself or a subdirectory") \
    X(TRANSFER_DEST_UNAVAILABLE, "路径过长或目标目录暂不可用", "Path is too long or destination is unavailable") \
    X(TRANSFER_DEST_EXISTS, "目标已存在同名项目", "An item with this name already exists at the destination") \
    X(TRANSFER_MOVE_FAILED, "移动失败", "Move failed") \
    X(TRANSFER_HOTSPOT_STOPPING, "热点正在关闭", "Hotspot is shutting down") \
    X(TRANSFER_REQUEST_BUSY, "文件请求繁忙，请稍后重试", "File service is busy. Try again shortly.") \
    X(CLOUD_WIFI_PREPARING, "正在准备WIFI传输…", "Preparing Wi-Fi transfer...") \
    X(CLOUD_TAB_WIFI, "WIFI传输", "Wi-Fi") \
    X(COMMON_RESTART, "重启", "Restart") \
    X(NETWORK_SCANNING_SHORT, "正在扫描", "Scanning") \
    X(NETWORK_SAVED_COUNT_FMT, "已保存（%d）", "Saved (%d)") \
    X(NETWORK_UNSAVED_COUNT_FMT, "未保存（%d）", "Unsaved (%d)") \
    X(NETWORK_NO_SAVED_NEARBY, "附近暂无已保存网络", "No saved networks nearby") \
    X(NETWORK_NO_UNSAVED_NEARBY, "附近暂无未保存网络", "No unsaved networks nearby") \
    X(NETWORK_TRANSFER_READY, "WIFI传输已就绪", "Wi-Fi transfer is ready") \
    X(NETWORK_TRANSFER_OPEN_HINT, "手机或电脑连接同一WIFI\n扫码或用浏览器打开下方网址", "Connect your phone or computer to the same Wi-Fi network.\nScan the code or open the address below in a browser.") \
    X(NETWORK_TRANSFER_STOP_HINT, "切换传输方式或退出传输页将停止传输", "Switching transfer mode or leaving this page stops the transfer.") \
    X(NETWORK_BACK_TO_LIST, "返回列表", "Back to list") \
    X(NETWORK_CURRENT_SSID_FMT, "当前网络：%s", "Network: %s") \
    X(QUICK_ENABLED, "已开启", "On") \
    X(QUICK_DISABLED, "已关闭", "Off") \
    X(QUICK_SWITCHING, "切换中", "Switching") \
    X(QUICK_FAILED, "请重试", "Retry") \
    X(QUICK_CONNECTED, "已连接", "Connected") \
    X(QUICK_CONNECTING, "连接中", "Connecting") \
    X(QUICK_DISCONNECTED, "未连接", "Offline") \
    X(SETTINGS_DISPLAY_GRAY_HINT, "清屏后显示 16 阶原生灰度；00 为黑，15 为白。", "After clearing, 16 native gray levels are displayed: 00 is black; 15 is white.") \
    X(SETTINGS_DISPLAY_BUSY, "屏幕忙，请稍后重试。", "Display is busy. Try again shortly.") \
    X(SETTINGS_TEST_DISPLAY, "显示测试", "Display test") \
    X(SETTINGS_DISPLAY_START_HINT, "点击“16 阶灰阶”开始测试。\n下方应显示从黑到白的 16 条灰阶，编号 00–15。", "Tap 16 gray levels to start.\n16 bars from black to white should appear below, numbered 00–15.") \
    X(SETTINGS_DISPLAY_GRAY, "16 阶灰阶", "16 gray levels") \
    X(SETTINGS_DISPLAY_DITHER, "点阵灰阶对照", "Dither comparison") \
    X(SETTINGS_DISPLAY_DITHER_HINT, "黑白点阵对照：用黑白像素模拟灰度。", "Dither comparison: black and white pixels simulate gray levels.") \
    X(SETTINGS_DISPLAY_RETEST_HINT, "可重复点击测试；NEXT 重测，PREV 返回。", "Tap to repeat. NEXT retests; PREV goes back.") \
    X(WB_STATUS_QUERYING, "（状态查询中）", "(Checking)") \
    X(WB_STATUS_FAILED, "（查询失败）", "(Check failed)") \
    X(WB_STATUS_ONLINE, "（在线）", "(Online)") \
    X(WB_STATUS_OFFLINE, "（离线）", "(Offline)") \
    X(WB_STATUS_UNKNOWN, "（未知）", "(Unknown)") \
    X(WB_AUTH_CHECKING, "正在检查 WorkBuddy 授权…", "Checking WorkBuddy authorization...") \
    X(WB_SYNC_MESSAGES, "同步消息", "Sync messages") \
    X(WB_REFRESHING, "刷新中", "Refreshing") \
    X(WB_AUTH_SAVED_CHECKING, "设备授权已保存，正在确认电脑连接…", "Authorization saved. Checking the computer connection...") \
    X(WB_CONNECTED, "电脑已连接，可以开始使用。", "Computer connected. Ready to use.") \
    X(WB_BOUND_OFFLINE, "设备已绑定，电脑暂未连接。\n请在电脑打开对应账号的 WorkBuddy，连接后从菜单“同步消息”重试。", "Device linked; computer is offline.\nOpen WorkBuddy on your computer with the linked account, then choose Sync messages from the menu.") \
    X(WB_BOUND_CONNECT_FAILED, "设备已绑定，暂时无法连接电脑：\n", "Device linked. Cannot connect to the computer:\n") \
    X(WB_RELINK_HINT, "\n\n处理提示中的问题后，再点击右上角菜单“重新绑定”。", "\n\nResolve the issue, then choose Relink from the top-right menu.") \
    X(WB_SYNC_RETRY_HINT, "\n\n无需重复扫码，可从菜单“同步消息”重试。", "\n\nChoose Sync messages from the menu to retry. No need to scan again.") \
    X(WB_QR_GENERATING, "正在生成新的授权二维码…", "Generating a new authorization code...") \
    X(WB_SCAN_FRIEND, "请朋友扫码，用自己的账号完成授权", "Ask your friend to scan and authorize with their own account.") \
    X(WB_SCAN_AUTHORIZE, "请用手机扫码登录并授权", "Scan with your phone to sign in and authorize.") \
    X(WB_AUTH_INCOMPLETE, "授权未完成，请刷新二维码重试", "Authorization incomplete. Refresh the code and retry.") \
    X(WB_QR_EXPIRED, "二维码已过期，请点击刷新", "Code expired. Tap Refresh.") \
    X(WB_AUTH_RECONNECTING, "网络暂时中断，正在重新查询授权…", "Connection interrupted. Checking authorization again...") \
    X(WB_AUTHORIZE_PHONE, "请在手机上完成腾讯 WorkBuddy 授权", "Complete Tencent WorkBuddy authorization on your phone.") \
    X(WB_WAIT_CLOSE, "正在等待上一次连接关闭…", "Waiting for the previous connection to close...") \
    X(WB_WAIT_MEMORY, "内存不足，正在等待资源释放…", "Not enough memory. Waiting for resources...") \
    X(WB_RELINK_PREPARING, "正在准备重新绑定…", "Preparing to relink...") \
    X(WB_LINK_CHECKING, "正在检查绑定，请稍候…", "Checking device link. Please wait...") \
    X(WB_REPLY_INTERRUPTED, "助理指令已受理，但自动读取中断：\n", "Request accepted, but reply retrieval was interrupted:\n") \
    X(WB_TASK_CONTINUES_HINT, "\n\n助理任务可能继续执行。可从菜单“同步消息”查看，请勿重复发送同一指令。", "\n\nThe task may still be running. Choose Sync messages to check. Avoid sending the same request again.") \
    X(WB_REPLY_TIMEOUT, "助理指令已受理，三分钟内暂未读到回复。助理任务仍可能继续执行，稍后可从菜单“同步消息”查看，请勿重复发送同一指令。", "Request accepted, but no reply received within three minutes. The task may still be running. Choose Sync messages later to check. Avoid sending the same request again.") \
    X(WB_RECORDING_CANCELLED, "录音已取消", "Recording cancelled") \
    X(AUDIO_MODULE_INIT_FAILED, "音频模块初始化失败，请检查模块连接后重启设备", "Audio module initialization failed. Check the connection and restart the device.") \
    X(AUDIO_MODULE_NOT_READY, "音频模块尚未就绪，请稍后重试", "Audio module is not ready. Try again shortly.") \
    X(WB_RECORDING_TASK_BUSY, "录音任务暂忙，请稍后重试", "Recording is busy. Try again shortly.") \
    X(WB_SEND_UNCONFIRMED_HINT, "\n\n发送结果尚未确认，请先查看助理消息，避免重复发送同一指令。", "\n\nDelivery is unconfirmed. Check assistant messages before sending the request again.") \
    X(WB_RECOGNITION_INCOMPLETE_HINT, "\n\n本次识别未完成，指令未发送到电脑。", "\n\nRecognition did not finish. The request was not sent to the computer.") \
    X(WB_PREVIOUS_CLOSING, "上一轮连接正在关闭，请稍后再试", "The previous connection is closing. Try again shortly.") \
    X(WB_CONTEXT_MEMORY, "请求上下文内存不足，请稍后再试", "Not enough memory for the request. Try again shortly.") \
    X(WB_TASK_MEMORY, "语音或消息任务内存不足，请稍后再试", "Not enough memory for voice or messages. Try again shortly.") \
    X(WB_HOLD_TO_SPEAK_HINT, "请持续按住，看到倾听波形后再说话（至少半秒）", "Keep holding. Speak after the listening waveform appears (at least half a second).") \
    X(WB_RECORDING_FINISH_BUSY, "录音收尾任务暂忙，请稍后重试", "Recording is finishing. Try again shortly.") \
    X(WB_MIC_READ_FAILED, "麦克风读取失败，请重新进入应用", "Cannot read microphone. Reopen the app.") \
    X(WB_MINUTE_SUFFIX, " 分钟", " min") \
    X(WB_SECOND_SUFFIX, " 秒", " sec") \
    X(WB_CONFIRM_PHONE, "请在手机上确认绑定", "Confirm the device link on your phone.") \
    X(WB_QR_VALIDITY_PREFIX, "手机扫码绑定助理\n二维码有效期 ", "Scan with your phone to link the assistant\nCode valid for ") \
    X(WB_QR_FAILED, "二维码生成失败，请点击刷新重试。", "Cannot generate code. Tap Refresh to retry.") \
    X(WB_QR_DISPLAY_FAILED, "二维码暂时无法显示", "Cannot display the code") \
    X(WB_RELINK_WAIT, "正在准备重新绑定，请稍候…", "Preparing to relink. Please wait...") \
    X(WB_RECOGNIZING, "正在识别…", "Recognizing...") \
    X(WB_SYNCING, "正在同步消息…", "Syncing messages...") \
    X(WB_SENDING, "正在发送…", "Sending...") \
    X(WB_PROCESSING, "助理正在处理…", "Assistant is working...") \
    X(WB_NO_MESSAGES, "暂无可同步的文本消息", "No text messages to sync") \
    X(WB_SYNCED, "消息已同步", "Messages synced") \
    X(WB_LAYOUT_FAILED, "消息排版失败，请稍后重试", "Cannot lay out messages. Try again shortly.") \
    X(WB_RELINK, "重新绑定", "Relink") \
    X(WB_RELINK_TITLE, "重新绑定助理账号？", "Relink the assistant account?") \
    X(WB_RELINK_WARNING, "新账号授权成功前，保留原绑定。已提交的任务可能继续执行。", "The current link is kept until the new account is authorized. Submitted tasks may continue running.") \
    X(WB_RELINK_CANCEL, "暂不更换", "Keep current") \
    X(WB_RELINK_CONTINUE, "继续绑定", "Continue") \
    X(WB_SCAN_LINK, "请使用手机扫码绑定", "Scan with your phone to link the device.") \
    X(WB_STOPPED_HINT, "本次录音或等待已停止。已提交的任务可能继续执行，可同步消息查看。", "Recording or waiting stopped. Submitted tasks may continue running. Sync messages to check.") \
    X(WB_STOPPED, "已停止录音或等待，可继续对话", "Recording or waiting stopped. You can continue chatting.") \
    X(WB_WELCOME_READY, "已经准备好了，随时可以开始对话。", "Ready whenever you are. Start a conversation.") \
    X(WB_WELCOME_TASK, "今天想处理些什么？我来帮你一起完成。", "What would you like to work on today?") \
    X(WB_WELCOME_IDEAS, "有什么想法或安排，都可以说给我听。", "Tell me about your ideas or plans.") \
    X(WB_HISTORY_MEMORY, "历史内存不足，请稍后重试", "Not enough memory for chat history. Try again shortly.") \
    X(WB_ME_PREFIX, "我：", "Me: ") \
    X(WB_ASSISTANT_PREFIX, "助理：", "Assistant: ") \
    X(AUDIO_SERVICE_START_FAILED, "音频服务启动失败，请稍后重试", "Audio service failed to start. Try again shortly.") \
    X(WB_STORAGE_TASK_BUSY, "设备存储任务暂忙，请稍后重试", "Device storage task is busy. Try again shortly.") \
    X(WB_STORAGE_BUSY, "设备存储暂忙，请稍后重试", "Device storage is busy. Try again shortly.") \
    X(WB_ENDPOINT_MISSING, "尚未配置 WorkBuddy 服务地址", "WorkBuddy service address is not configured.") \
    X(WB_NETWORK_SETTINGS_HINT, "网络未连接，请先在设置中连接网络", "No network. Connect in Settings first.") \
    X(WB_CONNECTION_CREATE_FAILED, "无法创建网络连接，请稍后再试", "Cannot create a network connection. Try again shortly.") \
    X(WB_SERVICE_TIMEOUT, "服务连接失败或请求超时，请稍后重试", "Service connection failed or request timed out. Try again shortly.") \
    X(WB_RESPONSE_TIMEOUT, "等待服务响应超时，请稍后重试", "Service response timed out. Try again shortly.") \
    X(WB_RESPONSE_READ_FAILED, "服务响应读取失败，请稍后重试", "Cannot read service response. Try again shortly.") \
    X(WB_RESPONSE_TOO_LONG, "服务响应过长，请稍后重试", "Service response is too long. Try again shortly.") \
    X(WB_HTTP_ERROR_PREFIX, "服务暂时不可用（HTTP ", "Service unavailable (HTTP ") \
    X(WB_RESPONSE_INVALID, "服务返回异常，请稍后重试", "Invalid service response. Try again shortly.") \
    X(WB_NO_REPLY, "服务没有返回回复，请稍后重试", "No reply from the service. Try again shortly.") \
    X(WB_VOICE_ENDPOINT_MISSING, "尚未配置实时语音服务地址", "Live voice service address is not configured.") \
    X(WB_TOKEN_MISSING_SCAN, "设备授权凭证缺失，请重新扫码授权", "Authorization credentials are missing. Scan again to authorize.") \
    X(WB_VOICE_ENDPOINT_INVALID, "实时语音服务地址格式无效", "Invalid live voice service address") \
    X(WB_RECORDING_BUFFER_MEMORY, "录音缓存内存不足，请稍后再试", "Not enough recording buffer memory. Try again shortly.") \
    X(WB_TOKEN_NOT_SAVED, "设备尚未保存授权凭证", "No authorization credentials saved on the device") \
    X(WB_ONLINE_STATUS_INVALID, "服务没有返回有效的电脑在线状态", "Service returned no valid computer connection status.") \
    X(WB_AUTH_STATUS_MISSING, "服务未返回设备授权状态", "Service returned no device authorization status.") \
    X(WB_SCAN_CANCELLED, "扫码请求已取消", "Scan request cancelled") \
    X(WB_RELINK_UNSUPPORTED, "服务暂不支持重新绑定，请联系管理员更新服务", "Relinking is unavailable. Ask the administrator to update the service.") \
    X(WB_QR_RESPONSE_INVALID, "服务返回的扫码信息无效", "Service returned invalid scan information.") \
    X(WB_AUTH_SAVE_FAILED, "无法保存设备授权，请释放存储空间后重试", "Cannot save device authorization. Free storage space and retry.") \
    X(WB_SCAN_SESSION_INVALID, "扫码会话无效，请刷新二维码", "Invalid scan session. Refresh the code.") \
    X(WB_AUTH_STATUS_INVALID, "服务返回的授权状态无效，请刷新二维码", "Invalid authorization status. Refresh the code.") \
    X(WB_TOKEN_SAVE_FAILED, "授权成功但设备凭证保存失败，请重新进入应用重试", "Authorized, but credentials could not be saved. Reopen the app and retry.") \
    X(WB_ENTER_QUESTION, "请先输入问题", "Enter a question first.") \
    X(WB_QUESTION_INVALID, "问题过长或包含无效字符，请缩短后重试", "Question is too long or contains invalid characters. Shorten it and retry.") \
    X(WB_AUTHORIZE_FIRST, "请先扫码登录并授权 WorkBuddy", "Scan to sign in and authorize WorkBuddy first.") \
    X(WB_TEXT_MEMORY, "内存不足，暂时无法发送文字", "Not enough memory to send text.") \
    X(WB_MESSAGE_INVALID, "消息格式不正确，请更新服务后重试", "Invalid message format. Update the service and retry.") \
    X(WB_REQUEST_ID_INVALID, "电脑受理编号无效，无法自动读取回复", "Invalid computer request ID. Cannot retrieve replies automatically.") \
    X(WB_TOKEN_MISSING, "设备授权凭证缺失，请重新授权", "Authorization credentials are missing. Authorize again.") \
    X(WB_AUTO_REPLY_UNAVAILABLE, "自动回复接口尚不可用，请先更新Java服务；可点电脑消息手动查看", "Automatic replies are unavailable. Update the Java service, or sync messages manually.") \
    X(WB_AUTO_REPLY_MISMATCH, "自动回复数据与本次受理编号不匹配，请稍后手动查看电脑消息", "Reply does not match this request. Sync messages manually later.") \
    X(WB_VOICE_RESPONSE_FORMAT, "实时语音响应格式异常", "Invalid live voice response format") \
    X(WB_VOICE_RESPONSE_INVALID, "实时语音响应无效", "Invalid live voice response") \
    X(WB_VOICE_SESSION_MISMATCH, "实时语音会话编号不匹配", "Live voice session ID mismatch") \
    X(WB_VOICE_RECOGNITION_FAILED, "实时语音识别失败，请重新说话", "Live voice recognition failed. Speak again.") \
    X(WB_VOICE_TEXT_INVALID, "实时识别文字为空、过长或顺序错误", "Recognized text is empty, too long or out of order.") \
    X(WB_VOICE_VERSION, "实时语音协议版本不兼容，请更新Java服务", "Incompatible live voice protocol. Update the Java service.") \
    X(WB_VOICE_BUFFER_FAILED, "实时录音缓冲申请失败", "Cannot allocate live recording buffer") \
    X(WB_VOICE_NETWORK_HINT, "网络未连接，请重新连接后说话", "No network. Reconnect before speaking.") \
    X(WB_VOICE_CONNECTION_CREATE, "无法创建实时语音连接", "Cannot create live voice connection") \
    X(WB_VOICE_TRANSFER_FAILED, "实时语音传输异常，请重新说话", "Live voice transfer failed. Speak again.") \
    X(WB_VOICE_CONNECTION_FAILED, "实时语音连接失败，请检查Java版本、WebSocket代理和设备授权", "Live voice connection failed. Check the Java service version, WebSocket proxy and device authorization.") \
    X(WB_VOICE_EARLY_END, "识别提前结束，指令未发送", "Recognition ended early. Request not sent.") \
    X(WB_VOICE_DISCONNECTED, "语音连接中断，指令未发送，请重新说话", "Voice disconnected. Request not sent. Speak again.") \
    X(WB_VOICE_READY_TIMEOUT, "等待语音服务就绪超时，请稍后重试", "Voice service did not become ready in time. Try again shortly.") \
    X(WB_VOICE_FINAL_TIMEOUT, "等待最终识别结果超时，请重新说话", "Final recognition result timed out. Speak again.") \
    X(WB_VOICE_OVERFLOW, "实时音频待发送缓冲已满，请检查网络后重试", "Pending audio buffer is full. Check the network and retry.") \
    X(WB_VOICE_BUFFER_UNAVAILABLE, "实时录音缓冲不可用，请重新进入应用", "Live recording buffer is unavailable. Reopen the app.") \
    X(WB_VOICE_PCM_INVALID, "录音数据无效，请重新进入应用", "Recording data is invalid. Reopen the app.") \
    X(WB_VOICE_SEND_FAILED, "实时音频发送失败，请重新说话", "Live audio send failed. Speak again.") \
    X(WB_VOICE_TOO_SHORT, "录音不足半秒，请按住并说话更久", "Recording is shorter than half a second. Hold and speak longer.") \
    X(WB_VOICE_INCOMPLETE, "录音发送字节数不匹配，请重新说话", "Sent audio byte count does not match the recording. Speak again.") \
    X(WB_VOICE_FINISH_FAILED, "语音结束确认发送失败", "Cannot send end-of-voice confirmation") \
    X(WB_VOICE_CANCELLED, "本次语音已取消，指令未发送", "Voice cancelled. Request not sent.") \
    X(BOOK_BOOKMARK_LAYOUT_BUSY, "正文正在编排，请稍后添加书签", "Laying out text. Add the bookmark shortly.") \
    X(BOOK_BOOKMARK_ADD_MEMORY, "内存不足，无法添加书签", "Not enough memory to add a bookmark") \
    X(BOOK_BOOKMARK_LOCATE_FAILED, "无法定位书签，请检查书籍是否发生变化", "Cannot locate bookmark. Check whether the book has changed.") \
    X(ASSISTANT_AI_PREFIX, "AI：", "AI: ") \
    X(BOOK_BOOKMARK_IMAGE_SUFFIX, "（图片或章节页）", " (Image or chapter page)") \
    X(BOOK_BOOKMARK_IMAGE, "图片或章节页", "Image or chapter page") \
    X(WIFI_DISABLED_RETRY, "Wi-Fi 已关闭，请打开 Wi-Fi 后重试", "Wi-Fi is off. Turn it on and try again.") \
    X(AUDIO_INIT_FAILED, "音频初始化失败", "Audio initialization failed") \
    X(SETTINGS_TEST_CAMERA_DETECTED, "已检测到", "Detected") \
    X(SETTINGS_TEST_CAMERA_NOT_DETECTED, "未检测到", "Not detected") \
    X(SETTINGS_TEST_CAMERA_SHOT, "拍照", "Capture") \
    X(SETTINGS_TEST_CAMERA_RESULT, "拍照结果", "Camera result") \
    X(SETTINGS_TEST_CAPTURING, "拍照中…", "Capturing…") \
    X(SETTINGS_TEST_DETECTING, "检测中…", "Detecting…") \
    X(SETTINGS_TEST_TASK_CREATE_FAIL, "任务创建失败", "Task creation failed") \
    X(SETTINGS_TEST_CELL_EXT, "4G·外置", "4G·External") \
    X(SETTINGS_TEST_CELL_INT, "4G·内置", "4G·Internal") \
    X(SETTINGS_TEST_SCANNING, "扫描中…", "Scanning…") \
    X(SETTINGS_TEST_WIFI_CLOSE, "关闭", "Close") \
    X(SETTINGS_TEST_WIFI_COUNT_FMT, "共 %d 个网络", "%d network(s)") \
    X(SETTINGS_TEST_WIFI_FOUND_FMT, "发现 %d 个", "Found %d") \
    X(SETTINGS_TEST_WIFI_NEARBY, "附近 WiFi", "Nearby Wi-Fi") \
    X(SETTINGS_TEST_WIFI_NEXT, "下页", "Next") \
    X(SETTINGS_TEST_WIFI_NONE, "未发现网络", "No networks") \
    X(SETTINGS_TEST_WIFI_PREV, "上页", "Prev") \
    X(SETTINGS_TEST_WIFI_RESCAN, "重扫", "Rescan") \
    X(SETTINGS_TEST_WIFI_SCANNING, "正在扫描…", "Scanning…") \
    X(SETTINGS_TEST_WIFI_SCAN_FAIL, "扫描失败", "Scan failed") \
    X(REGISTERING_NETWORK, "等待网络...", "Waiting for network...") \
    X(SETTINGS_NET_BOARD_NO_AP, "当前板型不支持配网模式", "This board has no Wi-Fi config mode") \
    X(SETTINGS_NET_CFUN0, "正在关闭射频…\nAT+CFUN=0", "RF off…\nAT+CFUN=0") \
    X(SETTINGS_NET_CFUN0_FAIL, "AT+CFUN=0 执行失败", "AT+CFUN=0 failed") \
    X(SETTINGS_NET_CFUN1, "正在重新搜网…\nAT+CFUN=1", "Searching network…\nAT+CFUN=1") \
    X(SETTINGS_NET_CFUN1_FAIL, "恢复4G通信失败，请重试或重启设备", "Could not restore 4G. Retry or restart the device.") \
    X(SETTINGS_NET_CURRENT_FMT, "当前：%s", "Current: %s") \
    X(SETTINGS_NET_CUR_4G, "当前：4G 上网", "Current: Cellular") \
    X(SETTINGS_NET_CUR_DASH, "当前：--", "Current: --") \
    X(SETTINGS_NET_ECSIMCFG_FAIL, "AT+ECSIMCFG 执行失败", "AT+ECSIMCFG failed") \
    X(SETTINGS_NET_MODE_4G, "4G 上网", "Cellular") \
    X(SETTINGS_NET_MODE_TITLE, "上网方式", "Network mode") \
    X(SETTINGS_NET_MODE_WIFI, "WiFi 上网", "Wi-Fi") \
    X(SETTINGS_NET_NOT_4G, "当前不在 4G 模式，无法切换", "Not in 4G mode, cannot switch") \
    X(SETTINGS_NET_NO_4G, "未检测到 4G 模块", "No 4G module") \
    X(SETTINGS_NET_REBOOT_HINT, "切换后设备将自动重启", "Device reboots after switching") \
    X(SETTINGS_NET_SIM_EXT, "外置卡", "External SIM") \
    X(SETTINGS_NET_SIM_INT, "内置卡", "Internal SIM") \
    X(SETTINGS_NET_SIM_TASK_FAIL, "无法启动 SIM 切换任务", "Cannot start SIM switch task") \
    X(SETTINGS_NET_SIM_TITLE, "SIM 卡", "SIM") \
    X(SETTINGS_NET_SWITCHED_FMT, "已切换到%s", "Switched to %s") \
    X(SETTINGS_NET_SWITCH_4G, "正在切换到 4G…", "Switching to 4G…") \
    X(SETTINGS_NET_SWITCH_SIM_CFUN0_FMT, "正在切换到%s…\nAT+CFUN=0", "Switching to %s…\nAT+CFUN=0") \
    X(SETTINGS_NET_SWITCH_SIM_FMT, "正在切换到%s…\n%s", "Switching to %s…\n%s") \
    X(SETTINGS_NET_SWITCH_WIFI, "正在切换到 WiFi…", "Switching to Wi-Fi…") \
    X(SETTINGS_TEST_CELL_CANCELLED, "测试已取消", "Test cancelled") \
    X(SETTINGS_TEST_CELL_CFUN0_FAIL, "CFUN=0失败", "CFUN=0 failed") \
    X(SETTINGS_TEST_CELL_CFUN1_FAIL, "CFUN=1失败", "CFUN=1 failed") \
    X(SETTINGS_TEST_CELL_LOSS_FMT, "丢包 %d/%d", "Loss %d/%d") \
    X(SETTINGS_TEST_CELL_MODEM_NOT_READY, "模组未就绪", "Modem not ready") \
    X(SETTINGS_TEST_CELL_MODEM_NO_RESP, "模组无响应", "Modem no response") \
    X(SETTINGS_TEST_CELL_NEED_4G, "需切到4G模式", "Switch to 4G mode") \
    X(SETTINGS_TEST_CELL_NO_RESOURCE, "资源不足", "Out of resources") \
    X(SETTINGS_TEST_CELL_NO_SIM, "未检测到SIM卡", "No SIM detected") \
    X(SETTINGS_TEST_CELL_PING_CMD_FAIL, "PING命令失败", "PING command failed") \
    X(SETTINGS_TEST_CELL_PING_FAIL, "PING失败", "PING failed") \
    X(SETTINGS_TEST_CELL_PING_OK, "PING成功", "PING OK") \
    X(SETTINGS_TEST_CELL_PING_WAIT_TIMEOUT, "PING等待超时", "PING wait timeout") \
    X(SETTINGS_TEST_CELL_REG_REJECT, "网络注册被拒绝", "Registration rejected") \
    X(SETTINGS_TEST_CELL_SEARCH_TIMEOUT, "搜网超时", "Network search timeout") \
    X(SETTINGS_TEST_CELL_SIM_SWITCH_FAIL, "切换SIM失败", "SIM switch failed") \
    X(SETTINGS_TEST_CELL_SLOT_FAIL, "SIM槽位未生效", "SIM slot not applied") \
    X(SETTINGS_TEST_CELL_TESTING, "测试中…", "Testing…") \
    X(SETTINGS_TEST_CELL_TEST_FAIL, "测试失败", "Test failed") \
    X(SETTINGS_TEST_CELL_WAIT_RESOURCE, "等待资源…", "Waiting for resources…") \
    X(SETTINGS_TEST_SIGNAL_BOARD_UNSUP, "板型不支持", "Board unsupported") \
    X(SETTINGS_TEST_SIGNAL_CELL_CSQ_FMT, "4G CSQ %2d", "4G CSQ %2d") \
    X(SETTINGS_TEST_SIGNAL_CELL_CSQ_UNKNOWN, "4G CSQ 未知", "4G CSQ unknown") \
    X(SETTINGS_TEST_SIGNAL_CELL_UNAVAIL, "4G 不可用", "4G unavailable") \
    X(SETTINGS_TEST_SIGNAL_READING, "读取中…", "Reading…") \
    X(SETTINGS_TEST_SIGNAL_TITLE, "网络信号", "Signal") \
    X(SETTINGS_TEST_SIGNAL_WIFI_DISCONNECTED, "WiFi 未连接", "Wi-Fi disconnected") \
    X(SETTINGS_TEST_SIGNAL_WIFI_READ_FAIL, "WiFi 读取失败", "Wi-Fi read failed") \
    X(SETTINGS_TEST_SIGNAL_WIFI_RSSI_FMT, "WiFi %d dBm", "Wi-Fi %d dBm") \
    X(SWITCH_TO_4G_NETWORK, "切换到 4G...", "Switching to 4G...") \
    X(SWITCH_TO_WIFI_NETWORK, "切换到 Wi-Fi...", "Switching to Wi-Fi...") \
    X(SETTINGS_TAB_LOG, "日志", "Logs") \
    X(SETTINGS_LOG_TITLE, "设备日志", "Device logs") \
    X(SETTINGS_LOG_ON, "开启", "On") \
    X(SETTINGS_LOG_OFF, "关闭", "Off") \
    X(SETTINGS_LOG_CURRENT_ON, "当前：开启", "Current: On") \
    X(SETTINGS_LOG_CURRENT_OFF, "当前：关闭", "Current: Off") \
    X(SETTINGS_LOG_HINT, "开启后将运行日志写入SD卡：metalio/e-ink/logs/metalio_logs.txt", "When on, runtime logs are saved to SD: metalio/e-ink/logs/metalio_logs.txt") \
    X(SETTINGS_LOG_HINT_OFF, "关闭后停止采集；虚拟U盘启用期间暂停写盘", "Off stops capture. SD writing pauses while USB disk is enabled") \
    X(SETTINGS_LOG_CLEAR, "清除日志", "Clear logs") \
    X(SETTINGS_LOG_CLEARING, "正在清理…", "Clearing…") \
    X(SETTINGS_LOG_CLEAR_OK, "日志已清除", "Logs cleared") \
    X(SETTINGS_LOG_CLEAR_FAIL, "清理失败，请检查SD卡或稍后重试", "Clear failed. Check SD card or try again.") \
    X(SETTINGS_LOG_UPLOAD, "上传日志", "Upload logs") \
    X(SETTINGS_LOG_CONNECTING_NET, "正在连接网络", "Connecting to network") \
    X(SETTINGS_LOG_UPLOAD_COUNT_FMT, "已完成 %u，待上传 %u", "Completed %u, remaining %u") \
    X(SETTINGS_LOG_UPLOADING, "正在上传日志", "Uploading logs") \
    X(SETTINGS_LOG_UPLOAD_OK, "日志上传成功", "Logs uploaded") \
    X(SETTINGS_LOG_UPLOAD_FAIL, "日志上传失败，请重试", "Upload failed. Please retry") \
    X(SETTINGS_LOG_UPLOAD_NO_FILE, "没有可上传的日志", "No logs to upload") \
    X(SETTINGS_LOG_CANCELLING, "正在取消，请稍候", "Cancelling, please wait") \
    X(SETTINGS_LOG_PREPARING, "正在准备日志", "Preparing logs") \
    X(SETTINGS_LOG_SD_UNAVAILABLE, "SD卡不可用，请关闭虚拟U盘后重试", "SD unavailable. Disable USB disk and retry") \
    X(SETTINGS_LOG_NO_MEMORY, "内存不足，请稍后重试", "Not enough memory. Please retry later") \
    X(HOME_APP_TASK, "每日清单", "Tasks")                                                                      \
    X(HOME_APP_ASSISTANT, "百问AI", "Ask AI")                                                                  \
    X(HOME_APP_BOOK, "阅读", "Books")                                                                          \
    X(HOME_APP_WALLPAPER, "壁纸", "Walls")                                                                     \
    X(HOME_APP_CLOUD, "传输", "Transfer")                                                                      \
    X(HOME_APP_SETTINGS, "设置", "Settings")                                                                   \
    X(HOME_DATE_PLACEHOLDER, "--月--日", "--/--")                                                              \
    X(HOME_DATE_FMT, "%02d月%02d日", "%02d/%02d")                                                              \
    X(HOME_DATE_SLASH_PLACEHOLDER, "----.-.--", "----.-.--")                                                   \
    X(HOME_DATE_SLASH_FMT, "%d.%d.%d", "%d.%d.%d")                                                             \
    X(HOME_WDAY_SUN, "周日", "Sun")                                                                            \
    X(HOME_WDAY_MON, "周一", "Mon")                                                                            \
    X(HOME_WDAY_TUE, "周二", "Tue")                                                                            \
    X(HOME_WDAY_WED, "周三", "Wed")                                                                            \
    X(HOME_WDAY_THU, "周四", "Thu")                                                                            \
    X(HOME_WDAY_FRI, "周五", "Fri")                                                                            \
    X(HOME_WDAY_SAT, "周六", "Sat")                                                                            \
    X(BATTERY_NEED_CHARGE, "电量不足，请充电", "Low battery, please charge")                                   \
    X(COMMON_UNKNOWN, "未知", "Unknown")                                                                       \
    X(POWERED_OFF, "已关机", "Powered off")                                                                    \
    X(SETTINGS_TAB_NETWORK, "网络", "Network")                                                                 \
    X(SETTINGS_TAB_THEME, "主题", "Theme")                                                                     \
    X(SETTINGS_TAB_LANGUAGE, "语言", "Language")                                                               \
    X(SETTINGS_TAB_HAPTIC, "震动", "Haptic")                                                                   \
    X(SETTINGS_TAB_CHARGING, "充电", "Charging") \
    X(SETTINGS_CHARGING_CURRENT, "充电电流", "Charge current") \
    X(SETTINGS_CHARGING_HINT, "默认 1A，选择后自动保存", "Default: 1A. Selection is saved automatically.") \
    X(SETTINGS_CHARGING_FAILED, "设置失败，请重试", "Could not apply setting. Try again.") \
    X(SETTINGS_TAB_POWER, "功耗", "Power")                                                                     \
    X(SETTINGS_SCREEN_BLACK_DEPTH_TITLE, "黑色深度", "Black depth") \
    X(SETTINGS_SCREEN_BLACK_DEPTH_DEFAULT, "默认", "Default") \
    X(SETTINGS_SCREEN_BLACK_DEPTH_MEDIUM, "中", "Medium") \
    X(SETTINGS_SCREEN_BLACK_DEPTH_STRONG, "强", "Strong") \
    X(SETTINGS_SCREEN_BLACK_DEPTH_HINT, "提高黑色深度可能影响屏幕寿命，并增加底色变化风险，建议优先使用默认档。", "Increasing black depth may affect display lifespan and cause background discoloration. Default is recommended.") \
    X(SETTINGS_SCREEN_REFRESH_TITLE, "刷新模式", "Refresh mode") \
    X(SETTINGS_SCREEN_REFRESH_CLEAR, "清晰", "Clear") \
    X(SETTINGS_SCREEN_REFRESH_CLEAR_HINT, "细节更清晰，残影更少。", "Sharper detail, less ghosting.") \
    X(SETTINGS_SCREEN_REFRESH_BALANCED_HINT, "兼顾画质与速度，可能加重残影。", "Balanced quality and speed; may increase ghosting.") \
    X(SETTINGS_SCREEN_REFRESH_BALANCED, "均衡", "Balanced") \
    X(SETTINGS_SCREEN_REFRESH_FAST, "快速", "Fast") \
    X(SETTINGS_SCREEN_REFRESH_EXTREME, "极速", "Ultra fast") \
    X(SETTINGS_SCREEN_REFRESH_EXTREME_HINT, "翻页最快，可能加重残影。", "Fastest page turns; may increase ghosting.") \
    X(SETTINGS_SCREEN_REFRESH_FAST_HINT, "保留灰阶，响应更快；可能加重残影。", "Faster response with grayscale retained; may increase ghosting.") \
    X(SETTINGS_TAB_FRONTLIGHT, "屏幕", "Screen")                                                                \
    X(SETTINGS_TAB_CONVERSATION, "对话", "Talk")                                                               \
    X(SETTINGS_TAB_STORAGE, "存储", "Storage")                                                                 \
    X(SETTINGS_TAB_BLUETOOTH, "蓝牙", "Bluetooth")                                                             \
    X(SETTINGS_TAB_TEST, "测试", "Test")                                                                       \
    X(SETTINGS_TAB_ABOUT, "关于", "About")                                                                     \
    X(SETTINGS_THEME_TITLE, "首页主题", "Home theme")                                                          \
    X(SETTINGS_THEME_HINT, "返回首页后立即生效", "Applies when you return home")                               \
    X(SETTINGS_THEME_PIXEL, "像素视界", "Pixel Vision") \
    X(SETTINGS_THEME_CURRENT_PIXEL, "当前：像素视界", "Current: Pixel Vision") \
    X(HOME_PIXEL_TODAY, "今日事项", "Today") \
    X(HOME_PIXEL_PENDING, "未完成", "Pending") \
    X(HOME_PIXEL_DAY_NOTE, "诸事皆宜", "A good day") \
    X(HOME_PIXEL_READ, "阅读", "Read") \
    X(HOME_PIXEL_NOTES, "便签", "Notes") \
    X(SETTINGS_THEME_WHITE, "白底", "White")                                                                   \
    X(SETTINGS_THEME_GRAY, "网点灰底", "Dotted gray")                                                          \
    X(SETTINGS_THEME_BORDER, "黑色描边", "Black border")                                                       \
    X(SETTINGS_THEME_SLASH, "斜切棋盘", "Slash grid")                                                          \
    X(SETTINGS_THEME_CURRENT_WHITE, "当前：白底", "Current: White")                                            \
    X(SETTINGS_THEME_CURRENT_GRAY, "当前：网点灰底", "Current: Dotted gray")                                   \
    X(SETTINGS_THEME_CURRENT_BORDER, "当前：黑色描边", "Current: Black border")                                \
    X(SETTINGS_THEME_CURRENT_SLASH, "当前：斜切棋盘", "Current: Slash grid")                                   \
    X(SETTINGS_LANG_TITLE, "界面语言", "Language")                                                             \
    X(SETTINGS_LANG_HINT, "切换后立即生效", "Takes effect immediately")                                        \
    X(SETTINGS_LANG_ZH_CN, "简体中文", "简体中文")                                                             \
    X(SETTINGS_LANG_EN_US, "English", "English")                                                               \
    X(SETTINGS_LANG_CURRENT_ZH, "当前：简体中文", "Current: 简体中文")                                         \
    X(SETTINGS_LANG_CURRENT_EN, "当前：English", "Current: English")                                           \
    X(SETTINGS_HAPTIC_TITLE, "按键震动", "Button haptic")                                                      \
    X(SETTINGS_HAPTIC_HINT, "界面按钮短震反馈", "UI buttons and cover keys")                                   \
    X(SETTINGS_HAPTIC_ON, "开启", "On")                                                                        \
    X(SETTINGS_HAPTIC_OFF, "关闭", "Off")                                                                      \
    X(SETTINGS_HAPTIC_CURRENT_ON, "当前：开启", "Current: On")                                                 \
    X(SETTINGS_HAPTIC_CURRENT_OFF, "当前：关闭", "Current: Off")                                               \
    X(SETTINGS_HAPTIC_INTENSITY_TITLE, "震动强度", "Haptic strength") \
    X(SETTINGS_HAPTIC_WEAK, "弱", "Weak") \
    X(SETTINGS_HAPTIC_MEDIUM, "中", "Medium") \
    X(SETTINGS_HAPTIC_STRONG, "强", "Strong") \
    X(SETTINGS_HAPTIC_CURRENT_WEAK, "当前：弱", "Current: Weak") \
    X(SETTINGS_HAPTIC_CURRENT_MEDIUM, "当前：中", "Current: Medium") \
    X(SETTINGS_HAPTIC_CURRENT_STRONG, "当前：强", "Current: Strong") \
    X(SETTINGS_POWER_3_MIN, "3 分", "3 min")                                                                   \
    X(SETTINGS_POWER_10_MIN, "10 分", "10 min")                                                                \
    X(SETTINGS_POWER_30_MIN, "30 分", "30 min")                                                                \
    X(SETTINGS_POWER_3_HOUR, "3 时", "3 h")                                                                    \
    X(SETTINGS_POWER_6_HOUR, "6 时", "6 h")                                                                    \
    X(SETTINGS_POWER_30_SEC, "30 秒", "30 s")                                                                  \
    X(SETTINGS_POWER_60_SEC, "60 秒", "60 s")                                                                  \
    X(SETTINGS_POWER_120_SEC, "120 秒", "120 s")                                                               \
    X(SETTINGS_POWER_OFF_NEVER, "永不", "Never")                                                               \
    X(SETTINGS_POWER_IDLE_MHZ_TITLE, "空闲降频（默认320Mhz）", "Idle CPU (def 320 MHz)")                       \
    X(SETTINGS_POWER_IDLE_MHZ_HINT, "无操作空闲时的 CPU 频率", "CPU clock when idle")                            \
    X(SETTINGS_POWER_NET_GRACE_TITLE, "保网时长（默认 30 秒）", "Keep-alive (def 30 s)")                        \
    X(SETTINGS_POWER_NET_GRACE_HINT, "无保网需求后仍联网的时长；待机仍断网", "Time online after last need; standby disconnects") \
    X(SETTINGS_POWER_STANDBY_TITLE, "浅睡待机（默认 3 分钟）", "Light sleep (def 3 min)")                       \
    X(SETTINGS_POWER_STANDBY_HINT, "无操作多久进入浅睡待机", "Idle time before light sleep")                     \
    X(SETTINGS_POWER_OFF_TITLE, "自动关机（默认 30 分钟）", "Auto power-off (def 30 min)")                      \
    X(SETTINGS_POWER_OFF_HINT, "浅睡累计多久自动关机", "Light-sleep time before power-off")                      \
    X(SETTINGS_CONV_TITLE, "语音播报", "Voice playback")                                                       \
    X(SETTINGS_CONV_HINT, "控制百问AI是否播报语音\n关闭后仅显示文字", "Whether Ask AI speaks aloud\nOff = text only") \
    X(SETTINGS_CONV_TTS_ON, "开启 TTS", "TTS on")                                                              \
    X(SETTINGS_CONV_TTS_OFF, "关闭 TTS", "TTS off")                                                            \
    X(SETTINGS_CONV_CUR_ON, "当前：开启 TTS", "Current: TTS on")                                               \
    X(SETTINGS_CONV_CUR_OFF, "当前：关闭 TTS", "Current: TTS off")                                             \
    X(SETTINGS_CONV_CUR_DASH, "当前：—", "Current: —")                                                         \
    X(SETTINGS_CONV_PARSE_FAIL, "响应解析失败", "Response parse failed")                                       \
    X(SETTINGS_CONV_REQUEST_FAIL, "请求失败", "Request failed")                                                \
    X(SETTINGS_CONV_MISSING_ENABLED, "缺少 enabled", "Missing enabled")                                        \
    X(SETTINGS_CONV_NO_NETWORK, "无网络", "No network")                                                         \
    X(SETTINGS_CONV_CONN_FAIL, "创建连接失败", "Connection failed")                                           \
    X(SETTINGS_CONV_NET_NOT_READY, "网络未就绪", "Network not ready")                                          \
    X(SETTINGS_CONV_ENABLING, "正在开启…", "Enabling…")                                                        \
    X(SETTINGS_CONV_DISABLING, "正在关闭…", "Disabling…")                                                      \
    X(SETTINGS_CONV_BUSY, "忙，请稍候", "Busy, please wait")                                                   \
    X(SETTINGS_CONV_CONNECTING, "连接网络…", "Connecting…")                                                    \
    X(SETTINGS_STORAGE_TITLE, "内存卡", "Storage")                                                              \
    X(SETTINGS_STORAGE_PC_BUSY, "内存卡：电脑占用中", "SD: in use by PC")                                        \
    X(SETTINGS_STORAGE_INSERTED, "内存卡：已插入", "SD: inserted")                                               \
    X(SETTINGS_STORAGE_MISSING, "内存卡：未检测到", "SD: not detected")                                          \
    X(SETTINGS_STORAGE_CAP_DASH, "容量：—", "Capacity: —")                                                       \
    X(SETTINGS_STORAGE_CAP_EXPORT, "容量：导出中，停用后可查看", "Capacity: exporting; disable to view")         \
    X(SETTINGS_STORAGE_CAP_FMT, "容量：%.1fGB/%.1fGB", "Capacity: %.1fGB/%.1fGB")                                 \
    X(SETTINGS_STORAGE_CAP_FAIL, "容量：读取失败", "Capacity: read failed")                                      \
    X(SETTINGS_STORAGE_USB_OFF, "停用模拟 U 盘", "Disable USB disk")                                             \
    X(SETTINGS_STORAGE_USB_ON, "启用模拟 U 盘", "Enable USB disk")                                               \
    X(SETTINGS_STORAGE_USB_DISABLED, "当前固件未启用模拟 U 盘", "USB disk not enabled in this firmware")         \
    X(SETTINGS_STORAGE_USB_HINT_IDLE, "启用后电脑可以将本机识别为U盘", "After enabling, a PC can see this device as a USB drive") \
    X(SETTINGS_STORAGE_USB_HINT_SWITCHING, "正在切换，请稍候…", "Switching, please wait…")                       \
    X(SETTINGS_STORAGE_USB_HINT_ENABLING, "正在启用模拟 U 盘…", "Enabling USB disk…")                            \
    X(SETTINGS_STORAGE_USB_HINT_DISABLING, "正在停用模拟 U 盘…", "Disabling USB disk…")                          \
    X(SETTINGS_STORAGE_USB_HINT_HOST, "已启用，请在电脑访问，\n停用前请先弹出", "Enabled, open it on the PC\nEject before turning it off") \
    X(SETTINGS_STORAGE_USB_HINT_LOCAL, "已启用，主机未占用；点停用恢复 USB 调试", "Enabled — host idle; disable to restore USB debug") \
    X(SETTINGS_STORAGE_USB_HINT_ENABLE_FAIL, "启用失败，请检查 USB 线并重试", "Enable failed — check USB cable and retry") \
    X(SETTINGS_STORAGE_USB_HINT_DISABLE_FAIL, "停用失败，请先在电脑上弹出 U 盘", "Disable failed — eject the disk on PC first") \
    X(SETTINGS_STORAGE_USB_HINT_NO_SD, "未检测到内存卡", "No SD card detected")                                  \
    X(SETTINGS_STORAGE_USB_HINT_FORMAT, "内存卡需要格式化 (FAT32)", "SD card needs formatting (FAT32)")           \
    X(SETTINGS_STORAGE_USB_HINT_HOST_BUSY, "操作失败，请先在电脑上弹出 U 盘", "Failed — eject the disk on PC first") \
    X(BT_TITLE, "蓝牙", "Bluetooth")                                                                           \
    X(BT_RESET_HINT, "点击以复位蓝牙", "Tap to reset Bluetooth")                                              \
    X(BT_VERSION_FMT, "蓝牙版本号：%s", "Bluetooth version: %s")                                              \
    X(BT_CHECK_VERSION, "检测版本", "Check version") \
    X(BT_CHECKING_VERSION, "检测中", "Checking") \
    X(BT_MODE_TITLE, "模式切换", "Mode selection")                                                           \
    X(BT_RESET_BTN, "复位蓝牙", "Reset BT")                                                                    \
    X(BT_MODE1, "模式1", "Mode 1")                                                                             \
    X(BT_MODE2, "模式2", "Mode 2")                                                                             \
    X(BT_MODE3, "模式3", "Mode 3")                                                                             \
    X(BT_SELECT_MODE, "请选择蓝牙模式", "Select a Bluetooth mode")                                              \
    X(BT_MODE1_ACTIVE, "模式1 已激活\n(AT+RX=2 / AT+MODE=1)", "Mode 1 active\n(AT+RX=2 / AT+MODE=1)")             \
    X(BT_SCAN_BTN, "扫描设备", "Scan")                                                                         \
    X(BT_MUSIC_BTN, "音乐模式", "Music")                                                                       \
    X(BT_CALL_BTN, "通话模式", "Call")                                                                         \
    X(BT_CONNECTING_FMT, "连接中: %s...", "Connecting: %s...")                                                  \
    X(BT_MODE1_SET, "模式1 已设置", "Mode 1 set")                                                               \
    X(BT_MODE2_SET, "模式2 已设置，可扫描设备", "Mode 2 set, ready to scan")                                     \
    X(BT_MODE3_SET, "模式3 已设置", "Mode 3 set")                                                               \
    X(BT_SCANNING, "正在扫描...", "Scanning...")                                                               \
    X(BT_FOUND_FMT, "发现设备: %s", "Found: %s")                                                                \
    X(BT_SCAN_DONE_FMT, "扫描完成，共 %d 个设备", "Scan done, %d device(s)")                                     \
    X(BT_CONNECTING, "正在连接...", "Connecting...")                                                           \
    X(BT_CONNECT_OK, "连接成功", "Connected")                                                                  \
    X(BT_CONNECT_TIMEOUT, "连接失败 (超时)", "Connect failed (timeout)")                                        \
    X(BT_CALL_MODE_SCO, "通话模式 (SCO 已建立)", "Call mode (SCO up)")                                          \
    X(BT_MUSIC_MODE_SCO, "音乐模式 (SCO 已断开)", "Music mode (SCO down)")                                      \
    X(BT_SWITCH_MODE1, "切换模式1...", "Switching to mode 1...")                                               \
    X(BT_SWITCH_MODE2, "切换模式2...", "Switching to mode 2...")                                               \
    X(BT_SWITCH_MODE3, "切换模式3...", "Switching to mode 3...")                                               \
    X(BT_UART_NOT_INIT, "UART 未初始化", "UART not initialized")                                               \
    X(BT_SWITCH_CALL, "切换通话模式...", "Switching to call mode...")                                          \
    X(BT_SWITCH_MUSIC, "切换音乐模式...", "Switching to music mode...")                                        \
    X(BT_PWR_RESET_OK, "蓝牙电源已复位", "BT power reset")                                                      \
    X(BT_PWR_RESETTING, "正在复位，请稍候...", "Resetting, please wait...")                                     \
    X(BT_PWR_RESET_RECONNECT, "已复位，正在重新连接...", "Reset complete, reconnecting...")                    \
    X(BT_PWR_RESET_FAILED, "复位失败，请重试", "Reset failed, please retry")                                   \
    X(BT_NEED_MODE2, "请先切换到模式2", "Switch to mode 2 first")                                               \
    X(BT_SCAN_START, "开始扫描...", "Starting scan...")                                                        \
    X(BT_NEED_CONNECT, "请先连接蓝牙设备", "Connect a Bluetooth device first")                                  \
    X(SETTINGS_FRONTLIGHT_TITLE, "前光", "Front light")                                                        \
    X(SETTINGS_FRONTLIGHT_HINT, "色温与亮度 0–100", "CCT and brightness 0–100")                                \
    X(SETTINGS_FRONTLIGHT_COOL, "冷光", "Cool")                                                                \
    X(SETTINGS_FRONTLIGHT_WARM, "暖光", "Warm")                                                                \
    X(SETTINGS_FRONTLIGHT_BOTH, "冷暖光", "Cool + warm")                                                       \
    X(SETTINGS_FRONTLIGHT_OFF, "关闭", "Off")                                                                  \
    X(SETTINGS_FRONTLIGHT_CURRENT_COOL, "当前：冷光", "Current: Cool")                                         \
    X(SETTINGS_FRONTLIGHT_CURRENT_WARM, "当前：暖光", "Current: Warm")                                         \
    X(SETTINGS_FRONTLIGHT_CURRENT_BOTH, "当前：冷暖光", "Current: Cool + warm")                                \
    X(SETTINGS_FRONTLIGHT_CURRENT_OFF, "当前：关闭", "Current: Off")                                           \
    X(SETTINGS_FRONTLIGHT_BRIGHTNESS_FMT, "亮度：%d", "Brightness: %d")                                        \
    X(SETTINGS_FRONTLIGHT_BRIGHTNESS_DEC, "-", "-")                                                            \
    X(SETTINGS_FRONTLIGHT_BRIGHTNESS_INC, "+", "+")                                                            \
    X(SETTINGS_ABOUT_MODEL, "设备型号", "Device model")                                                        \
    X(SETTINGS_ABOUT_CHIP, "芯片型号", "Chip")                                                                 \
    X(SETTINGS_ABOUT_CORES, "CPU 核心", "CPU cores")                                                           \
    X(SETTINGS_ABOUT_CORES_FMT, "%u 核", "%u cores")                                                           \
    X(OTA_CHECK_FAILED, "检测失败，请检查网络后重试", "Check failed. Check network and retry.") \
    X(OTA_ALREADY_LATEST, "当前已是最新版本", "Firmware is up to date") \
    X(SETTINGS_ABOUT_FW, "点击检测更新", "Check for updates")                                                               \
    X(SETTINGS_ABOUT_FW_VER_FMT, "固件版本 %s", "Firmware %s")                                                 \
    X(SETTINGS_ABOUT_BUILD, "编译时间", "Build time")                                                          \
    X(SETTINGS_ABOUT_MAC, "MAC 地址", "MAC")                                                                   \
    X(SETTINGS_ABOUT_FLASH, "Flash 容量", "Flash")                                                             \
    X(SETTINGS_ABOUT_PSRAM, "PSRAM 总大小", "PSRAM")                                                           \
    X(SETTINGS_ABOUT_NONE, "无", "None")                                                                       \
    X(SETTINGS_TEST_AUTO, "自动测试", "Auto test")                                                             \
    X(SETTINGS_TEST_TOUCH, "触摸测试", "Touch test")                                                           \
    X(SETTINGS_TEST_BATTERY, "电池测试", "Battery test")                                                       \
    X(SETTINGS_TEST_AGING, "老化测试", "Aging test")                                                           \
    X(SETTINGS_TEST_SIGNAL_SKIP, "网络信号：本轮未接 WiFi/4G", "Signal: WiFi/4G not wired this round")          \
    X(SETTINGS_TEST_OK, "正常", "OK")                                                                          \
    X(SETTINGS_TEST_FAIL, "失败", "Failed")                                                                    \
    X(SETTINGS_TEST_SKIP, "跳过", "Skip")                                                                      \
    X(SETTINGS_TEST_NOT_DETECTED, "未检测到", "Not found")                                                     \
    X(SETTINGS_TEST_READ_FAIL, "读取失败", "Read failed")                                                      \
    X(SETTINGS_TEST_SDCARD, "SD卡", "SD card")                                                                 \
    X(SETTINGS_TEST_SD_MOUNT_FAIL, "未插卡/挂载失败", "No card / mount fail")                                  \
    X(SETTINGS_TEST_AUDIO_TITLE, "音频", "Audio")                                                              \
    X(SETTINGS_TEST_AUDIO_HOLD, "按住说话", "Hold to talk")                                                    \
    X(SETTINGS_TEST_AUDIO_RECORDING, "录音中…", "Recording…")                                                  \
    X(SETTINGS_TEST_AUDIO_CONFIRM, "录音、喇叭是否正常？", "Mic and speaker OK?")                               \
    X(SETTINGS_TEST_YES, "是", "Yes")                                                                          \
    X(SETTINGS_TEST_NO, "否", "No")                                                                            \
    X(RECORD_PLAYING, "播放中…", "Playing…")                                                                   \
    X(SETTINGS_TEST_CAMERA, "摄像头", "Camera")                                                                \
    X(SETTINGS_TEST_BACK, "返回", "Back")                                                                      \
    X(SETTINGS_TEST_TOUCH_START, "点击屏幕，开始测试", "Tap screen to start")                                  \
    X(SETTINGS_TEST_BATTERY_HINT, "SOC: 3.30~4.30V; ≥4.30需稳30s→100%",                                         \
      "SOC: 3.30~4.30V; ≥4.30 hold 30s→100%")                                                                   \
    X(SETTINGS_TEST_BATTERY_GAUGE, "电量计", "Fuel gauge")                                                     \
    X(SETTINGS_TEST_BATTERY_SOC_SMOOTH, "电量(平滑)", "SOC (smooth)")                                          \
    X(SETTINGS_TEST_BATTERY_SOC_RAW, "电量(瞬时)", "SOC (instant)")                                            \
    X(SETTINGS_TEST_BATTERY_VOLT, "电压", "Voltage")                                                           \
    X(SETTINGS_TEST_BATTERY_VOLT_RANGE, "电压波动", "Voltage swing")                                           \
    X(SETTINGS_TEST_BATTERY_CURR, "电流", "Current")                                                           \
    X(SETTINGS_TEST_BATTERY_GAUGE_STAT, "表计状态", "Gauge state")                                             \
    X(SETTINGS_TEST_BATTERY_CHARGING, "充电中", "Charging")                                                    \
    X(SETTINGS_TEST_BATTERY_DISCHARGING, "放电中", "Discharging")                                              \
    X(SETTINGS_TEST_BATTERY_IDLE_LOAD, "空载", "Idle")                                                         \
    X(SETTINGS_TEST_BATTERY_VBUS, "VBUS", "VBUS")                                                              \
    X(SETTINGS_TEST_BATTERY_CHIP_CHG, "芯片充电", "Chip charge")                                               \
    X(SETTINGS_TEST_BATTERY_CHG_EN, "充电使能", "Charge enable")                                               \
    X(SETTINGS_TEST_BATTERY_ICHG, "设定电流", "Charge current")                                                \
    X(SETTINGS_TEST_BATTERY_VREG, "截止电压", "Charge voltage")                                                \
    X(SETTINGS_TEST_BATTERY_MATCH, "一致性", "Consistency")                                                    \
    X(SETTINGS_TEST_BATTERY_CHG_NOT, "未充电/已满", "Not charging / full")                                     \
    X(SETTINGS_TEST_BATTERY_CHG_CC, "涓流/预充/CC", "Trickle/pre/CC")                                          \
    X(SETTINGS_TEST_BATTERY_CHG_CV, "恒压降流", "CV taper")                                                    \
    X(SETTINGS_TEST_BATTERY_CHG_TOPOFF, "Top-off", "Top-off")                                                  \
    X(SETTINGS_TEST_BATTERY_EN_ON, "开", "On")                                                                 \
    X(SETTINGS_TEST_BATTERY_EN_OFF, "关", "Off")                                                               \
    X(SETTINGS_TEST_BATTERY_VBUS_NONE, "无输入", "No input")                                                   \
    X(SETTINGS_TEST_BATTERY_VBUS_SDP, "USB SDP", "USB SDP")                                                    \
    X(SETTINGS_TEST_BATTERY_VBUS_CDP, "USB CDP", "USB CDP")                                                    \
    X(SETTINGS_TEST_BATTERY_VBUS_DCP, "USB DCP", "USB DCP")                                                    \
    X(SETTINGS_TEST_BATTERY_VBUS_UNK_ADP, "未知适配器", "Unknown adapter")                                     \
    X(SETTINGS_TEST_BATTERY_VBUS_NONSTD, "非标适配器", "Non-std adapter")                                      \
    X(SETTINGS_TEST_BATTERY_VBUS_OTG, "OTG", "OTG")                                                            \
    X(SETTINGS_TEST_BATTERY_VBUS_ADP, "适配器", "Adapter")                                                     \
    X(SETTINGS_TEST_BATTERY_VBUS_UNKNOWN_IN, "有输入(未识别)", "Input (unknown)")                              \
    X(SETTINGS_TEST_BATTERY_MATCH_OK, "一致", "Match")                                                         \
    X(SETTINGS_TEST_BATTERY_MATCH_DPDM, "一致(DPDM关)", "Match (DPDM off)")                                    \
    X(SETTINGS_TEST_BATTERY_MISMATCH_CHIP, "芯片在充/表计否", "Chip chg / gauge no")                           \
    X(SETTINGS_TEST_BATTERY_MISMATCH_GAUGE, "表计在充/芯片否", "Gauge chg / chip no")                          \
    X(SETTINGS_TEST_BATTERY_MISMATCH_NO_CHG, "有输入未在充", "Input, not charging")                            \
    X(SETTINGS_TEST_BATTERY_MISMATCH_NO_IN, "无输入却在充?", "No input but charging?")                         \
    X(SETTINGS_TEST_AGING_RUNNING, "老化测试中（震动循环）", "Aging in progress")                              \
    X(SETTINGS_TEST_AGING_TICK_FMT, "运行 %u s  震=%d", "Running %u s  vibe=%d")                               \
    X(STANDBY, "待命", "Standby")                                                                              \
    X(CONNECTING, "连接中...", "Connecting...")                                                                \
    X(LISTENING, "聆听中...", "Listening...")                                                                  \
    X(SPEAKING, "回答中...", "Speaking...")                                                                    \
    X(PLEASE_WAIT, "请稍候...", "Please wait...")                                                              \
    X(LOADING_PROTOCOL, "登录服务...", "Logging in...")                                                        \
    X(VOICE_STARTING_NET, "正在联网启动，完成后自动开始", "Connecting; will start automatically")           \
    X(SERVER_NOT_CONNECTED, "无法连接服务，请稍后再试", "Unable to connect to service")                      \
    X(WIFI_CONFIG_MODE, "配网模式", "Wi-Fi Setup")                                                             \
    X(NEED_WIFI_CFG, "请先完成配网", "Set up Wi-Fi first")                                                     \
    X(VERSION, "版本 ", "Ver ")                                                                                \
    X(FOUND_NEW_ASSETS, "发现新资源: %s", "Found new assets: %s")                                              \
    X(LOADING_ASSETS, "加载资源...", "Loading assets...")                                                      \
    X(ERROR, "错误", "Error")                                                                                  \
    X(DOWNLOAD_ASSETS_FAILED, "下载资源失败", "Failed to download assets")                                     \
    X(CHECKING_NEW_VERSION, "检查新版本...", "Checking for new version...")                                    \
    X(CHECK_NEW_VERSION_FAILED, "检查新版本失败，将在 %d 秒后重试: %s", "Check version failed, retry in %d s: %s") \
    X(ACTIVATION, "激活设备", "Activation")                                                                    \
    X(ACTIVATION_CODE_FMT, "验证码:%s", "Code:%s")                                                             \
    X(OTA_UPGRADE, "OTA 升级", "OTA Upgrade")                                                                  \
    X(UPGRADING, "正在升级系统...", "System is upgrading...")                                                  \
    X(UPGRADE_FAILED, "升级失败", "Upgrade failed")                                                            \
    X(OTA_SUCCESS_REBOOT, "升级成功，即将重启", "Upgrade OK, rebooting")                                       \
    X(OTA_MANUAL, "手动升级", "Manual upgrade")                                                                \
    X(RTC_MODE_OFF, "AEC 关闭", "AEC Off")                                                                     \
    X(RTC_MODE_ON, "AEC 开启", "AEC On")                                                                       \
    X(SERVER_ERROR, "服务错误", "Server error")                                                                \
    X(SERVER_TIMEOUT, "服务超时", "Server timeout")                                                            \
    X(OTA_TITLE, "系统升级", "System upgrade")                                                                 \
    X(OTA_CUR_VER_FMT, "当前版本: %s", "Current: %s")                                                          \
    X(OTA_NEW_VER_FMT, "新版本: %s", "New: %s")                                                                \
    X(OTA_HINT, "升级过程请勿断电", "Do not power off during upgrade")                                         \
    X(OTA_CONFIRM_FMT, "发现新版本 %s\n是否升级？", "New version %s\nUpgrade?")                                 \
    X(OTA_SD_CONFIRM_FMT, "SD 固件升级\n当前 %s\n是否升级？", "SD firmware\nCurrent %s\nUpgrade?")              \
    X(OTA_UPGRADE_NOW, "立即升级", "Upgrade now")                                                              \
    X(OTA_IGNORE_VERSION, "忽略此版本", "Ignore version")                                                      \
    X(OTA_REMIND_LATER, "稍后提醒", "Remind later")                                                            \
    X(COMMON_CANCEL, "取消", "Cancel")                                                                         \
    X(COMMON_REMOVE, "移除", "Remove")                                                                         \
    X(COMMON_SELECT_ALL, "全选", "Select all")                                                                 \
    X(TASK_API_ERROR, "接口返回错误", "API error")                                                             \
    X(TASK_BATCH_COMPLETING, "批量完成中...", "Batch completing...")                                           \
    X(TASK_BATCH_FAIL, "批量操作失败", "Batch failed")                                                         \
    X(TASK_BATCH_REMOVING, "批量移除中...", "Batch removing...")                                               \
    X(TASK_BATCH_RESULT_FMT, "成功 %d，失败 %d", "%d ok, %d failed")                                            \
    X(TASK_BATCH_START_FAIL, "批量任务启动失败", "Failed to start batch")                                      \
    X(TASK_CHOOSE_ACTION, "选择操作", "Choose action")                                                         \
    X(TASK_COMPLETE, "完成", "Done")                                                                           \
    X(TASK_COMPLETED, "已完成", "Completed")                                                                   \
    X(TASK_COMPLETED_N_FMT, "已完成 %d 项", "Completed %d")                                                    \
    X(TASK_COMPLETE_FAIL, "完成失败", "Complete failed")                                                       \
    X(TASK_COMPLETE_START_FAIL, "完成任务启动失败", "Failed to start complete")                                \
    X(TASK_COMPLETE_TODO, "完成待办", "Complete")                                                              \
    X(TASK_COMPLETING, "完成中...", "Completing...")                                                           \
    X(TASK_CONNECTING_NET, "正在联网…", "Connecting…")                                                         \
    X(TASK_CONN_FAIL, "创建连接失败", "Connection failed")                                                     \
    X(TASK_DATA_FORMAT_ERR, "数据格式错误", "Bad data format")                                                 \
    X(TASK_DELETED, "已删除", "Deleted")                                                                       \
    X(TASK_DELETE_FAIL, "删除失败", "Delete failed")                                                           \
    X(TASK_DELETE_START_FAIL, "删除任务启动失败", "Failed to start delete")                                    \
    X(TASK_DELETE_TODO, "删除待办", "Delete")                                                                  \
    X(TASK_DELETING, "删除中...", "Deleting...")                                                               \
    X(TASK_DONE_OK, "任务已完成", "Task completed")                                                            \
    X(TASK_EMPTY_DONE, "暂无已完成任务", "No completed tasks")                                                 \
    X(TASK_EMPTY_TODO, "暂无待办", "No todos")                                                                 \
    X(TASK_INVALID, "无效待办", "Invalid todo")                                                                \
    X(TASK_LOAD_FAIL, "加载失败", "Load failed")                                                               \
    X(TASK_NEED_WIFI_CFG, "请先完成配网", "Set up Wi-Fi first")                                                 \
    X(TASK_NET_NOT_READY, "网络未就绪", "Network not ready")                                                   \
    X(TASK_NO_COMPLETABLE, "没有可完成的待办", "Nothing to complete")                                          \
    X(TASK_NO_NETWORK, "无网络", "No network")                                                                 \
    X(TASK_PARSE_FAIL, "响应解析失败", "Response parse failed")                                                \
    X(TASK_PLEASE_WAIT, "请稍候...", "Please wait...")                                                         \
    X(TASK_REFRESH, "刷新", "Refresh")                                                                         \
    X(TASK_REFRESHED, "已刷新", "Refreshed")                                                                   \
    X(TASK_REFRESHING, "刷新中...", "Refreshing...")                                                           \
    X(TASK_REFRESH_START_FAIL, "刷新启动失败", "Failed to start refresh")                                      \
    X(TASK_REMOVED_N_FMT, "已移除 %d 项", "Removed %d")                                                        \
    X(TASK_REQUEST_FAIL, "请求失败", "Request failed")                                                         \
    X(TASK_SELECTED_FMT, "已选 %d", "Selected %d")                                                             \
    X(TASK_SELECT_FIRST, "请先选择", "Select items first")                                                     \
    X(TASK_TAB_DONE, "已完成", "Done")                                                                         \
    X(TASK_TAB_TODO, "待办", "Todo")                                                                           \
    X(TASK_TITLE_DONE, "已完成", "Done")                                                                       \
    X(TASK_TITLE_TODO, "待办事项", "Schedule")                                                                 \
    X(CONNECT_TO, "连接 ", "Connect ")                                                                         \
    X(CONNECTED_TO, "已连接 ", "Connected ")                                                                   \
    X(SETTINGS_NET_WIFI_CFG_TITLE, "WiFi 网络", "Wi-Fi network")                                               \
    X(SETTINGS_NET_WIFI_CFG_HINT, "扫描附近 WiFi，选择后输入密码", "Scan nearby Wi-Fi and enter the password") \
    X(SETTINGS_NET_ENTER_CFG, "配置 WIFI 网络", "Configure Wi-Fi")                                             \
    X(SETTINGS_NET_CUR_WIFI, "当前：WiFi 上网", "Current: Wi-Fi")                                               \
    X(SETTINGS_NET_REBOOT_COUNT_FMT, "%s\n%d 秒后重启…", "%s\nRebooting in %d s…")                             \
    X(SETTINGS_NET_REBOOTING, "正在重启…", "Rebooting…")                                                       \
    X(NETWORK_TITLE, "配置 WIFI", "Configure Wi-Fi")                                                           \
    X(NETWORK_TAB_NEARBY, "附近", "Nearby")                                                                    \
    X(NETWORK_TAB_SAVED, "已保存", "Saved")                                                                    \
    X(NETWORK_SCAN, "刷新", "Refresh")                                                                         \
    X(NETWORK_SCANNING, "正在扫描附近 WiFi…", "Scanning nearby Wi-Fi…")                                        \
    X(NETWORK_WIFI_INIT, "正在初始化 WiFi…", "Initializing Wi-Fi…")                                            \
    X(NETWORK_WIFI_INIT_FAIL, "WiFi 初始化失败", "Wi-Fi init failed")                                          \
    X(NETWORK_SCAN_FAIL, "启动扫描失败", "Failed to start scan")                                               \
    X(NETWORK_SCAN_TIMEOUT, "扫描超时", "Scan timed out")                                                      \
    X(NETWORK_SCAN_DONE_FMT, "扫描完成，共 %d 个网络", "Scan done, %d networks")                               \
    X(NETWORK_SCAN_TASK_FAIL, "无法启动扫描任务", "Cannot start scan task")                                    \
    X(NETWORK_BUSY_CONNECT, "正在连接，请稍后再扫描", "Connecting; scan later")                                \
    X(NETWORK_NEARBY_EMPTY, "未发现网络，点「刷新」重试", "No networks. Tap Refresh to retry")                  \
    X(NETWORK_SAVED_EMPTY, "暂无已保存的 WiFi", "No saved Wi-Fi yet")                                           \
    X(NETWORK_AUTH_OPEN, "[开放]", "[Open]")                                                                   \
    X(NETWORK_AUTH_SECURE, "[加密]", "[Secured]")                                                              \
    X(NETWORK_CONNECT_TO_FMT, "连接到：%s", "Connect to: %s")                                                  \
    X(NETWORK_PWD_HINT, "请输入 WiFi 密码（8~63 字符）", "Enter Wi-Fi password (8–63 chars)")                   \
    X(NETWORK_PWD_PLACEHOLDER, "WiFi 密码", "Wi-Fi password")                                                  \
    X(NETWORK_SHOW_PWD, "显示密码", "Show password")                                                           \
    X(NETWORK_CANCEL, "取消", "Cancel")                                                                        \
    X(NETWORK_CONNECT, "连接", "Connect")                                                                      \
    X(NETWORK_CONNECTING_FMT, "正在连接 %s …", "Connecting to %s …")                                           \
    X(NETWORK_CONNECTED_FMT, "已连接 %s", "Connected to %s")                                                   \
    X(NETWORK_CONNECT_FAIL, "连接失败", "Connection failed")                                                   \
    X(NETWORK_CONNECT_FAIL_REFRESH, "请重新刷新 WiFi 列表", "Please refresh the Wi-Fi list")                   \
    X(NETWORK_CONNECT_TIMEOUT, "连接超时", "Connection timed out")                                             \
    X(NETWORK_CONNECT_TIMEOUT_HINT, "未能在 15 秒内完成连接，请重试", "Could not connect within 15s; retry")   \
    X(NETWORK_CONNECT_TASK_FAIL, "无法启动连接任务", "Cannot start connect task")                              \
    X(NETWORK_SSID_INVALID, "SSID 不合法", "Invalid SSID")                                                     \
    X(NETWORK_PWD_TOO_LONG, "密码超长", "Password too long")                                                   \
    X(NETWORK_ERR_BAD_PASSWORD, "密码错误，请重新输入", "Wrong password; try again")                           \
    X(NETWORK_ERR_AP_GONE, "未找到该 WiFi（信号丢失）", "Wi-Fi not found (signal lost)")                       \
    X(NETWORK_ERR_ASSOC, "关联失败，路由器拒绝连接", "Association failed; AP rejected")                        \
    X(NETWORK_ERR_WEAK, "信号太弱，连接超时", "Signal too weak; timed out")                                    \
    X(NETWORK_ERR_REASON_FMT, "连接被拒绝 (reason=%u)", "Rejected (reason=%u)")                                \
    X(NETWORK_DEFAULT_FMT, "%s（默认）", "%s (default)")                                                       \
    X(NETWORK_SET_DEFAULT, "置顶", "Default")                                                                  \
    X(NETWORK_DELETE, "删除", "Delete")                                                                        \
    X(NETWORK_CLEAR_ALL, "清空", "Clear")                                                                      \
    X(NETWORK_SET_DEFAULT_OK, "已设为默认网络", "Set as default network")                                      \
    X(NETWORK_DELETED_OK, "已删除该网络", "Network deleted")                                                   \
    X(NETWORK_CLEARED_OK, "已清空已保存网络", "Cleared saved networks")                                        \
    X(SCANNING, "扫描中...", "Scanning...")                                                                    \
    X(SCANNING_WIFI, "扫描 WiFi...", "Scanning Wi-Fi...")                                                      \
    X(WIFI_STATUS_SCANNING, "WiFi扫描中", "WiFi scanning")                                                     \
    X(WIFI_STATUS_CONNECTING, "WiFi连接中", "WiFi connecting")                                                 \
    X(VOLUME, "音量 ", "Volume ")                                                                              \
    X(MAX_VOLUME, "最大音量", "Max volume")                                                                    \
    X(MUTED, "已静音", "Muted")                                                                    \
    X(BOOK_ANTIALIAS, "抗锯齿", "Anti-aliasing") \
    X(BOOK_BAD_ARG, "参数错误", "Invalid argument") \
    X(BOOK_BLANK, "（空白）", "(Blank)") \
    X(BOOK_BODY, "正文", "Body") \
    X(BOOK_CANCELLED, "已取消", "Cancelled") \
    X(BOOK_CHAPTER_FMT, "第%d章", "Ch.%d") \
    X(BOOK_CHAPTER_NO_TEXT, "（本章无文本）", "(No text in chapter)") \
    X(BOOK_CHAPTER_PROGRESS, "底栏信息", "Footer info") \
    X(BOOK_CHECKSUM_FAIL, "校验失败", "Checksum failed") \
    X(BOOK_CONN_FAIL, "创建连接失败", "Connection failed") \
    X(BOOK_CONTINUE, "继续阅读", "Continue") \
    X(BOOK_COUNT_FMT, "%d本", "%d") \
    X(BOOK_COVER_TOO_LARGE, "封面过大", "Cover too large") \
    X(BOOK_DELETE_FAILED, "删除失败", "Delete failed") \
    X(BOOK_DETAIL, "详情", "Details") \
    X(BOOK_DOWNLOAD_FAIL, "下载失败", "Download failed") \
    X(BOOK_EMPTY_COVER, "空封面", "Empty cover") \
    X(BOOK_EMPTY_FILE, "空文件", "Empty file") \
    X(BOOK_EMPTY_HOME_FMT, "暂无书籍\n请将 .epub / .txt / .ebook\n放到 %s", "No books\nPut .epub / .txt / .ebook\nin %s") \
    X(BOOK_EMPTY_SHELF_FMT, "书架为空\n请将 .epub / .txt / .ebook\n放到 %s", "Bookshelf empty\nPut .epub / .txt / .ebook\nin %s") \
    X(BOOK_FILE_TOO_LARGE, "文件过大", "File too large") \
    X(BOOK_FINISHED, "已看完", "Finished") \
    X(BOOK_FONT_EMPTY, "无可用字体", "No fonts") \
    X(BOOK_FONT_NAME_INVALID, "文件名无效", "Invalid filename") \
    X(BOOK_FONT_TITLE, "字体", "Font") \
    X(BOOK_FOOTER_BATTERY, "电量", "Battery") \
    X(BOOK_FOOTER_PROGRESS, "进度", "Progress") \
    X(BOOK_FOOTER_TIME, "时间", "Time") \
    X(BOOK_FOOTER_TITLE, "章节名", "Chapter") \
    X(BOOK_IMAGE_PLACEHOLDER, "[图片]", "[Image]") \
    X(BOOK_LAYOUT_BUSY, "编排中", "Building") \
    X(BOOK_LAYOUT_DONE, "完成", "Done") \
    X(BOOK_LINE_GAP, "行距", "Line spacing") \
    X(BOOK_MARGIN, "边距", "Margin") \
    X(BOOK_MARGIN_NARROW, "窄", "Narrow") \
    X(BOOK_MARGIN_STANDARD, "标准", "Standard") \
    X(BOOK_MARGIN_VERY_WIDE, "很宽", "Very wide") \
    X(BOOK_MARGIN_WIDE, "宽", "Wide") \
    X(BOOK_NET_NOT_READY, "网络未就绪", "Network not ready") \
    X(BOOK_NO_CONTINUE, "暂无可续", "Nothing to continue") \
    X(BOOK_NO_COVER_URL, "无封面地址", "No cover URL") \
    X(BOOK_NO_DOWNLOAD_URL, "缺少下载地址", "Missing download URL") \
    X(BOOK_NO_LOCAL_FILE, "无本地文件", "No local file") \
    X(BOOK_NO_NETWORK, "无网络", "No network") \
    X(BOOK_NO_RECENT, "暂无最近阅读", "No recent books") \
    X(BOOK_NO_SD, "未检测到 SD 卡\n请插入后重试", "No SD card\nInsert and retry") \
    X(BOOK_NO_SD_SHORT, "未检测到 SD 卡", "No SD card") \
    X(BOOK_OPEN_FAILED, "打开失败", "Open failed") \
    X(BOOK_OPEN_FAILED_IO, "打开失败\n文件不存在或 SD 读取失败", "Open failed\nFile missing or SD read failed") \
    X(BOOK_OPEN_FAILED_DATA, "打开失败\n书籍损坏或正文格式无效", "Open failed\nDamaged book or invalid content") \
    X(BOOK_OPEN_FAILED_MEMORY, "打开失败\n可用内存不足", "Open failed\nNot enough memory") \
    X(BOOK_OPEN_FAILED_CACHE, "打开失败\n无法创建或读取阅读缓存，请检查存储卡和剩余空间", "Open failed\nCannot create or read the cache. Check the SD card and free space") \
    X(BOOK_OPEN_FAILED_UNSUPPORTED, "打开失败\n暂不支持此书籍格式或文件参数", "Open failed\nUnsupported book format or file parameters") \
    X(BOOK_OPEN_CANCELLED, "已取消打开", "Opening cancelled") \
    X(BOOK_OPEN_FAILED_TASK, "打开失败\n无法启动解析任务", "Open failed\nCannot start parse task") \
    X(BOOK_OPENING, "正在打开", "Opening") \
    X(BOOK_LOADING, "加载中...", "Loading...") \
    X(BOOK_ORIENT, "屏幕方向", "Orientation") \
    X(BOOK_ORIENT_LAND_LEFT, "左横", "Left") \
    X(BOOK_ORIENT_LAND_RIGHT, "右横", "Right") \
    X(BOOK_ORIENT_PORTRAIT, "竖屏", "Port.") \
    X(BOOK_OUT_OF_MEMORY, "内存不足", "Out of memory") \
    X(BOOK_PAGE_LOAD_FAILED, "无法加载页面", "Cannot load page") \
    X(BOOK_PATH_INVALID, "路径无效", "Invalid path") \
    X(BOOK_READ_DURATION, "阅读时长", "Total time") \
    X(BOOK_READ_FAIL, "读取失败", "Read failed") \
    X(BOOK_READ_FILE_FAIL, "（读文件失败）", "(Read failed)") \
    X(BOOK_READ_PCT_FMT, "已阅读%d.%d%%", "Read %d.%d%%") \
    X(BOOK_RECENT, "最近阅读", "Recent") \
    X(BOOK_SAVE_FAIL, "保存失败", "Save failed") \
    X(BOOK_SELECTED_FMT, "已选 %d", "Selected %d") \
    X(BOOK_SHELF_TITLE, "我的书架", "My bookshelf") \
    X(BOOK_SPACING_COMPACT, "紧凑", "Compact") \
    X(BOOK_SPACING_RELAXED, "宽松", "Relaxed") \
    X(BOOK_SPACING_STANDARD, "标准", "Standard") \
    X(BOOK_SPACING_VERY_RELAXED, "很宽松", "Very relaxed") \
    X(BOOK_START, "开始阅读", "Start reading") \
    X(BOOK_STORAGE_UNAVAIL, "存储目录不可用", "Storage unavailable") \
    X(BOOK_SYNC_EMPTY_BODY, "空请求体", "Empty request body") \
    X(BOOK_TAP_ZONE_BACK, "返回", "Back") \
    X(BOOK_TAP_ZONE_FULL_REFRESH, "全刷一次", "Full refresh") \
    X(BOOK_TAP_ZONE_MENU, "菜单", "Menu") \
    X(BOOK_TAP_ZONE_NEXT, "下一页", "Next") \
    X(BOOK_TAP_ZONE_NONE, "无", "None") \
    X(BOOK_TAP_ZONE_PREV, "上一页", "Prev") \
    X(BOOK_TAP_ZONES, "触摸分区", "Tap zones") \
    X(BOOK_TAP_ZONES_PICK, "选择动作", "Choose action") \
    X(BOOK_TAP_ZONES_RESET, "恢复默认", "Reset") \
    X(BOOK_TAP_ZONES_TITLE, "触摸分区", "Tap zones") \
    X(BOOK_TOC, "目录", "Contents") \
    X(BOOK_TOC_BUILDING, "目录识别中…", "Finding contents...") \
    X(BOOK_TOC_EMPTY, "暂无目录", "No contents") \
    X(BOOK_TOC_PAGE_FMT, "目录 %d/%d", "Contents %d/%d") \
    X(BOOK_TODAY_DURATION, "今日时长", "Today") \
    X(BOOK_UNDERLINE, "下划线", "Underline") \
    X(BOOK_UNDERLINE_DASHED, "虚线", "Dashed") \
    X(BOOK_UNDERLINE_SOLID, "实线", "Solid") \
    X(BOOK_WRITE_FAIL, "写文件失败", "Write failed") \
    X(CLOUD_API_ERROR, "接口返回错误", "API error") \
    X(CLOUD_CANCELLED, "已取消", "Cancelled") \
    X(CLOUD_CANCELLING, "正在取消…", "Cancelling…") \
    X(CLOUD_CONN_FAIL, "创建连接失败", "Connection failed") \
    X(CLOUD_CONNECT_NET, "连接网络…", "Connecting…") \
    X(CLOUD_CONNECTING_NET, "正在联网…", "Connecting…") \
    X(CLOUD_COVER_FAIL, "封面加载失败", "Cover load failed") \
    X(CLOUD_DATA_FORMAT_ERR, "数据格式错误", "Bad data format") \
    X(CLOUD_DECODE_FAIL, "解码失败", "Decode failed") \
    X(CLOUD_DELETE_FAIL, "删除失败", "Delete failed") \
    X(CLOUD_DOWNLOAD_FAIL, "下载失败", "Download failed") \
    X(CLOUD_DOWNLOADING, "正在下载", "Downloading") \
    X(CLOUD_EMPTY_ALL, "暂无待传输资源", "Nothing to transfer") \
    X(CLOUD_EMPTY_BOOK, "暂无书籍", "No books") \
    X(CLOUD_EMPTY_FONT, "暂无字体", "No fonts") \
    X(CLOUD_EMPTY_WALLPAPER, "暂无壁纸", "No Walls") \
    X(CLOUD_FETCHING_LIST, "正在拉取列表…", "Fetching list…") \
    X(CLOUD_FILE_INVALID, "文件无效", "Invalid file") \
    X(CLOUD_IN_QUEUE, "已在队列中", "Already queued") \
    X(CLOUD_JSON_CREATE_FAIL, "JSON 创建失败", "JSON create failed") \
    X(CLOUD_JSON_PARSE_FAIL, "JSON 解析失败", "JSON parse failed") \
    X(CLOUD_JSON_SERIALIZE_FAIL, "JSON 序列化失败", "JSON serialize failed") \
    X(CLOUD_LIST_TOO_LARGE, "列表过大", "List too large") \
    X(CLOUD_LOAD_COVER, "加载封面…", "Loading covers…") \
    X(CLOUD_LOAD_FAIL, "加载失败", "Load failed") \
    X(CLOUD_LOADING, "加载中…", "Loading…") \
    X(CLOUD_MISSING_TASK_ID, "缺少 taskId", "Missing taskId") \
    X(CLOUD_NEED_WIFI_CFG, "请先完成配网", "Set up Wi-Fi first") \
    X(CLOUD_NET_NOT_READY, "网络未就绪", "Network not ready") \
    X(CLOUD_NO_COVER, "暂无封面", "No cover") \
    X(CLOUD_NO_DOWNLOAD_URL, "缺少下载地址", "Missing download URL") \
    X(CLOUD_NO_LOCAL_FILE, "无本地文件", "No local file") \
    X(CLOUD_NO_NETWORK, "无网络", "No network") \
    X(CLOUD_NO_SD, "未检测到 SD 卡", "No SD card") \
    X(CLOUD_PATH_INVALID, "路径无效", "Invalid path") \
    X(CLOUD_PENDING, "待接收", "Incoming") \
    X(CLOUD_PLEASE_WAIT, "请稍候", "Please wait") \
    X(CLOUD_PREVIEW_FAIL, "无法预览", "Cannot preview") \
    X(CLOUD_PREVIEW_START_FAIL, "预览启动失败", "Failed to start preview") \
    X(CLOUD_PREVIEW_URL_LONG, "预览地址过长", "Preview URL too long") \
    X(CLOUD_READ_TIMEOUT, "读取超时或中断", "Read timeout or interrupted") \
    X(CLOUD_REFRESH, "刷新", "Refresh") \
    X(CLOUD_REFRESHED, "已刷新", "Refreshed") \
    X(CLOUD_REFRESHING, "刷新中…", "Refreshing…") \
    X(CLOUD_REQUEST_FAIL, "请求失败", "Request failed") \
    X(CLOUD_FETCH_FAIL, "获取推送内容失败，请检查网络后重试", "Failed to fetch incoming content. Check your network and try again.") \
    X(CLOUD_SAVE, "保存", "Save") \
    X(CLOUD_FILE_EXISTS, "此位置已有同名文件", "A file with this name already exists") \
    X(CLOUD_CONFLICT_FMT, "此位置已有同名文件\n%s\n请选择保存方式", "File already exists\n%s\nChoose how to save") \
    X(CLOUD_KEEP_BOTH, "保留两份", "Keep both") \
    X(CLOUD_SKIP_FILE, "跳过此文件", "Skip file") \
    X(CLOUD_REPLACE_FILE, "覆盖原文件", "Replace file") \
    X(CLOUD_SAVE_FAIL, "保存失败", "Save failed") \
    X(CLOUD_SAVE_LOCAL, "保存到本地", "Save to device") \
    X(CLOUD_SAVED_0_1, "已保存 0 / 共 1", "Saved 0 / 1") \
    X(CLOUD_SAVED_FMT, "已保存 %d / 共 %d", "Saved %d / %d") \
    X(CLOUD_SAVED_SYNC_FAIL, "已保存(同步失败)", "Saved (sync failed)") \
    X(CLOUD_SAVING, "正在保存", "Saving") \
    X(CLOUD_SAVING_NAME_FMT, "正在保存\n%s", "Saving\n%s") \
    X(CLOUD_SELECT_FIRST, "请先选择", "Select items first") \
    X(CLOUD_SELECTED_FMT, "已选 %d", "Selected %d") \
    X(CLOUD_STORAGE_UNAVAIL, "存储目录不可用", "Storage unavailable") \
    X(CLOUD_SYNC_FAIL, "同步失败", "Sync failed") \
    X(CLOUD_TAB_ALL, "全部", "All") \
    X(CLOUD_TAB_APP, "APP推送", "APP Push") \
    X(CLOUD_TAB_AP, "AP传输", "Hotspot") \
    X(CLOUD_TAB_USB, "USB传输", "USB") \
    X(CLOUD_AP_STEP1, "第一步", "Step 1") \
    X(CLOUD_AP_JOIN, "用手机或电脑连接到热点或扫码连接", "Join the hotspot, or scan the code") \
    X(CLOUD_AP_SSID_FMT, "WiFi名称：%s", "Wi-Fi: %s") \
    X(CLOUD_AP_STEP2, "第二步", "Step 2") \
    X(CLOUD_AP_OPEN, "用手机或电脑浏览器访问这个地址：", "Open this address in a browser:") \
    X(CLOUD_AP_HINT, "也可以扫描下方二维码打开地址", "Or scan the code below") \
    X(CLOUD_AP_OPENING, "AP打开中...", "Starting hotspot...") \
    X(CLOUD_AP_FAIL, "热点启动失败", "Hotspot failed to start") \
    X(CLOUD_AP_FAIL_INIT, "WiFi初始化失败", "Wi-Fi init failed") \
    X(CLOUD_AP_FAIL_START, "热点打开失败", "Hotspot failed to start") \
    X(CLOUD_AP_FAIL_HTTP, "网页服务启动失败", "Web page failed to start") \
    X(CLOUD_WIFI_FAIL, "WiFi连接失败", "Wi-Fi connection failed") \
    X(CLOUD_CELLULAR_NEED_WIFI, "当前为4G联网，请切换到Wi-Fi后重试", "Using 4G. Switch to Wi-Fi and try again.") \
    X(CLOUD_USB_WAY, "方式一", "Method") \
    X(CLOUD_TAB_BOOK, "书籍", "Books") \
    X(CLOUD_TAB_FONT, "字体", "Fonts") \
    X(CLOUD_TAB_WALLPAPER, "壁纸", "Walls") \
    X(CLOUD_TASK_FAIL, "任务失败", "Task failed") \
    X(CLOUD_TYPE_BOOK, "书籍", "Book") \
    X(CLOUD_TYPE_FONT, "字体", "Font") \
    X(CLOUD_TYPE_WALLPAPER, "壁纸", "Walls") \
    X(CLOUD_UNKNOWN_TYPE, "未知类型", "Unknown type") \
    X(CLOUD_WAIT_DOWNLOAD, "请等待下载任务结束", "Wait for download to finish") \
    X(COMMON_DELETE, "删除", "Delete") \
    X(COMMON_EMPTY, "暂无内容", "Empty") \
    X(COMMON_OFF, "关闭", "Off") \
    X(COMMON_ON, "开启", "On") \
    X(STANDBY_CONN_FAIL, "创建连接失败", "Connection failed") \
    X(STANDBY_DATE_FMT, "%02d月%02d日", "%02d/%02d") \
    X(STANDBY_DATE_PLACEHOLDER, "--月--日", "--/--") \
    X(STANDBY_DECODE_FAIL, "解码失败", "Decode failed") \
    X(STANDBY_EMPTY_TODO, "暂无待办", "No todos") \
    X(STANDBY_NO_DATA, "无数据", "No data") \
    X(STANDBY_NO_NETWORK, "无网络", "No network") \
    X(STANDBY_NO_TODO_CACHE, "暂无清单缓存", "No task cache") \
    X(STANDBY_NO_WEATHER, "无天气", "No weather") \
    X(STANDBY_PARSE_FAIL, "解析失败", "Parse failed") \
    X(STANDBY_REQUEST_FAIL, "请求失败", "Request failed") \
    X(STANDBY_WEATHER_FAIL, "天气失败", "Weather failed") \
    X(STANDBY_WP_NOT_FOUND, "未找到待机壁纸", "Standby wallpaper not found") \
    X(WALLPAPER_ADJUST, "调整", "Adjust") \
    X(WALLPAPER_CHIP_SHUTDOWN, "关机", "Power-off") \
    X(WALLPAPER_CHIP_STANDBY, "待机", "Standby") \
    X(WALLPAPER_DECODE_FAIL, "解码失败", "Decode failed") \
    X(WALLPAPER_DELETE_BTN, "删除壁纸", "Delete wallpaper") \
    X(WALLPAPER_DELETE_FAIL, "删除失败", "Delete failed") \
    X(WALLPAPER_DELETE_START_FAIL, "删除启动失败", "Failed to start delete") \
    X(WALLPAPER_DELETING, "正在删除…", "Deleting…") \
    X(WALLPAPER_EMPTY_FMT, "暂无壁纸\n请放到 %s", "No wallpapers\nPlace in %s") \
    X(WALLPAPER_ENABLE_FAIL, "启用失败", "Enable failed") \
    X(WALLPAPER_FILE_INVALID, "文件无效", "Invalid file") \
    X(WALLPAPER_FILE_MISSING, "文件不存在", "File not found") \
    X(WALLPAPER_LOADING, "加载中…", "Loading…") \
    X(WALLPAPER_ENTER_LOADING, "壁纸正在加载中...", "Loading wallpaper...") \
    X(WALLPAPER_MIRROR_H, "镜像", "Mirror") \
    X(WALLPAPER_NAME_INVALID, "文件名无效", "Invalid filename") \
    X(WALLPAPER_NO_SD, "未检测到 SD 卡", "No SD card") \
    X(WALLPAPER_OVERWRITE, "覆盖", "Overwrite") \
    X(WALLPAPER_PATH_INVALID, "路径无效", "Invalid path") \
    X(WALLPAPER_PREVIEW_FAIL, "无法预览", "Cannot preview") \
    X(WALLPAPER_PREVIEW_OOM, "内存不足，预览失败", "Out of memory, preview failed") \
    X(WALLPAPER_PREVIEW_START_FAIL, "预览启动失败", "Failed to start preview") \
    X(WALLPAPER_ROTATE_LEFT, "左转", "Left") \
    X(WALLPAPER_ROTATE_RIGHT, "右转", "Right") \
    X(WALLPAPER_SAVE, "保存", "Save") \
    X(WALLPAPER_SAVE_AS, "另存为", "Save as") \
    X(WALLPAPER_SAVE_FAIL, "保存失败", "Save failed") \
    X(WALLPAPER_SAVING, "正在保存…", "Saving…") \
    X(WALLPAPER_SELECTED_FMT, "已选 %d", "Selected %d") \
    X(WALLPAPER_SET_SHUTDOWN, "设为关机", "Set as power-off") \
    X(WALLPAPER_SET_STANDBY, "设为待机", "Set as standby") \
    X(WALLPAPER_SHUTDOWN_ON, "关机中", "Power-off on") \
    X(WALLPAPER_STANDBY_ON, "待机中", "Standby on")


namespace Lang {

enum class LangStringId : size_t {
#define LANG_ENUM(name, zh, en) name,
    LANG_STRING_LIST(LANG_ENUM)
#undef LANG_ENUM
    kCount,
};

static constexpr size_t kStringCount = static_cast<size_t>(LangStringId::kCount);

static const char* const kZh[kStringCount] = {
#define LANG_ZH(name, zh, en) zh,
    LANG_STRING_LIST(LANG_ZH)
#undef LANG_ZH
};

static const char* const kEn[kStringCount] = {
#define LANG_EN(name, zh, en) en,
    LANG_STRING_LIST(LANG_EN)
#undef LANG_EN
};

const char* CODE = LANG_DEFAULT_CODE;

namespace Strings {
#define LANG_DECL(name, zh, en) const char* name = "";
LANG_STRING_LIST(LANG_DECL)
#undef LANG_DECL
} // namespace Strings

static const char* NormalizeCode(const char* code) {
    if (code != nullptr && std::strcmp(code, "zh-CN") == 0) {
        return "zh-CN";
    }
    return "en-US";
}

static const char* const* LiteralsFor(const char* code) {
    if (code != nullptr && std::strcmp(code, "zh-CN") == 0) {
        return kZh;
    }
    return kEn;
}

static void ApplyLiterals(const char* const* literals) {
#define LANG_APPLY(name, zh, en)                                                                               \
    Strings::name = literals[static_cast<size_t>(LangStringId::name)];
    LANG_STRING_LIST(LANG_APPLY)
#undef LANG_APPLY
}

static void PersistTask(void* arg) {
    const char* code = static_cast<const char*>(arg);
    {
        Settings settings("ui", true);
        settings.SetString("language", code);
    }
    ESP_LOGI(TAG, "language persisted: %s", code);
    vTaskDelete(nullptr);
}

const char* Current() {
    return CODE;
}

bool SetLanguage(const char* code, bool persist) {
    const char* normalized = NormalizeCode(code);
    const bool changed = (CODE == nullptr) || (std::strcmp(CODE, normalized) != 0);
    ApplyLiterals(LiteralsFor(normalized));
    CODE = normalized;
    if (persist) {
        // LVGL worker 栈在 PSRAM：写 NVS 放到内部 RAM 栈任务
        if (xTaskCreate(PersistTask, "lang_nvs", 4096, const_cast<char*>(normalized), 5, nullptr) !=
            pdPASS) {
            ESP_LOGE(TAG, "xTaskCreate(lang_nvs) failed");
        }
    }
    return changed;
}

void InitFromNvs() {
    Settings settings("ui", false);
    const std::string saved = settings.GetString("language", "");
    if (saved.empty()) {
        SetLanguage(LANG_DEFAULT_CODE, false);
    } else {
        SetLanguage(saved.c_str(), false);
    }
}

namespace {
struct LangStaticInit {
    LangStaticInit() {
        Lang::SetLanguage(LANG_DEFAULT_CODE, false);
    }
};
LangStaticInit g_lang_static_init;
} // namespace

} // namespace Lang

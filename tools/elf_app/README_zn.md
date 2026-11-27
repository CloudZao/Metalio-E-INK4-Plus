# ELF 应用与开发

**中文** | [English](README.md)

本目录集中管理现有应用、构建工具、ELF 加载器约定和公共开发接口。商店使用与安装事务见[应用商店](../../docs/app-store.md)。函数签名以 [app_api.h](../../main/business/app_store/app_api.h) 为准，允许的运行时导入以 [app_runtime_math.def](../../main/business/app_store/app_runtime_math.def) 为准。

## 应用目录

- [计算器](calculator/README_zn.md)：小数四则运算与顺序计算。
- [单位换算](unit_converter/README_zn.md)：长度、质量、温度与面积换算。
- [万年历](calendar/README_zn.md)：公历浏览、日期选择与今天定位。
- [进制换算](base_converter/README_zn.md)：32 位无符号二、八、十、十六进制换算。
- [英文阅读](english_reader/README_zn.md)：离线章节阅读、词汇卡片与练习。
- [大包诊断](package_check/README_zn.md)：下载与资源按需读取验证。
- [番茄钟](pomodoro/README_zn.md)：离线专注与休息计时。
- [重力迷宫](gravity_maze/README_zn.md)：倾斜控制、计分与循环音乐。
- [录音机](recorder/README_zn.md)：单条录音的采集、保存与播放。
- [摄像与图片](imaging/README_zn.md)：摄像头取景、图片与显示效果。
- [网络测试](network_test/README_zn.md)：协议收发、DNS/Ping 及生命周期验证。

构建目标不等于应用已适配该设备。各应用文档注明实际内容区和能力要求，尤其不能把能生成 397 ELF 理解为能在 397 上运行。

## 开发目录

- [1. 支持范围](#1-支持范围)
- [2. 快速开始](#2-快速开始)
- [3. 构建与工程组织](#3-构建与工程组织)
- [4. ABI 与生命周期](#4-abi-与生命周期)
- [5. 能力接口](#5-能力接口)
- [6. 图标与内容资源](#6-图标与内容资源)
- [7. 编写规范](#7-编写规范)
- [8. 发布与设备操作](#8-发布与设备操作)
- [9. 发布前验证与排障](#9-发布前验证与排障)
- [10. 宿主实现与许可](#10-宿主实现与许可)

## 1. 支持范围

- 470 使用 ESP32-S31 与 RISC-V `ilp32f` 工具链；397 使用 ESP32-S3 与对应 Xtensa 工具链。两者均输出 ELF32、小端、ET_DYN，应用接口版本为 `XZ_APP_ABI_VERSION == 1`，机器码不能跨芯片安装。应用还需检查板型的内容区尺寸与能力，构建成功不表示所有示例都适配该板。不能上传桌面 ELF 或普通 ESP-IDF 整包固件替代应用。
- 应用入口为 `int app_main(int argc, char** argv)`；`argc == 1`，`argv[0]` 指向 `xz_app_api_t`，不是命令行字符串。返回零表示初始化成功，非零表示启动失败。
- 宿主负责 ELF 加载、控件、回调和资源清理。应用返回后，注册的按钮和定时回调仍可运行；应用不得自行建立永久后台任务。
- 宿主提供托管的黑白绘图、三轴加速度采样、应用内返回键、推白切换画面、限时震动、前台音频播放、单槽录音、资源图片、前台摄像头取景及显式全刷接口，详见第 5 节。提供托管 HTTP/HTTPS、WebSocket/WSS、TCP/TLS 和 UDP 联网接口；不提供计步、后台闹钟、待机唤醒或任意文件系统访问。板上存在硬件或固件内部类，不代表 ELF 已获得该能力。商店自身的联网能力也不等于应用网络 API。
- 应用与固件共享执行环境，原生 ELF 不是安全沙箱。仅安装可信应用；SHA256 验证完整性，不证明发布者身份。局域网 HTTP 不提供传输身份认证，不应向公网开放服务。

## 2. 快速开始

在仓库根目录工作，启用与固件匹配的工具链环境，确保 `python` 指向 Python 3。标准构建脚本要求应用目录名与主源文件名一致；图标为 128×128 的二进制 PBM（P4）。先创建一个独立目录，再把下方完整示例保存为 `tools/elf_app/hello/hello.c`：

```powershell
New-Item -ItemType Directory -Path ./tools/elf_app/hello
Copy-Item -LiteralPath ./tools/elf_app/calculator/icon.pbm -Destination ./tools/elf_app/hello/icon.pbm
```

```c
#include "app_api.h"

static const xz_app_api_t* app;
static void* value_label;
static unsigned count;

static void Count(void* context) {
    (void)context;
    count = (count + 1) % 10;
    const char text[2] = {(char)('0' + count), 0};
    app->set_text(value_label, text);
}

int app_main(int argc, char** argv) {
    if (argc != 1 || !argv || !argv[0]) return -1;
    app = (const xz_app_api_t*)argv[0];
    if (app->abi_version != XZ_APP_ABI_VERSION ||
        !XZ_APP_HAS_FIELD(app, set_text) || !app->label ||
        !app->button || !app->set_text ||
        app->width < 240 || app->height < 320) return -1;

    const int zh = app->language && app->language[0] == 'z';
    void* title = app->label(app->root, 24, 24, app->width - 48, 48,
                            zh ? "点击计数" : "Tap counter");
    value_label = app->label(app->root, 24, app->height / 2 - 40,
                            app->width - 48, 80, "0");
    void* button = app->button(app->root, 24, app->height - 100,
                            app->width - 48, 64,
                            zh ? "加一" : "Add one", Count, 0);
    if (!title || !value_label || !button) return -1;

    if (XZ_APP_HAS_FIELD(app, set_style) && app->set_style) {
        const xz_app_style_t style = {36, XZ_APP_ALIGN_CENTER, 0, 0, 0};
        app->set_style(value_label, &style);
    }
    return 0;
}
```

替换编译器占位路径后构建；`readelf` 和 `objcopy` 必须在编译器同一目录中：

```powershell
$compiler = '<riscv32-esp-elf-gcc.exe 的完整路径>'
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/hello -Target esp32s31 -OutputDirectory ./tools/elf_app/hello/build/470
```

生成的 `tools/elf_app/hello/build/470/hello.elf` 是发布文件。构建不会自动上传、上架或升级设备固件。复制示例图标可用于入门，正式发布前替换成自己的图标。

为 397 构建应用时，使用匹配的 ESP32-S3 编译器，并分开保存产物：

```powershell
./tools/elf_app/build_app.ps1 -Compiler '<xtensa-esp32s3-elf-gcc.exe 的完整路径>' -AppDirectory ./tools/elf_app/hello -Target esp32s3 -OutputDirectory ./tools/elf_app/hello/build/397
```

`-Target` 默认为 `esp32s31`，保留原输出路径；选择 `esp32s3` 时默认输出到 `<应用>/build/esp32s3`，可用 `-OutputDirectory` 指定其他目录。脚本会选择架构参数和配套二进制工具，并检查运行时导入。本地安装 JSON 的 target 必须为 `esp32s3`，大小与 SHA256 必须对应生成的 ELF，两者放在设备同一个应用目录。397 原生支持黑白，图片区域默认抖点，16/8 灰阶模式不可用。



## 3. 构建与工程组织

[build_app.ps1](build_app.ps1) 编译 `<目录名>.c`，加入当前 SDK 和 `tools/elf_app/common/` 的头文件路径。若应用目录有 `build_content.py`，先运行它；随后检查动态导入，再嵌入 `icon.pbm` 和可选 `assets.bin`。脚本不会自动编译目录中的其他 `.c` 文件；较小逻辑可按现有示例拆成私有头内实现，多源文件项目需要维护等价的自定义构建流程。

当前标准编译选项如下；使用自定义构建时保留目标架构、位置无关代码、入口、导入检查和资源段处理：

```text
-mabi=ilp32f
-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv
-nostdlib -nostartfiles -ffreestanding -fPIC -shared
-fdata-sections -ffunction-sections
-Wl,--gc-sections,-e,app_main,--strip-all
-fvisibility=hidden -Os
```

不要假定 `printf`、`malloc`、`memcpy`、`memset`、C++ 标准库、ESP-IDF 或 LVGL 符号可以导入。编译器可能为结构体复制、清零或数值运算生成辅助调用；构建脚本会拒绝未登记导入。先调整代码或提供必要的应用内实现；若确需新增公共导入，由宿主维护者审查并同步更新运行时清单及固件，不能只放宽构建检查。番茄钟展示了本地复制、清零实现，计算器展示了允许的数学辅助导入。

## 4. ABI 与生命周期

- 使用当前头文件，不复制一个私自修改的接口结构。宿主只在 ABI 1 尾部追加兼容成员；不得重排、删除或更改已有成员类型。破坏兼容的修改需要新 ABI 版本。
- 先检查 ABI，再检查 `XZ_APP_HAS_FIELD(api, member)`，最后检查能力位和函数指针。不能因为 ABI 都是 1，就认为旧固件拥有新功能；也不能在边界检查前读取尾部成员。
- `root`、`width`、`height` 描述状态栏以下的内容区。只在该区域绘制，按尺寸排版，不自行占用或重画系统状态栏。宿主不替应用加标题；是否显示标题由应用决定。
- `host_context` 传给定时器、存储、键盘、退出、提示和防待机接口；`asset_context` 只传给资源读取接口。两者不可互换或自行构造。
- 所有 API 和回调在宿主 UI 线程使用。入口、按钮、确认、定时器和退出回调都应快速返回，不执行等待循环、长计算或大量同步 I/O。
- 回调数据应是应用存活期间有效的静态状态，不能是 `app_main` 的局部变量地址。文本创建和更新会复制文本；宿主不为应用托管任意工作线程或异步对象。
- 离页、删除页面或入口失败时，宿主停止音频会话与采样订阅、取消托管定时器、释放防待机与震动引用、删除键盘，再执行一次退出回调，随后释放界面和 ELF。入口返回非零时宿主保留该返回值用于日志定位，并立即释放 ELF，加载页显示运行错误。待机是页面上的覆盖层，不等于退出，不保证触发退出回调；唤醒后宿主等画面物理刷新完成才恢复应用定时器、采样和音频。

以下是需要完整前台计时能力时的检查方式。基础文本与按钮应用不需要强制要求所有新能力：

```c
if (!XZ_APP_HAS_FIELD(api, set_keep_awake)) return -1;
const uint32_t required = XZ_APP_CAP_TIME | XZ_APP_CAP_TIMER |
                          XZ_APP_CAP_EXIT | XZ_APP_CAP_KEEP_AWAKE;
if ((api->capabilities & required) != required || !api->host_context ||
    !api->monotonic_ms || !api->timer_start || !api->timer_cancel ||
    !api->set_on_exit || !api->set_keep_awake) return -1;
```

### ELF 加载流程

宿主重新校验安装清单、文件大小、SHA256、目标架构与 ABI 后，流式读取 ELF 并完成加载段分配、重定位和导入解析；不会按整个包大小常驻分配缓冲。随后在运行页调用应用入口，入口成功且加载页首次上屏后展示应用内容。入口失败时保留可返回的错误页，停止已注册的托管服务、执行退出清理、删除部分控件并释放 ELF。不支持的重定位或导入符号不会执行入口。原生 ELF 与固件共享地址空间，加载校验不构成安全沙箱。

应用注册的返回处理先于默认离页；HOME 保留系统导航。待机恢复须等物理画面就绪后再恢复定时器、采样和音频。

## 5. 能力接口

`capabilities` 表示宿主实现了相应接口，不表示此刻 SD 卡、时钟或其他资源可用。每次调用都检查返回结果。下列说明与头文件共同使用；函数完整签名见头文件。

### 基础 UI

- `label`、`button`：在指定父容器创建文本和按钮，返回不透明控件句柄；按钮回调通过 `context` 取应用状态，按下时自动绑定系统点击震动，遵循系统震动开关。应用不把句柄转换成 LVGL 对象使用。
- `set_text`：更新文本、按钮或输入框，调用时复制字符串，不支持用它更新容器或进度条。
- `set_style`：可选样式，包含字号、左中右对齐、圆角、边框和黑底白字。字号为零保持原字体。它不提供任意颜色、动画或字体文件加载。
- `set_enabled`：可选控件禁用状态，不改变控件可见性。不要用不可用按钮代替必要的错误说明。

### 时间：`XZ_APP_CAP_TIME`

`get_local_date` 是较早的可选接口，成功返回 1，未就绪或参数错误返回 0，不能按新接口的零成功规则解释。`get_local_time` 成功返回 `XZ_APP_OK`，填写本地年月日、时分秒、星期（0 为周日）和 UTC 秒级时间戳；设备时间未就绪返回 `XZ_APP_ERR_NOT_READY`。`monotonic_ms` 返回当前启动期间的单调毫秒数，不受系统校时跳变影响，不保证跨重启或深睡连续。

### 定时器：`XZ_APP_CAP_TIMER`

`timer_start(host, interval_ms, repeat, callback, context)` 支持 200ms—24h，最多同时 8 个；`repeat == 0` 为一次性，非零为重复。成功返回非零 ID，失败返回零。一次性定时器在回调前移除，回调内可以取消自身或创建新定时器；`timer_cancel` 对未知或已完成 ID 返回 `XZ_APP_ERR_NOT_FOUND`。ID 只在本次运行有效。

这些是前台回调，不是后台闹钟。待机或页面不活动时不执行应用回调，恢复后不补发遗漏次数，也不唤醒设备。倒计时比较单调时间，不按回调次数累计秒数；跨待机如何处理剩余时间由应用定义。定时器自身不阻止自动待机。

### 私有存储：`XZ_APP_CAP_STORAGE`

`storage_read/write/remove` 使用 1—48 字符的键，仅接受英文字母、数字、下划线、连字符，不接受路径。每键最多 16KiB，每应用最多 32 个已保存键。读取成功返回实际字节数，空 buffer 查询大小，容量不足不自动截断；缺失键返回 `XZ_APP_ERR_NOT_FOUND`。空数据键是有效数据，不应把零长度当作缺失。

`storage_write` 和 `storage_remove` 返回结果码；零长度写入可使用空数据指针。数据位于 [SD 布局](../../main/infra/storage/sd_paths.h) 的 `SD_APP_ROOT/settings/apps/<应用名称>--<appId>/<key>.bin`，与安装目录分开；更新保留状态，卸载删除状态及其恢复备份和临时副本。大小写遵循 FAT 文件系统，建议统一小写键。SD 导出或待机可能返回未就绪；不要绕过接口直接操作 SD。写入临时副本位于 `SD_APP_ROOT/temp/app_store/<应用名称>--<appId>/data-<key>.bin.part`，完成或失败后清理；恢复备份 `<key>.bin.cloud-backup` 保留在状态目录，读取可回退到中断写入留下的备份，不保证任意断电场景的数据完整性。断电留下的临时副本不用于恢复，下次写入同一个键时覆盖，也可在应用未运行时清理。应用名称使用安装清单固定的 `installedName`，ID 隔离同名应用；`storage_remove` 同时清理指定键的正式文件、备份和临时副本。

为存档定义版本、长度和必要校验。读取后先验证，再恢复状态；不要直接信任保存的数据。保存关键交互，长时间运行时限频保存，不逐帧或逐秒写 SD，也不只依赖退出回调保存。

### 扩展控件：`XZ_APP_CAP_WIDGETS`

- `container`：固定区域传 `scroll_y == 0`，纵向滚动区域传非零。列表由容器与文本、按钮组合，没有单独列表数据模型；黑白图形使用绘图接口。
- `clear_children`：删除指定容器的全部子控件，不删除容器本身；不得在绘制事件内调用。
- `input`：单行输入框，`max_chars` 为 1—256 个字符；确认回调可为空。字符上限不等于 UTF-8 字节容量，保存文本时留足缓冲区。
- `get_text`：接受文本、按钮、输入框；成功返回 UTF-8 字节数，不包括终止符。空 buffer 查询大小；实际容量需加一，容量不足返回 `XZ_APP_ERR_LIMIT`，有空间时把输出置为空字符串。
- `keyboard`：显式绑定输入框，当前输入框不会自动弹出键盘。每应用最多一个，使用系统英文/符号布局，不提供中文输入法。宿主负责按键初始化，应用不调用内部 LVGL 或键盘函数。键盘放在父容器底部，应用需要为它安排可见空间，避免遮住输入框。
- `keyboard_close`：取消待处理确认并删除键盘；之后可给其他输入框创建键盘。确认回调可读取文字并关闭键盘。输入框先删除不会留下可写入的悬空目标。
- `progress`、`set_progress`：创建 0—100 进度条，超过 100 的值钳制到 100，不播放动画。它不自动显示百分比文本。

### 退出：`XZ_APP_CAP_EXIT`

`set_on_exit` 注册一个短回调，后一次替换前一次，空回调取消注册。可在回调内尝试保存，但不能启动新定时器、打开键盘或重新持有防待机引用。断电、系统崩溃或存储不可用时不能保证回调和保存成功。

### 提示：`XZ_APP_CAP_NOTICE`

`notify` 使用系统顶部提示，调用时复制文本。仅在应用前台且未进入待机时显示；不用于持续进度、频繁计数、倒计时或开发调试刷屏。应用根据 `language` 提供相应文案。

### 防自动待机：`XZ_APP_CAP_KEEP_AWAKE`

`set_keep_awake(host, enabled)` 显式阻止无操作自动待机，不保网、不强制打开音频，也不禁止电源键主动待机。重复开启不会累加引用；暂停时关闭，离页或入口失败时宿主自动释放。只在任务确实需要时启用，不把它作为应用默认常驻行为。

### 绘图：`XZ_APP_CAP_DRAWING`

`drawing(parent, x, y, width, height)` 创建黑白绘图区，宽高最大 1024×1600，返回不透明句柄；像素缓冲由宿主管理，随控件删除释放。`draw(widget, commands, count)` 把命令复制进宿主画布：最多 1024 条，零条清空为白底。每条 `xz_app_draw_t` 描述直线（kind 0）、矩形（kind 1）或椭圆（kind 2），坐标相对绘图区且必须落在区内，矩形与椭圆含两端；线宽 1—8，`filled` 非零时矩形与椭圆填充黑色，`reserved` 必须为零。直线端点可任意方向。任一命令非法时整批拒绝并返回 `XZ_APP_ERR_INVALID`，画布保持原内容。绘图适合迷宫、棋盘等低频重绘，不用于逐帧动画。

### 加速度：`XZ_APP_CAP_ACCELERATION`

`set_acceleration(host, enabled)` 订阅三轴加速度采样，单位 mg，是重力加速度不是角速度。`get_acceleration` 返回最近 500ms 内的样本；未订阅、页面不在前台、待机或样本过期返回 `XZ_APP_ERR_NOT_READY`。坐标不自动旋转，竖屏中 X 正向向下、Y 正向向左。采样仅在前台运行，离页与待机自动停止，唤醒且画面刷新完成后按订阅状态恢复。不需要时关闭订阅，避免传感器持续采样耗电。

### 前台计时与返回键

`foreground_ms(host)` 返回应用累计前台毫秒；待机、推白等待与离页不累计，适合按实际游玩时长计时。`set_on_back(host, callback, context)` 注册返回键处理：返回非零表示应用已消费（例如从关卡回菜单），返回零使用宿主默认返回离页；后一次注册替换前一次，空回调取消。HOME 键始终走系统导航，不交给应用。

### 推白切换：`XZ_APP_CAP_SCREEN_CHANGE`

`request_screen_change(host)` 请求下一次物理提交先推白再显示目标画面，不阻塞 UI。请求后宿主暂停应用定时器、采样与音频，直到推白帧物理完成才恢复，避免旧画面残影期间继续跑业务。适合关卡切换等整屏内容变化；不要高频调用。

### 震动：`XZ_APP_CAP_VIBRATION`

`vibrate(host, duration_ms)` 请求 1—15000ms 单次震动，尊重系统震动开关与档位；开关关闭时为空操作并返回成功。上一次震动未结束时返回 `XZ_APP_ERR_BUSY`，不叠加。震动期间宿主阻止无操作待机，结束自动释放；电源键待机仍由系统处理。

### 音频播放：`XZ_APP_CAP_AUDIO`

托管前台播放，与录音互斥。`audio_get_caps` 查询实际支持的格式、来源与输出参数；当前支持 Ogg 封装单声道 Opus（`XZ_APP_AUDIO_OGG_OPUS`）及 16kHz 单声道 16 位 PCM WAV（`XZ_APP_AUDIO_WAV_PCM`），来源为应用资源节（`XZ_APP_AUDIO_ASSET`，用 `offset`/`length` 指定区域）或产品 SD 根目录下的相对路径（`XZ_APP_AUDIO_SD`，路径逐段校验，拒绝 `.`、`..` 与非法文件名）。`audio_play` 接纳请求后由宿主后台任务打开文件、解码和输出，返回成功仅表示请求已接纳；新请求替换当前播放。`audio_pause` 暂停或继续，暂停会关闭文件并归还音频硬件；`audio_stop` 请求停止，后台自行收尾。`audio_set_volume` 设置系统音量 0—100，与系统音量键共用，立即生效并持久化。`audio_get_status` 查询状态、结果码、进度、循环次数与当前系统音量，按调用方 `struct_size` 复制。

系统对话、录音测试等音频业务优先于应用播放：被抢占时播放失败或中断，`result` 为 `XZ_APP_ERR_BUSY`。待机、离页与推白等待期间自动暂停并归还硬件，恢复前台后继续。播放期间宿主保持功放与运行频率并阻止无操作待机；应用仍应在退出回调或离页前停止不需要的播放。

### 托管联网：`XZ_APP_CAP_NETWORK`

检查 `network_close` 字段边界、能力位及各函数指针后使用。`network_open(host, config, &id)` 复制配置并异步连接，成功仅表示请求已接纳。配置结构设置 `struct_size`；HTTP/HTTPS 和 WS/WSS 使用完整 URL，TCP/TLS/UDP 使用主机名或 IPv4 与独立端口。当前不提供 IPv6、UDP 本地端口绑定、广播、组播、MQTT 或监听服务。

`network_get_status` 按调用方 `struct_size` 复制状态。`network_send` 复制一条负载，成功不等于已发出；`pending_send` 清零后才能发送下一条。HTTP 的方法、请求头与正文在 `network_open` 中提供，不使用 `network_send`。HTTP 支持 GET、POST、PUT、PATCH、DELETE、HEAD；不会自动跟随重定向或重试。4xx/5xx 是已收到的 HTTP 响应，网络结果码与 `http_status` 分别判断。取消或超时不能证明服务器未执行写操作。

`network_read` 返回实际读取字节数；没有记录返回 `XZ_APP_ERR_NOT_READY`，零表示一条真实空消息。TCP/TLS 将连续接收数据合并到字节流记录，每次最多读取调用方 `capacity` 字节，未读部分继续保留；非空数据不接受零容量读取。HTTP、WS 和 UDP 每次读取一条完整记录；先查 `queued_messages` 和 `next_length`，容量不足返回 `XZ_APP_ERR_LIMIT` 并保留记录。HTTP/TCP/TLS 的分块不是业务消息，应用按累计长度自行组包。WS 和 UDP 保留消息边界，WS 文本记录 flags 为零，二进制及其他协议记录设置 `XZ_APP_NET_BINARY`。接收缓冲满时 HTTP 背压到总期限，其他协议报告上限并关闭，不静默丢弃或截断。

接收缓冲容量 `receive_buffer_size`、发送槽容量 `send_buffer_size` 和 HTTP 响应总量限制 `response_size_limit` 由 ELF 逐次指定。接收容量必须为正；发送容量可为零，HTTP 请求正文长度不能超过该容量。响应总量限制为零时允许在接收缓冲与期限约束下流式读取，不设固定累计上限。宿主校验整数范围、加法溢出及实际分配结果，不以固定内部 RAM 余量或预留倍数拒绝应用请求。WS 的收发容量不得超过现有协议实现支持的 65535 字节，IPv4 UDP 不得超过 65507 字节；这些是传输实现或协议边界。每应用最多保留四个句柄，整机最多四个活动网络工作，离页收尾仍占活动名额，每连接最多排队 32 条记录。URL 最多 1024 字节，TCP/TLS/UDP 主机名最多 253 字节；请求头最多八项，名称最多 64 字节、值最多 512 字节。拒绝头部换行、URL 用户信息及应用覆盖传输和 WS 握手控制头。

`network_get_header` 只读取宿主保存的 HTTP 响应头：Content-Type、Content-Length、ETag、Last-Modified、Location、Retry-After、Content-Range、Accept-Ranges；单值最多 1024 字节，名称忽略大小写。输出容量包含字符串终止符，返回长度不含终止符。未收到响应头返回 NOT_READY，缺失返回 NOT_FOUND，未保留的头返回 UNSUPPORTED。

`timeout_ms` 为 1000—60000ms。HTTP 从接纳开始计算总期限；其他协议用于初次联网、连接及每次发送，不限制连接空闲寿命。DNS 和底层驱动仍受各自阻塞收尾约束，取消不保证瞬时断开。TLS 默认使用系统信任根并验证服务端身份；可用 `ca_pem` 指定最多 8192 字节、仅用于该连接的 PEM 信任根，不提供跳过证书校验开关。宿主不会向应用目标自动附加设备标识、系统凭据或云业务请求头。

`network_close` 立即撤销句柄并异步收尾，不等后台退出；COMPLETE、CLOSED、ERROR 状态中的已入队数据可在关闭句柄前继续读取。应用退出、离页、待机、系统升级和推白暂停前台服务时撤销全部句柄，恢复后由应用重新连接，不自动重放发送。后台只持有宿主数据，不调用 ELF 回调；待机和关机等待连接收尾后才切换硬件。系统升级期间不接纳新的应用网络工作，并在擦写固件分区或下载前等待已有工作排空；排空超时则终止准备，不修改固件分区。实际内存分配或任务创建失败返回 MEMORY，后台活动名额不足返回 BUSY。工程 RAM 诊断阈值不作为 ELF 网络请求的准入条件。

`network_prepare(host, timeout_ms, &id)` 复用系统网络准备流程，只尝试恢复并等待网络，不发送 HTTP、WebSocket 等业务请求，也不创建远端业务连接。调用前检查字段边界、能力位及指针；可在 `app_main` 中调用，成功表示已异步接纳。超时范围 1000–60000 毫秒，句柄与普通请求共用名额及 `network_get_status`、`network_close`：OPENING 表示准备中，COMPLETE 表示系统网络已就绪，ERROR 携带失败原因。就绪不保证外网或特定服务可达。关闭句柄请求取消，底层等待可能持续至期限；不会修改已保存的网络配置或覆盖手动关闭 Wi-Fi。

首次成功接纳 `network_open` 或 `network_prepare` 后，应用在前台默认持续软保网，两次请求之间也不因空闲倒计时断网。使用前检查 `network_set_idle_timer` 字段边界和指针；`network_set_idle_timer(host, 1)` 归还页面保网引用，无活动网络工作时遵循系统空闲断网计时，`network_set_idle_timer(host, 0)` 恢复默认持续保网。仅接受 0、1，重复调用不累加引用；不修改系统计时配置，也不重置最近用户活动时间，开启时若已超时可立即断网。离页、待机、推白暂停和退出释放页面引用；恢复前台时沿用本次应用会话的计时选择。活动请求及其收尾始终保留自身软保网引用。这些引用不阻止自动待机或覆盖手动关闭 Wi-Fi。

#### DNS 与 ICMP Ping

`XZ_APP_CAP_DNS` 和 `XZ_APP_CAP_PING` 分别表示独立地址解析和 ICMP Echo 能力。先检查能力位、新接口字段边界及指针。两者复用系统 lwIP IPv4 网络、现有四个句柄与工作名额、前台保网、退出取消和后台排空流程；不修改 DNS 服务器、Wi-Fi 设置或系统信任根，不使用蜂窝模块的 AT 命令通道。

`network_resolve(host, hostname, timeout_ms, &id)` 异步返回系统解析器给出的首个 IPv4 地址。名称最长 253 字节，使用 ASCII 主机名或 IPv4 文本；域名只接受字母、数字、点、连字符和下划线，国际化域名使用 Punycode。期限为 1000—60000 毫秒，包含网络准备及地址解析。字面 IPv4 可直接返回，不证明发生了 DNS 查询；域名查询可使用系统缓存。当前不提供 IPv6、多地址枚举、自定义 DNS 服务器或其他记录类型。底层 `getaddrinfo` 不能强制中断：关闭立即撤销句柄，但工作名额和网络引用持续到原调用返回；返回后检查取消及期限，不交付过期结果。

`network_ping(host, config, &id)` 接收 `xz_app_ping_config_t`，设置 `struct_size`、目标、网络准备期限、探测次数、间隔、单次回复超时、负载大小和 TTL。网络准备及解析期限为 1000—60000 毫秒；探测次数由 ELF 指定，零表示持续到关闭句柄；间隔可为零，回复超时必须为正。负载范围 0—65507 字节来自 IPv4 协议边界，TTL 为 1—255。负载缓冲按请求实际分配，不套用工程 RAM 调试余量。Ping 在已有宿主工作线程执行，轮询取消并关闭 socket，不新增脱离生命周期的后台任务。

使用 `network_get_status` 查询流程，使用 `network_get_diagnostics` 读取 `xz_app_net_diagnostics_t`；输出结构设置 `struct_size`，宿主按容量复制。DNS 的 OPENING 表示准备或解析中，COMPLETE 表示已获得地址。Ping 的 OPENING 表示准备或解析中，CONNECTED 表示探测中；有限次探测结束且至少收到一次有效回复时为 COMPLETE，全部无回复时为 ERROR/TIMEOUT。部分丢包通过 `sent`、`received` 和 `timeouts` 判断，不等同于全部通过。统计含地址、序号、回复 TTL、最近/最小/最大/平均往返毫秒数及工作耗时；毫秒精度允许零毫秒结果，无回复时 RTT 为零。收发计数饱和到 UINT32_MAX。统计可在运行中读取，关闭后句柄失效；探测句柄不使用 `network_send`、`network_read` 或 HTTP 响应头接口。

ICMP 回显受路由、目标主机及防火墙策略影响；无回复不证明 HTTP 等服务不可达。电脑端 Service Lab 不处理 ICMP，Ping 由操作系统响应，应使用允许 ICMP 的已知目标。接收端校验来源地址、ICMP 类型、标识、序号、校验和及回显负载。

联网错误新增 `XZ_APP_ERR_TIMEOUT` 与 `XZ_APP_ERR_CANCELLED`，`error_code` 为可选底层数值码。状态中的收发字节数只计应用正文或负载并饱和到 UINT32_MAX，不包括协议开销，也不证明对端已经处理。接口不构成原生 ELF 的安全沙箱。[网络测试应用](network_test/README_zn.md)提供使用示例。

### 返回值

`XZ_APP_OK` 为零；`XZ_APP_ERR_INVALID` 为参数错误，`XZ_APP_ERR_NOT_READY` 为环境未就绪，`XZ_APP_ERR_NOT_FOUND` 为对象不存在，`XZ_APP_ERR_LIMIT` 为资源或容量上限，`XZ_APP_ERR_IO` 为存储、采集或未细分的解码失败，`XZ_APP_ERR_MEMORY` 为分配失败，`XZ_APP_ERR_UNSUPPORTED` 为格式或功能未支持，`XZ_APP_ERR_BUSY` 为资源被其他业务占用。并非所有函数都返回这些值：控件创建检查空指针，定时器创建检查零，文本与存储读取检查非负字节数，`get_local_date` 检查 1；`void` 函数没有结果码。不要把所有“非零”统一解释为失败。




### 单槽录音

通过 `record_get_caps` 查询实际输入参数。`record_start` 异步准备麦克风和临时文件；`record_pause` 暂停或继续写入，暂停期间仍持有麦克风。`record_stop` 异步停止并保存，`record_get_status` 查询状态、有效时长、结果及已保存文件相对路径。接口位于 ABI 尾部，使用前检查 `record_get_status` 字段边界、`XZ_APP_CAP_RECORDING` 能力位及函数指针。

录音固定为 16kHz、单声道、16 位 PCM WAV，每个应用仅保存一条。新录音成功提交后覆盖上一条；准备失败、空录音和写入失败保留旧文件。保存路径由宿主管理，不提供任意写文件接口。录音和播放互斥，忙时返回 `XZ_APP_ERR_BUSY`。系统音频优先，抢占会结束当前录音；离页、待机或退出时宿主停止采集并在后台保存，应用不应在停止请求后立即假定文件已保存。录音期间阻止无操作待机，电源键待机仍可执行。

### 图片与显示效果

检查 `XZ_APP_CAP_IMAGE`、`image_get_status` 字段边界及函数指针后，用 `image(host, parent, x, y, width, height)` 创建等比居中的图片区域。每应用最多四个区域，单区域宽高各不超过 1600，面积不超过 1 Mi 像素。图片区域不滚动，系统下拉手势保持可用。

`image_load_asset(host, widget, offset, length)` 在后台读取 `.xz.app.assets` 中的一张完整图片，区域长度为 1 字节至 4 MiB。支持普通及渐进式 JPEG、受限 PNG、A2I1、A2L8 与已有 EBGR 灰度封装；PNG 限非交错 8/16 位灰度或真彩、8 位索引色，透明部分合成到白底。所有输入转成有界 L8 原图，再生成显示副本。返回成功仅表示已接纳，通过 `image_get_status` 查询 `loading` 和 `result`；失败保留上一张可显示图片。新请求、切换到摄像头及控件删除会作废旧结果，退出时停止交付。

检查 `XZ_APP_CAP_IMAGE_TONE`、`image_get_effects` 字段边界与函数指针，先查询当前 `xz_app_image_effects_t`，再修改需要的字段并调用 `image_set_effects`。所有请求都从保存的原始 L8 图重新计算，不累计变换误差，不改变其他系统图片的固定压暗策略；失败保留上一张原图和显示图。

- 调色：`contrast`、`brightness`、`shadows`、`highlights` 均为 -100—100%，默认 0，分别控制以中灰为中心的反差、满量程亮度偏移、偏重暗部的曲线和偏重亮部的曲线。阴影和高光调整保持黑白端点。
- 软件曝光：`exposure` 为 -200—200，单位 0.01 EV，默认 0。按近似 sRGB 线性化 L8 灰度，乘以 2^EV 后重新编码；不改变传感器，不恢复已经丢失的细节。
- Gamma：`gamma` 为 25—400，单位 0.01，默认 100；使用 1/Gamma 指数，增大时提亮中间调。
- 滤镜：`sharpness` 为 -100—100%，默认 0；负数向 3×3 均值柔化，正数使用同一邻域做锐化。`invert` 为 0 或 1，默认 0。
- 几何：`rotation` 为顺时针 0、90、180 或 270 度；`mirror_x`、`mirror_y` 为 0 或 1。`crop_x/y/width/height` 使用原始 L8 图宽高的万分比，默认 `(0,0,10000,10000)`；宽高须为正且矩形不能越界，最小结果为一个像素。先裁剪，再镜像和旋转，输出按控件区域等比居中适配。裁剪针对已按区域尺寸限制解码的 L8 图，不会重新解码出更高分辨率。
- 输出：`mode` 为 16 阶、8 阶、4×4 黑白有序抖点或纯黑白。`threshold` 是二值化参数，0—255；-1 使用中点 128，只影响纯黑白模式，与系统文字的 I1 阈值无关，也不是阴影调节。8 阶不切换屏幕波形或保证更快刷新。板型不支持的原生灰阶模式返回 `XZ_APP_ERR_UNSUPPORTED`；黑白板默认抖点。

调色计算依次为软件曝光、亮度、阴影、高光、对比度、Gamma、反相，再进行邻域滤镜，最后按输出坐标量化或抖点。每一步限制在有效灰度范围。宿主使用固定 256 项查表和独立显示缓冲，关闭滤镜时不做邻域采样。

### 自定义像素算法

检查 `XZ_APP_CAP_IMAGE_PIXELS`、`image_set_pixels` 字段边界及相关指针后，应用可以在自己的缓冲区处理紧排 L8 像素。L8 中 0 为黑、255 为白，每像素一字节，无调色板或行填充。

`image_read_pixels(host, widget, processed, offset, buffer, length)` 复制像素，`processed=0` 读原始图，1 读显示图；每次最多 64 KiB，返回字节数或负错误码，到末尾返回 0。`image_get_status` 的 `source_width/height` 和 `width/height` 分别描述这两种视图，`revision` 在成功更新时递增。跨多个 UI 回调分块读取时，应暂停预览并避免换图或调参，或核对读取前后的 revision 并在变化时重试；离页后不得继续使用接口。

`image_set_pixels(host, widget, width, height, pixels, length)` 将应用缓冲复制为新原图，不借用应用指针。边长各不超过 1600，面积不超过 1 Mi 像素，长度必须等于宽乘高；成功后继续应用该区域的显示参数。它会停止该区域的摄像头交付、作废旧解码请求。若已在应用中处理像素而希望直接显示，应先把对应的调色和几何参数恢复为中性值；最终灰阶量化仍由宿主完成。

这些接口可用于自定义阈值算法、直方图、边缘检测、叠字或水印，而无需向宿主逐一增加算法接口。处理基础是有界灰度图，不提供 RGB 色相/饱和度、JPEG/PNG 编码导出或任意文件系统访问。

### 摄像头取景与完整刷新

检查 `XZ_APP_CAP_CAMERA` 后，`camera_preview(host, widget, 1)` 将前台摄像头最新灰度帧送到图片区域，零表示停止。取景复用同一硬件会话，帧间归还显示 DMA 门闩；只保留最新结果，交付后至少等待 800ms 再采集。实际显示速度受采集和墨水屏刷新限制，不代表视频帧率。`image_set_effects` 同样适用于取景；停止后可以继续调整保留帧。`camera_get_status` 查询运行状态、结果及已交付帧数，帧数不等于物理刷新次数。

待机、离页及退出取消取景，硬件后台收尾不访问 ELF 内存；待机唤醒且页面可交互后，按原需求恢复取景。采集失败停止自动重试，应用可重新开启。快速停止再开启会作废旧任务及其未交付帧，旧任务的结束状态不会覆盖新请求。内存不足或预算不足返回 `XZ_APP_ERR_MEMORY`，不支持的编码返回 `XZ_APP_ERR_UNSUPPORTED`。该接口仅提供取景，不写照片或录像，不申请网络；支持接口不表示摄像头此刻已连接。USB 摄像头仍与虚拟 U 盘互斥。

检查 `XZ_APP_CAP_FULL_REFRESH` 后调用 `request_full_refresh(host)`，宿主标记下一次提交完整刷新并合并在途重复请求。它不改变页面内容，不同步等待物理完成；470 使用后端完整刷新，397 使用自身完整刷新流程，不把请求解释为推白动画。仅前台、非待机的应用可以请求。

## 6. 图标与内容资源

图标仅以 `.xz.app.icon` 放进 ELF，无需额外上传图片。标准脚本把 `icon.pbm` 通过 [build_icon.py](build_icon.py) 转成 XZIC，并用 objcopy 添加为非加载、只读 PROGBITS。宿主读取图标时不执行应用；没有图标资源的旧包使用通用图标，但标准构建脚本要求提供 PBM。

XZIC v1 为小端：字节 0—3 为 `XZIC`；偏移 4、6、8、10 的 uint16 分别为版本 1、宽度 128、高度 128、格式 1；偏移 12 的 uint32 为像素长度 2048。偏移 16 起是按行排列、每字节高位在前的 1 位像素，1 为黑，0 为透明，总长度 2064 字节。元数据清单不是这个图标格式的一部分。

大词库等内容放在 `assets.bin`，脚本添加为 `.xz.app.assets`，不可标记 SHF_ALLOC 或压缩标志。使用前检查 `asset_context`、`asset_size`、`read_asset` 字段边界及函数指针。`read_asset(asset_context, offset, buffer, length)` 成功返回请求长度，失败返回零；按 `offset <= asset_size`、`length <= asset_size - offset` 校验范围，分小块同步读取，应用自行解析格式。

加载器流式读取安装包，不为整个包分配同等大小的 PSRAM，但执行段、数据段和重定位仍需要运行内存。32 MiB 的包上限不是可用 RAM 保证；不要把大量内容声明为普通加载常量或复制进长期缓冲区。`package_check` 和 `english_reader` 展示了资源构建与按需读取。音频播放的资源来源直接引用 `.xz.app.assets` 中的字节区域，由宿主后台读取，应用不需要把音频复制进加载段或自行读文件。

## 7. 编写规范

- 保持业务逻辑、UI 和资源格式职责清楚，复用现有框架。核心计算或状态机尽量不依赖宿主，便于模拟边界情况；不要为简单应用引入重框架。
- 所有外部数据都校验长度、范围、版本与返回值；需要输出字符串时考虑 UTF-8 字节数与终止符，避免格式化缓冲区溢出。
- 创建控件后检查结果，使用最少必要数量；只刷新发生变化的文字、数字或进度。墨水屏不适合高帧率动画，也不应把每秒倒计时做成全屏重建或弹窗。
- 用 `width`、`height` 排版并验证实际设备尺寸。固定区域使用非滚动容器，真实长列表使用纵向滚动；没有纵向溢出的区域应允许系统下拉菜单，不用滚动标记无故吞掉手势。
- 界面文案同时提供中文和英文，复用 `language`。当前常用 UI 字号为 25、30；数学字号 18、28、36 为子集，不能假定含中文。字体取决于设备字库，不任意请求不存在的字号或假定 SDK 会生成字体。
- 函数、数据和回调引用不超出应用生命周期；不直接调用硬件驱动、系统线程、LVGL 或文件系统绕过宿主管理。原生共享地址空间不是执行这些绕过的授权。
- 不把 URL、Token、请求头、配置对象、私有内容或用户数据放入提示和日志。宿主按 DEBUG/INFO/WARN/ERROR 记录必要定位信息；当前没有公开应用日志回调，不把 `printf` 或 `ESP_LOGx` 当成可用导入。
- 自有内容、图标、词库和依赖具备可分发授权，保留所需版权和许可证；校验和不能替代来源审核。

## 8. 发布与设备操作

1. 构建 ELF，保持应用 ID 稳定。ID 使用小写英文字母开头，后续仅小写字母、数字、下划线或连字符，总长不超过 48；版本为 `主.次.修订` 三段非负整数，不使用 `v` 前缀或预发布后缀，除零本身外不使用前导零，每段最多 6 位。
2. 在应用商店管理端上传 ELF，填写中英文名称、简述、类别、描述、更新说明，以及语言、提供者与版本。名称、版本等列表元数据由服务器清单管理，不是 ELF 中的自定义字符串自动推断；`uploadedAt` 由服务器生成。图标与可选内容资源在 ELF 内。
3. 上传后上架应用。相同应用 ID 和版本不可覆盖发布；内容变化时增加版本号。服务器按目标与 ABI 提供版本，不代替应用入口的能力检查。
4. 设备联网后，按[商店说明](../../docs/app-store.md#使用)确认服务配置，并由平台确认固定 MAC 绑定及板卡、芯片、固件和 ABI 兼容条件。烧录配套固件与安装 ELF 是两步；应用需要新能力时先升级固件。
5. 进入商店刷新，安装后可从商店或首页打开；点击应用行进入详情。多个安装按队列执行，点击进度按钮取消。离开商店会停止未完成下载。
6. 更新时在详情选择“检查更新”，确认后才安装。下架不删除本机应用；卸载会删除私有数据、恢复备份和临时副本。备份重要数据，不手工修改安装目录清单来替代正常更新。

单包最大 32 MiB。发布元数据需提供中英文版本，具体字段限制以管理端校验为准。商店目录、安装与更新规则见[应用商店](../../docs/app-store.md)。

### 源码、构建产物与安装测试包

源码、原始素材、构建脚本和中英文 README 保存在 `tools/elf_app/<应用名>/`。ELF、安装 JSON、构建目录和自动生成素材不纳入 Git。安装测试包的统一交付位置尚未确定；构建输出目录仅用于构建。

397、470 使用独立输出目录。每份安装测试 ELF 必须配套同型号的 `manifest.json`；目标、版本、字节大小和 SHA256 必须与本次产物一致。构建脚本仅生成 ELF 和资源，不生成安装 JSON。商店生成的安装信息与手工本地安装清单应分别核对，不以复制旧清单替代重新计算大小和摘要。

### 本地安装清单示例

以下仅为本地安装格式，不是云服务协议。在仓库根目录完成 Hello 构建后执行；示例 `id`、应用名称和版本用于本地测试，正式包使用其实际标识和版本。397 需改用对应输出文件及 `esp32s3`，且应用本身须满足该设备尺寸。


```powershell
$ErrorActionPreference = 'Stop'
$elfPath = './tools/elf_app/hello/build/470/hello.elf'
$manifest = [ordered]@{
    id = '0123456789abcdef0123456789abcdef'
    appId = 'hello'
    name = 'Hello'
    nameEn = 'Hello'
    description = 'Local installation example'
    descriptionEn = 'Local installation example'
    installedName = 'hello'
    version = '0.1.0'
    target = 'esp32s31'
    abiVersion = 1
    size = (Get-Item -LiteralPath $elfPath).Length
    sha256 = (Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash.ToLowerInvariant()
}
$jsonPath = Join-Path (Split-Path -Parent $elfPath) 'manifest.json'
$json = $manifest | ConvertTo-Json
[IO.File]::WriteAllText($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($jsonPath), $json, [Text.UTF8Encoding]::new($false))
```

将此目录中的 `hello.elf` 与 `manifest.json` 一起复制到 SD 的 `metalio/e-ink/apps/hello/`。这属于安装位置，不是统一交付位置。`installedName`、目录名和 ELF 基名一致；本地 `size` 为 JSON 数字，SHA256 为小写十六进制。每次重建重新生成，不能只修改版本或复制旧清单。安装与卸载的数据处理见[商店说明](../../docs/app-store.md)。

## 9. 发布前验证与排障

验证入口返回、旧固件拒绝或降级、两种语言的边界与截断、目标设备排版、按钮和滚动手势、重复进入退出、定时器取消、键盘反复打开关闭、存储缺失或损坏、SD 导出或不可用，以及应用更新后的存档兼容。计时应用验证暂停、恢复、阶段边界、系统校时和待机语义。模拟测试不能替代真机输入、墨水屏刷新、功耗和完整流程验证。

- 构建提示 `Unsupported runtime symbol`：检查动态导入与编译器生成的辅助调用，不只修改白名单。
- 安装成功但不能运行：核对目标架构、ABI、所需能力和入口返回值，查看 `AppStore`、`AppRuntime`、`AppServices` 的操作与阶段日志；不要让开发者打印完整 URL 或 Token 定位问题。
- 字体警告或缺字：检查设备实际字库与请求字号，不通过隐藏警告掩盖缺字。
- 输入框没有键盘：调用 `keyboard` 显式打开，检查父容器可见范围、绑定对象和是否已有键盘，检查返回句柄。初始化按键是宿主责任。
- 日期不可用：处理 `get_local_date == 0` 或 `get_local_time` 返回 `XZ_APP_ERR_NOT_READY`，不要把缺失日期当作有效今天。
- 存档读取失败：区分首次缺失、容量不足、格式校验失败和 SD 未就绪，提供可理解的恢复提示，不覆盖仍需保留的数据。
- 服务返回 404：核对所选服务是否支持应用商店。商店 URL 在 `main/infra/network/endpoints/cloudzao_endpoints.c` 的 `cloudzao_app_store_url` 中管理。

## 10. 宿主实现与许可

接口实现可查看 [运行页面](../../main/ui/apps/app_store/app_store_runtime.cc)、[基础服务](../../main/ui/apps/app_store/app_runtime_services.cc)和[存储与加载](../../main/business/app_store/app_store.cc)。这些是宿主维护入口，不是第三方 ELF 可直接链接的内部 API。

ELF Loader 位于 `components/elf_loader`，来自乐鑫 esp-iot-solution 提交 `5fbb2597b88f21ec41e952f81292ccd6ae569761`，保留 Apache-2.0 许可；本地 CMake 显式声明所需 lwIP 依赖。阅读示例所用内容的来源与许可见[英文阅读的来源与许可](english_reader/README_zn.md)，不能因打包进 ELF 而移除授权附录。

# ELF 应用开发规范与操作手册

**中文** | [English](DEVELOPMENT.md)

本文面向为本设备编写、构建和发布 ELF 应用的第三方开发者。接口定义以 [app_api.h](../../main/app_store/app_api.h) 为准，允许的编译器运行时导入以 [app_runtime_math.def](../../main/app_store/app_runtime_math.def) 为准。商店操作与网络配置见[应用商店使用说明](../../docs/app-store.md)，现有应用源码见[应用示例](README_zn.md)。

## 1. 支持范围

- 当前目标为 ESP32-S31，使用与固件匹配的 RISC-V 工具链，输出 ELF32、小端、RISC-V、ET_DYN、`ilp32f` ABI，应用接口版本为 `XZ_APP_ABI_VERSION == 1`。不能上传桌面 ELF、其他芯片固件或普通 ESP-IDF 整包固件替代应用。
- 应用入口为 `int app_main(int argc, char** argv)`；`argc == 1`，`argv[0]` 指向 `xz_app_api_t`，不是命令行字符串。返回零表示初始化成功，非零表示启动失败。
- 宿主负责 ELF 加载、控件、回调和资源清理。应用返回后，注册的按钮和定时回调仍可运行；应用不得自行建立永久后台任务。
- 当前不提供公开的音频播放、HTTP 请求、加速度计、计步、后台闹钟、待机唤醒、任意文件系统访问或通用图片控件接口。板上存在硬件或固件内部类，不代表 ELF 已获得该能力。商店自身的联网能力也不等于应用网络 API。
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
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/hello
```

生成的 `tools/elf_app/hello/hello.elf` 是发布文件。构建不会自动上传、上架或升级设备固件。复制示例图标可用于入门，正式发布前替换成自己的图标。

## 2.1 以计算器为模板

先阅读 [calculator.c](calculator/calculator.c) 和 [calculator_engine.h](calculator/calculator_engine.h)：前者负责入口、ABI 检查、控件布局、点击回调和显示更新；后者负责数字输入、运算状态与计算，不直接调用宿主控件。`Click` 将按键交给 `CalcInput`，再由 `Show` 同步结果、表达式和提示。`ApplyStyle` 在宿主提供可选样式接口时设置样式。

在仓库根目录构建原始计算器：

```powershell
$compiler = '<riscv32-esp-elf-gcc.exe 的完整路径>'
./tools/elf_app/calculator/build.ps1 -Compiler $compiler
```

输出为 `tools/elf_app/calculator/calculator.elf`。开发自己的应用时可复制该目录，并使目录名与主源文件名一致：

```powershell
Copy-Item -LiteralPath ./tools/elf_app/calculator -Destination ./tools/elf_app/my_app -Recurse
Rename-Item -LiteralPath ./tools/elf_app/my_app/calculator.c -NewName my_app.c
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/my_app
```

保留 `app_main` 的参数、ABI、接口存在性和内容区尺寸检查；按实际需求修改布局、回调及业务逻辑。不要直接链接 LVGL 或 ESP-IDF。`calculator_engine.h` 仅是计算器私有业务实现，可按新应用职责替换；重新绘制 `icon.pbm`，并在上传时使用新的应用标识。包通过入口、资源和导入检查不代表真机交互已验证，发布前按第 9 节验证。

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
- 离页、删除页面或入口失败时，宿主取消托管定时器、释放防待机引用、删除键盘，再执行一次退出回调，随后释放界面和 ELF。待机是页面上的覆盖层，不等于退出，不保证触发退出回调。

以下是需要完整前台计时能力时的检查方式。基础文本与按钮应用不需要强制要求所有新能力：

```c
if (!XZ_APP_HAS_FIELD(api, set_keep_awake)) return -1;
const uint32_t required = XZ_APP_CAP_TIME | XZ_APP_CAP_TIMER |
                          XZ_APP_CAP_EXIT | XZ_APP_CAP_KEEP_AWAKE;
if ((api->capabilities & required) != required || !api->host_context ||
    !api->monotonic_ms || !api->timer_start || !api->timer_cancel ||
    !api->set_on_exit || !api->set_keep_awake) return -1;
```

## 5. 能力接口

`capabilities` 表示宿主实现了相应接口，不表示此刻 SD 卡、时钟或其他资源可用。每次调用都检查返回结果。下列说明与头文件共同使用；函数完整签名见头文件。

### 基础 UI

- `label`、`button`：在指定父容器创建文本和按钮，返回不透明控件句柄；按钮回调通过 `context` 取应用状态。应用不把句柄转换成 LVGL 对象使用。
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

`storage_write` 和 `storage_remove` 返回结果码；零长度写入可使用空数据指针。数据位于 [SD 布局](../../main/boards/common/storage/sd_paths.h) 的 `SD_APP_ROOT/settings/apps/<应用名称>--<appId>/<key>.bin`，与安装目录分开；更新保留状态，卸载删除状态及其恢复备份和临时副本。大小写遵循 FAT 文件系统，建议统一小写键。SD 导出或待机可能返回未就绪；不要绕过接口直接操作 SD。写入临时副本位于 `SD_APP_ROOT/temp/app_store/<应用名称>--<appId>/data-<key>.bin.part`，完成或失败后清理；恢复备份 `<key>.bin.cloud-backup` 保留在状态目录，读取可回退到中断写入留下的备份，不保证任意断电场景的数据完整性。断电留下的临时副本不用于恢复，下次写入同一个键时覆盖，也可在应用未运行时清理。应用名称使用安装清单固定的 `installedName`，ID 隔离同名应用；`storage_remove` 同时清理指定键的正式文件、备份和临时副本。

为存档定义版本、长度和必要校验。读取后先验证，再恢复状态；不要直接信任保存的数据。保存关键交互，长时间运行时限频保存，不逐帧或逐秒写 SD，也不只依赖退出回调保存。

### 扩展控件：`XZ_APP_CAP_WIDGETS`

- `container`：固定区域传 `scroll_y == 0`，纵向滚动区域传非零。列表由容器与文本、按钮组合，当前没有单独列表数据模型或任意绘图 API。
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

### 返回值

`XZ_APP_OK` 为零；`XZ_APP_ERR_INVALID` 为参数错误，`XZ_APP_ERR_NOT_READY` 为环境未就绪，`XZ_APP_ERR_NOT_FOUND` 为对象不存在，`XZ_APP_ERR_LIMIT` 为资源或容量上限，`XZ_APP_ERR_IO` 为存储失败，`XZ_APP_ERR_MEMORY` 是分配失败错误码定义。并非所有函数都返回这些值：控件创建检查空指针，定时器创建检查零，文本与存储读取检查非负字节数，`get_local_date` 检查 1；`void` 函数没有结果码。不要把所有“非零”统一解释为失败。

## 6. 图标与内容资源

图标仅以 `.xz.app.icon` 放进 ELF，无需额外上传图片。标准脚本把 `icon.pbm` 通过 [build_icon.py](build_icon.py) 转成 XZIC，并用 objcopy 添加为非加载、只读 PROGBITS。宿主读取图标时不执行应用；没有图标资源的旧包使用通用图标，但标准构建脚本要求提供 PBM。

XZIC v1 为小端：字节 0—3 为 `XZIC`；偏移 4、6、8、10 的 uint16 分别为版本 1、宽度 128、高度 128、格式 1；偏移 12 的 uint32 为像素长度 2048。偏移 16 起是按行排列、每字节高位在前的 1 位像素，1 为黑，0 为透明，总长度 2064 字节。元数据清单不是这个图标格式的一部分。

大词库等内容放在 `assets.bin`，脚本添加为 `.xz.app.assets`，不可标记 SHF_ALLOC 或压缩标志。使用前检查 `asset_context`、`asset_size`、`read_asset` 字段边界及函数指针。`read_asset(asset_context, offset, buffer, length)` 成功返回请求长度，失败返回零；按 `offset <= asset_size`、`length <= asset_size - offset` 校验范围，分小块同步读取，应用自行解析格式。

加载器流式读取安装包，不为整个包分配同等大小的 PSRAM，但执行段、数据段和重定位仍需要运行内存。32 MiB 的包上限不是可用 RAM 保证；不要把大量内容声明为普通加载常量或复制进长期缓冲区。`package_check` 和 `english_reader` 展示了资源构建与按需读取。

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
4. 设备联网后，按[商店说明](../../docs/app-store.md#使用)配置云端或独立商店 API 基址，并由平台确认固定 MAC 绑定及板卡、芯片、固件和 ABI 兼容条件。烧录配套固件与安装 ELF 是两步；应用需要新能力时先升级固件。
5. 进入商店刷新，安装后可从商店或首页打开；点击应用行进入详情。多个安装按队列执行，点击进度按钮取消。离开商店会停止未完成下载。
6. 更新时在详情选择“检查更新”，确认后才安装。下架不删除本机应用；删除安装后私有数据仍保留。备份重要数据，不手工修改安装目录清单来替代正常更新。

单包最大 32MiB。在线目录每页最多 16 个应用，不设累计应用数量固定上限；设备按页获取并复用界面资源。名称上限为中文 32、英文 48 个字符；简述每语言最多 256 字节，类别 96 字节，描述和更新说明各 1024 字节，提供者 128 字节。接口中的语言为语言代码数组（如 `["zh-CN","en"]`），最多 8 项；本地清单以逗号分隔保存。文本字节上限使用 UTF-8 计算。

## 9. 发布前验证与排障

验证入口返回、旧固件拒绝或降级、两种语言的边界与截断、目标设备排版、按钮和滚动手势、重复进入退出、定时器取消、键盘反复打开关闭、存储缺失或损坏、SD 导出或不可用，以及应用更新后的存档兼容。计时应用验证暂停、恢复、阶段边界、系统校时和待机语义。模拟测试不能替代真机输入、墨水屏刷新、功耗和完整流程验证。

- 构建提示 `Unsupported runtime symbol`：检查动态导入与编译器生成的辅助调用，不只修改白名单。
- 安装成功但不能运行：核对目标架构、ABI、所需能力和入口返回值，查看 `AppStore`、`AppRuntime`、`AppServices` 的操作与阶段日志；不要让开发者打印完整 URL 或 Token 定位问题。
- 字体警告或缺字：检查设备实际字库与请求字号，不通过隐藏警告掩盖缺字。
- 输入框没有键盘：调用 `keyboard` 显式打开，检查父容器可见范围、绑定对象和是否已有键盘，检查返回句柄。初始化按键是宿主责任。
- 日期不可用：处理 `get_local_date == 0` 或 `get_local_time` 返回 `XZ_APP_ERR_NOT_READY`，不要把缺失日期当作有效今天。
- 存档读取失败：区分首次缺失、容量不足、格式校验失败和 SD 未就绪，提供可理解的恢复提示，不覆盖仍需保留的数据。
- 服务返回 404：核对所选服务是否支持应用商店。商店 URL 在 `main/cloudzao_endpoints.c` 的 `cloudzao_app_store_url` 中管理。

## 10. 宿主实现与许可

接口实现可查看 [运行页面](../../main/display/screen/app_store_screen/app_store_runtime.cc)、[基础服务](../../main/display/screen/app_store_screen/app_runtime_services.cc)和[存储与加载](../../main/app_store/app_store.cc)。这些是宿主维护入口，不是第三方 ELF 可直接链接的内部 API。

ELF Loader 位于 `components/elf_loader`，来自乐鑫 esp-iot-solution 提交 `5fbb2597b88f21ec41e952f81292ccd6ae569761`，保留 Apache-2.0 许可；本地 CMake 显式声明所需 lwIP 依赖。阅读示例所用内容的来源与许可见[示例说明](README_zn.md)，不能因打包进 ELF 而移除授权附录。

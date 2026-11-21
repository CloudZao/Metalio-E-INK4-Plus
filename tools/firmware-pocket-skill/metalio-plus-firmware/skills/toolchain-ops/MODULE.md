# Plus 材料与工具链

**只安装/加载 Skill 时不运行下载器或安装器，不要求 Python、工具链、02 材料包或短路径工程目录。** 安装完成指入口与包内资源可读；开始具体开发后才按需准备环境。原生 Windows PowerShell 5.1+ 可独立完成规划和取料，不需要 WSL、Linux 或 Docker。

## 按阶段进入

1. 先读 [取料路由](references/acquisition.md)：核对本次任务、当前阶段和已知环境，列出缺少的资源、用途与大小，再获取。安装阶段选集为零。不要用完整注册表充当安装待办。
2. 已知可复用环境先用 `scripts/Test-Environment.ps1` 核验版本、回执、解释器与依赖；还要满足本次所需 profile。通过才复用，不能把材料目录当成已经安装的环境。
3. 缺环境时，按 `Plan-Materials.ps1` 输出分阶段取料；最终准备基础环境仍需凑齐已验证的 `plus-base` 依赖闭包。官方整机和调试扩展按需要选择，不默认全量下载。
4. `Auto` 优先本次明确提供的02/原件目录及已校验缓存；部分02可补当前选集的缺项。没有02正常走冻结来源及可信镜像。显式 `Local` 或 `-Offline` 才要求选集完全来自本地，缺项就报告。
5. 资源与运行目录分开：**原始下载物、安装环境、本轮工程、交付产物是四种不同对象**。不修改原件；业务修改进 WORK。缓存和运行报告不写回 Skill，不搜索作者磁盘或其他项目。

## 开发时准备环境

```powershell
# 仅在具体开发需要新环境时执行；变量换成本机实际路径。
& "$Ops\scripts\Start-Toolchain.ps1" -Cache $RawCache -Destination $EnvRoot -Work $Work -DeviceRoot $DeviceRoot
```

`$Ops` 指本模块目录。只有独立 Skill 时省略 `-DeviceRoot`；已安装到宿主目录但另有材料时，显式提供本次设备根、`-Source` 或 WORK 中的 `-ContextFile`。路径绑定不下载，详见 [操作细节](references/operations.md)。已有有效环境可直接复用；入口不会覆盖来历不明的旧环境。

新环境使用用户可写、无空格、短 ASCII 路径，完整 ENV 路径最多40字符；安装前执行 `Test-SetupPaths.ps1`。40是当前安装器限制，不保证每个深层工具都支持长路径。不得为消除权限错误绕过宿主限制或修改全局策略。

固定身份是 **4.7英寸Plus / ESP32-S31、ESP-IDF 6.1.0提交 `fff9895c82d744c7237be8847347bdd1b07c6643`、CPython 3.13.17 x64**。不使用3.97的S3 SDK或工具清单。173项原始注册表不变；`plus-base`105项约1.27GB、`debug`109项、`official`169项、`full`173项。基础集合保留29个SDK归档和68个Python依赖，是已验证的保守闭包，不是每个任务理论上的最少字节。编译器单件约964MB。

## 交给构建模块

取料报告列出每项来源、版本、摘要、命中缓存与尝试；失败说明实际原因及下一步，不泛称 Skill 或设备不支持。记录安装与下载分别耗时。只有环境核验通过才标记环境就绪，业务和真机仍单独验收。

记录 `IDF_PATH`、`IDF_TOOLS_PATH`、Python、激活脚本与 `setup-timing.json`。每次构建在同一专用子进程激活后执行，见 [Windows 构建](../firmware-build/references/windows-build.md)。USB调试按本次授权执行，准备环境不代表获得烧录许可。

需要定位资源时用总包 `scripts/Read-PackageInfo.ps1 -Find "ninja" -Limit 3`，不要默认整表阅读。业务新增依赖与失败处理见 [操作细节](references/operations.md)。

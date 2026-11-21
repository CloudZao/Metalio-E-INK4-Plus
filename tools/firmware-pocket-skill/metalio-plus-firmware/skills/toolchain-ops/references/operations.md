# 操作细节与版本边界

仅在具体开发需要准备构建环境、复用环境、定位下载失败或配置来源时展开；只安装/加载 Skill 不运行这些步骤。本模块使用标准 Windows CPython 3.13.17；宿主自带其他 Python 只能运行部分检查，不能替代被冻结的构建解释器。所有命令从实际 Skill 位置调用，不依赖开发者电脑路径。

## 环境复用

已知现成环境先运行 `powershell.exe -NoProfile -ExecutionPolicy Bypass -File "<模块>\scripts\Test-Environment.ps1" -Destination "<环境目录>"`，检查回执、注册表/profile 摘要、Python 版本、IDF 命令及 `pip check`。通过后在本次构建的专用子进程激活；用相同的 `-ExecutionPolicy Bypass -File` 方式运行 WORK 内包含激活与构建的任务脚本，不假设安装进程的策略和环境变量会传回父终端。该检查不是完整安装目录文件审计；若环境被人工改过、版本不同或关键文件丢失，先报告，选择新的环境目录，不覆盖不明环境。组织策略禁止执行时报告实际权限阻塞，不修改用户或机器级策略。

已有标准 CPython 3.13.17 x64 可在 `Start-Toolchain.ps1` 或 `Prepare-Toolchain.ps1` 增加 `-BasePython <python.exe绝对路径>`。若没有，入口使用经摘要验证的标准安装器，在指定私有目录安装，不设全局 PATH、文件关联、启动器或快捷方式。安装器可能仍按官方行为登记本机安装信息，不能称为零系统痕迹的便携解包。

安装环境不要在完成后搬动：SDK/Python 虚拟环境与激活脚本记录绝对路径。Skill 包及原始下载物可搬运；运行环境在最终位置重建或使用匹配的原位置。

## 分阶段取料与安装

先读 [取料路由](acquisition.md)。PowerShell 入口不依赖 Python：`Plan-Materials.ps1` 默认 install 阶段选零项；明确阶段后输出冻结选集，`Get-Materials.ps1 -PlanFile ...` 只获取选集。需要新基础环境时，`Start-Toolchain.ps1` 才选择完整 plus-base 并调用安装器。阶段取料完成不代表环境已就绪。

有 Python 3.11+ 时可使用兼容包装器；`init` 只把随附注册表写入运行缓存，首次 `fetch` 前必须先 init。两个入口使用同一个 PowerShell 下载器。

```powershell
python "<模块>/scripts/toolchain_ops.py" init --mirror "<RAW_CACHE>"
python "<模块>/scripts/toolchain_ops.py" fetch --mirror "<RAW_CACHE>" --stage sdk --report-path "<WORK>/sdk-fetch.json"
```

Python默认 `--channel Auto`，支持 `--plan-file`、`--device-root`、`--context-file`、`--source`、`--mirror-map`、`--offline`；明确Local/Mirror分别使用 `--channel Local/Mirror`。`fetch` 默认stage=profile仅保留开发取料旧接口兼容，不得在Skill安装时调用而省略阶段。PowerShell同义参数见脚本帮助。

原有 `verify`、`Prepare-Toolchain.ps1` 和 Python `prepare` 仍验证完整选定 profile，不接受仅 sdk 或 tools 阶段的半套缓存。缓存保持 `registry.json + artifacts/...` 布局，多个阶段增量积累同一固定身份。`assets/acquisition-profiles.json` 未更改，已有环境的profile摘要仍可核验。

Auto本地缺项允许补齐；显式Local/Offline缺项报告并停止。已有完整文件但大小或SHA错误停止、不覆盖；选集之外的文件不影响选集。获取、展开与安装均以原始注册身份为准，不升级版本凑齐。

## 固定材料和业务依赖

基础准备保持 `--no-index` 与固定约束，防止安装期间临时升级。生成的 `Activate.ps1` 不设置 `PIP_NO_INDEX`、`PIP_FIND_LINKS` 或 `PIP_CONFIG_FILE`，不会自行改变用户已有 pip 策略。若宿主已经设置了离线策略，由当前测试/产品设置决定是否调整，不能把其存在归因于本 Skill。

组件存储位于运行目录；默认plus-base/debug为空（通用应用底座自身不依赖这21组件），official/full包含21个冻结组件。业务确需新增依赖时，先说明它与用户需求的关系，在独立工作区固定来源、版本、摘要并记录获取时间；不要修改基础注册表或用新版本替代已冻结组件。组件管理器可能查询远程资料，这不等同于要求产品全程断网；原件预置消除的是已列材料的下载波动。

### 业务新增组件的具体做法

当前固定的 `idf-component-manager 3.1.2` 按本地存储 → 配置的在线存储 → 官方注册服务查找；本地找到满足约束的版本即返回，找不到的组件/版本才继续查在线来源。`IDF_COMPONENT_LOCAL_STORAGE_URL` 不是“禁止联网”开关，因此不用删掉它来获取新组件。此行为依据该固定包的 `idf_component_tools/registry/multi_storage_client.py`，不推定其他版本也相同。

1. 在本任务自己的工程及构建目录开展，不修改共享基础环境。新工程不复制其他业务工程的 `dependencies.lock`、`managed_components` 或 CMake 缓存；同工程迭代则保留其现有 lock 的变更前副本。
2. 在该工程实际需要的 `idf_component.yml` 中对新组件填写明确版本，并把使用到的基础组件版本约束到本模块 profile 的固定版本。新增依赖若要求升级这些基础版本，先报告兼容冲突，不静默放宽。
3. 在这个工程的子 PowerShell 激活环境后，保留本地存储，给新增下载使用独立缓存；需要恢复到已知官方在线入口时，仅在该进程设置：

```powershell
# 放入 WORK 内专用的组件准备任务 .ps1，以 -ExecutionPolicy Bypass -File 启动
. "<环境>\Activate.ps1"
$env:IDF_COMPONENT_CACHE_PATH = "<本任务工程>\.component-cache"
$env:IDF_COMPONENT_STORAGE_URL = "https://components-file.espressif.com/"
idf.py --preview -C "<本任务工程>" -B "<本任务构建目录>" reconfigure
```

4. 让组件管理器依据本工程的 manifest 生成/更新它自己的 lock，检查解析结果：与基础 profile 同名的组件版本必须保持；新增直接与间接依赖均记录版本、下载来源、摘要和获取时间。随后用该 lock 构建。不把生成的工程 lock 回填到 Skill、基础 profile 或原始注册表。

若使用其他线上组件镜像，只替换这个子进程的在线存储地址，并核验新依赖身份。测试中两组对“基础表外的业务依赖”采用相同联网规则，相关耗时单列。官方项目的构建模块提供的配套 lock 只适用于其对应源码；不要把它当所有新应用的通用 lock。

本版默认plus-base选105原件、1,272,026,115字节，full才选173项、1,682,230,824字节；完整官方工程按official/full获取。SDK29归档和Python68包保留，以维持已冻结基础依赖闭包；GDB/OpenOCD/ROM ELF/clangd仅debug/full安装。安装后的SDK tools.json只把未选工具的顶层与平台覆盖install策略改为on_request，原始版本、URL、SHA不变，派生说明、原始摘要、当前关键文件摘要记入sdk-source-receipt；原始材料不改。安装器传入确切的工具版本列表，不用会自动补调试工具的required。工具展开、SDK、Python和构建目录额外占用数GB。调用前检查实际可用空间；不把“Skill 轻量”说成运行环境也只有数 MB。没有下载三个可选扩展包。

## 失败定位

| 症状 | 下一步 |
|---|---|
| 本地文件缺失 | Auto只补当前选集缺项；显式Local/Offline停止并列出缺项。 |
| 本地完整文件大小或摘要不同 | 报材料文件与注册身份，保留现场，不覆盖、不用新版本代替。 |
| 上游组件索引/约束文件 SHA 改变 | Auto/Official/Mirror可读取随Skill的22个原字节历史快照；其来源、日期、大小和SHA见assets/snapshot-provenance.json。仍逐件对原registry核验，不请求动态URL，不接受新SHA。Local严格验证调用者原料；缺失就报错。 |
| 所有地址失败 | 报具体文件、注册 URL、错误类别和次数，保留已核验缓存；连接恢复后继续，默认每地址最多 2 次。 |
| 缺少 `ESP_IDF_VERSION` | 用匹配环境的 `Activate.ps1`；准备过程从 SDK 实际 `version.txt` 导出该值，不能硬改版本掩盖不匹配。 |
| 长路径/中文路径或旧构建缓存 | 工程与运行目录使用短 ASCII 路径；不要搬用另一工程/系统的 CMake 缓存。 |
| wheel 构建失败 | 确认标准 3.13.17 x64、66 个 wheel 与 2 个 sdist 完整；不要改成 Python 嵌入式包或在线升级。 |
| 工程报 UART/IOT 组件、语言头或资源构建顺序问题 | 转固件构建模块，这是工程修复，不修改原始材料。 |

单个资源默认总获取预算900秒、连接/读超时30秒、每地址最多2次；只对暂时性错误重试，最多尝试4个去重来源。可针对大文件和已观测慢链路显式提高总预算（最多7200秒），不可无限重试。`-DeadlineSeconds`（Python `--deadline`）是单资源跨来源预算，不是整组选集完成保证。进度、续传偏移和最终SHA应有记录；准确限制以脚本参数为准。

可信备源、报告字段和错误分类见 [取料路由](acquisition.md)。不能从一次连接失败判断所有网络不可用；WinError 10013等网络访问受限与本地目录无写权限分别报告。HTTP404/403、TLS、完整性错误不能一概叫“网络不通”。失败后保留已核验资源，不把未尝试项写成已失败。

`.part` 与 `.part.json` 保存部分字节及冻结身份；下次同一缓存、同一profile可继续，切换来源也只能继续同一身份。服务返回206必须有匹配的Content-Range；返回200表示不支持Range，保留旧partial为 `.part.restart-<随机ID>` 并从0写新partial，绝不直接拼接200正文。完整partial只有最终全文件SHA通过才提升为原件；坏SHA保留为 `.part.rejected-<随机ID>` 并允许后续有界重试。没有身份sidecar的旧partial拒绝续写。已存在完整目标但损坏时停止，不静默隔离或覆盖它。保留文件会占空间，清理需由用户明确安排。

元数据快照总量675,879字节，来自r2已核验历史上游内容；随Skill携带它们是为消除动态索引/constraints不可复现问题。完整注册表保持原SHA，不把派生索引伪装成新的上游原件。组件安装时只从这些历史完整索引选择固定版本，并将单版本运行索引写到环境目录；原始快照不修改。

安装子进程的 `PIP_CACHE_DIR`、`TEMP`、`TMP` 定向到环境内的 `pip-cache`/`install-temp`，避免依赖用户全局缓存权限；不修改全局环境变量，也不将安装临时策略写入激活脚本。PowerShell在任何安装或缓存写入前按真实路径核验隔离，缓存后代reparse拒绝使用；外层短路径别名仍可用。

私有Python安装命令附 `/log <ENV>\python-install.log`，失败打印退出码和日志位置。短路径别名作为工具运行路径保留，真实解析路径只用于隔离检查；回执同时记录destination_alias/destination_resolved。激活脚本在SDK export后恢复已核验的短路径变量，防止上游规范化别名后再触发工具长路径限制。环境仍不能在完成后随意改别名映射或搬动。

## 证据口径

旧 r2 运维逻辑曾完成主机原生 Windows 环境准备与 ESP32-S31 hello_world 构建；新版增加自足取料、通道选择和边界检查，仍须与真实本轮环境结果分开记录。无本轮新环境、宿主 Skill 实验或真机日志时，对应状态是未测，不能继承为通过。


## 入口与权限说明

`Start-Toolchain` 和 `Get-Materials` 默认 Auto。需要确定性本地测试时，显式传 `-Channel Local -Source <本轮原件目录或ZIP>` 或 `-Offline`，用 `Resolve-Materials.ps1` 核对来源；`-Offline`由Get/Start执行禁网，离线状态另核对应报告；不要因为安装后的 Skill 找不到兄弟目录而直接进入网络下载。保存的 `material-context.json` 是本机运行配置，搬机器后重新绑定。显式通道覆盖自动选择；显式 Source 不能与 Official/Mirror 同用。`ContextFile` 只读取调用者给出的文件，不读取账户历史。

`Test-SetupPaths` 先检查≤40字符、ASCII、目录隔离，再用唯一临时小文件测试已有父目录的真实写权限，并删除自己的探针。它不证明 CPython 安装器对系统临时目录的权限，也不改变系统策略；若安装器访问被拒，保存安装退出码和日志，选择已有合格 Python 或请求宿主正常权限处理，不得绕过宿主权限限制。

若文件存在但交叉编译器无法打开深层 multilib C++ 头文件，核对完整路径长度、工具命令和同一最小编译。可以在本次激活后的进程调用 `Use-ShortToolsPath.ps1 -Alias <已有可写父目录下的短路径>`，创建或核对指向当前 tools 的 junction，并同步当前进程 PATH/IDF_TOOLS_PATH。脚本不替换已有不同目标，不搬动安装环境、不改注册表、不改全局 PATH；再次激活原环境后须再次应用别名。`LongPathsEnabled` 状态本身不证明具体工具支持长路径。

ccache 仅在最小编译经它挂起、绕开后能继续等证据成立时，使用构建入口的 `-DisableCcache`；这只影响该构建进程。不要把一次环境故障写成所有用户必须禁用缓存。构建后保留采用该规避的原因。


# 默认构建集合的调试提示

`plus-base`未安装GDB/ROM ELF调试扩展。固定SDK配置时可能提示无法生成esp_rom gdbinit；若编译退出0且镜像校验通过，该提示不代表BIN构建失败。确实需要调试器时选择`debug`或`full`新环境，不为消除这条调试提示默认下载全部扩展。


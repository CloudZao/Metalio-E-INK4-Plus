# 按任务取料：安装、规划、获取分开

适用Plus / ESP32-S31，原生Windows PowerShell 5.1+。本页只协调资源，不改变板级实现、业务自由度或集成协议。

## 1. 只安装 Skill

注册根 `SKILL.md`，保持完整包内目录，检查入口与所需文件可读即可。**不运行 Start-Toolchain、Prepare、fetch，不安装 Python，不要求02，不探测下载网站。** 不以构建工具未安装判定 Skill 安装失败。只有用户明确要求同时准备环境，或进入实际开发，才继续。

若用户同时提供材料目录，可记录其实际设备根；全局安装后用 `-DeviceRoot` 或 WORK 中的 `-ContextFile` 重新指定。绑定路径不下载，不读取作者磁盘和账户历史。没有02不用询问用户去补齐它。

## 2. 确定当前缺口

先查本轮显式给出的环境和已知安装回执，不扫描整盘。已有环境须通过 `Test-Environment.ps1`，并满足本轮请求的profile；直接复用合格环境，不为填满缓存重新取料。

需要资源时由 `Plan-Materials.ps1` 读取冻结阶段清单，说明用途、选集、大小上限。默认阶段是 `install`，选零项。其余阶段只能在有需求时选择：

| Stage | 当前用途 | plus-base中的原件数 |
|---|---|---:|
| `install` | 注册Skill | 0 |
| `python-bootstrap` | 准备固定CPython | 1 |
| `git-bootstrap` | 准备MinGit | 1 |
| `sdk` | 固定SDK主归档及其源码依赖 | 29 |
| `tools` | Plus交叉编译器、CMake、Ninja、IDF入口和ccache | 5 |
| `python-core` | 固定Python包池及constraints | 69 |
| `profile` | 环境安装器所需完整选集 | 105 |
| `extra` | 当前功能确需的明确资源ID，必须解释原因 | 按选集 |

前五个取料阶段（不含install/profile/extra）联合是105项基础集合。`official`/`debug`/`full` 按真实需求扩展，普通自建应用不默认取完整官方整机、21组件或调试器。资源列表不是业务能力白名单；确需表外依赖按 [操作细节](operations.md) 固定来源并记录。

68个Python包保留为已验证依赖池，并非按每个任务求出的理论最小集合。首次从零构建仍可能需要整个plus-base约1.27GB；编译器单件约964MB。分阶段的价值是避免安装Skill就下载、避免无关扩展、支持复用与失败后继续，不承诺把交叉编译器缩小。

## 3. 规划与获取

```powershell
# $Skill、$Work、$RawCache 指本机实际路径；WORK已存在。
$Ops = Join-Path $Skill 'skills\toolchain-ops'
& "$Ops\scripts\Plan-Materials.ps1" -Stage sdk -OutputFile "$Work\sdk-plan.json"
& "$Ops\scripts\Get-Materials.ps1" -PlanFile "$Work\sdk-plan.json" -Cache $RawCache -ReportPath "$Work\sdk-fetch.json"
```

若有本次设备目录，在获取命令增加 `-DeviceRoot $DeviceRoot`；也可用 `-Source <本轮原件目录或ZIP>`。只有01时省略即可。计划绑定原registry SHA，下载器重新核对选集、版本、字节和摘要，不接受另一个SDK的计划。

只需少量单件时，先用总包 `scripts/Read-PackageInfo.ps1 -Find "ninja" -Limit 3` 查真实ID，然后 `Plan-Materials.ps1 -Stage extra -Id <真实ID> -Reason <与本轮任务的关系>`。不得猜ID、选latest，或因为索引存在就下载全部条目。

已有计划先复用；需要调整时写新计划并解释变化。获取只产生缓存和报告，不执行资源中的安装器。凑齐选定profile后才运行 `Prepare-Toolchain.ps1`；需要完整新环境时也可直接由 `Start-Toolchain.ps1` 协调。阶段下载成功不等于环境安装成功。

## 4. 来源优先级与身份

- `Auto`：优先核验缓存与本轮02；部分02先用已有成员，缺项才读取随包历史元数据或在线获取。ZIP按选定成员取出并验证注册字节/SHA，不必为单件展开全部173项。原件不修改。
- `Local`：只允许当前本地来源和合格缓存；缺件报告，不联网。`-Offline`同样禁止联网，但可使用Skill随附并经原注册SHA核验的历史元数据；它们不包含工具二进制。
- `Official`：使用冻结原地址及可信备用来源。`Mirror` 需要实际配置镜像地址，按同一注册身份验证；不凭空编造自家镜像。
- 完整缓存或本地原件校验失败时保留并报告，不覆盖，不以换版本解决；部分下载用身份匹配的Range续传，完整SHA通过才成为可用原件。

自动备用来源限于可解释的映射：Espressif发布资产镜像 `dl.espressif.com/github_assets`、`dl.espressif.cn/github_assets`；官方组件CDN `components-file.espressif.cn`；Python包可通过TUNA/Aliyun的HTTPS简单索引查找**相同文件名及版本**。每个最终文件仍必须满足原注册大小和SHA。不是所有GitHub源码归档都必然有镜像，不能把镜像地址可推导说成资源已存在。

当前29个固定SDK源码归档主要仍来自 `codeload.github.com`；没有已知同字节备源时，使用用户配置的可信镜像，或报告具体缺失资源。不能因工具发布资产有镜像，就认为所有源码归档也都有镜像。

依据：[Espressif工具镜像说明](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/idf-tools.html)、[组件CDN说明](https://docs.espressif.com/projects/idf-component-manager/en/latest/troubleshooting/faq.html)、[TUNA PyPI说明](https://mirrors.tuna.tsinghua.edu.cn/help/pypi/)。镜像可用性随时间变化，实际尝试与报告为准。

其他镜像通过 `-MirrorMap` / `-MirrorBase` 明确配置，不使用任意代理、来历不明网盘，不关闭TLS校验，不执行下载的HTML。相同commit重新打包不一定有相同字节，SHA不符不能接纳。固定元数据快照有独立溯源记录，不伪称刚从线上下载。

`MirrorMap` 是保存在WORK的JSON对象，键为注册资源ID，值为最多两个真实HTTPS文件地址的数组（例如 `{"TOOL-005":["https://dl.espressif.com/github_assets/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip"]}`）。`MirrorBase` 按根地址加注册 `artifacts/...` 相对路径取料，适用于按注册布局发布的运维镜像。配置地址不改变注册身份。

## 5. 有限尝试与可行动反馈

单资源默认900秒总预算、每地址最多2次、最多4个去重来源；只有暂时性错误重试，大文件慢链路可明确提高预算。索引发现和实际下载都在有界尝试内。保存已验证缓存与有身份的partial，下次继续，不重复无限重启整组。

报告必须含资源ID、文件名、版本、用途、来源、各次错误与实际采用的文件SHA。阻塞时停止当前依赖步骤，未尝试的剩余项标为未尝试；仍可推进不依赖该资源的工作。

| 证据 | 对用户的说明和行动 |
|---|---|
| DNS、连接超时、连接重置 | 当前资源获取未完成；列出网站和资源，请检查连接、代理或能否访问对应网站后重试 |
| 网络套接字权限拒绝 | 当前执行环境限制访问该地址；检查宿主网络权限，不说本地文件无写权限 |
| HTTP404/403/限流 | 说明实际状态与地址；尝试可信备源，仍失败就报告资源不存在、访问被拒或限流证据 |
| TLS错误 | 保留证书错误，检查时间/证书/代理，不关闭校验 |
| 本地无权限/空间不足 | 列出实际目录和错误，选择合适运行位置；不要归因于网络 |
| 大小或SHA不同 | 文件身份校验未通过，保留异常证据并拒绝使用，不修改注册表 |

简短回复示例：“当前无法取得必需的 `<文件/版本>`（用于 `<用途>`）；已尝试 `<网站列表>`，结果为 `<实际错误>`。已完成的缓存保留。请检查这些地址的可访问性，或提供同版本且校验一致的文件/可信镜像；详情见 `<取料报告>`。”

不凭地理位置推断原因，不把资源下载失败说成设备或Skill不支持。未获得错误证据时写“原因尚未确认”。报告对URL凭据脱敏，不泄露Wi-Fi、Token或代理密码。

# 原生 Windows 构建

工程、build、日志写入本次 WORK，不写 Skill 或原始材料。使用短 ASCII 工程路径和已核验环境；每次进程重新激活。默认原生 Windows 和可用 USB 串口，构建本身不访问设备。

## 有界、可观测的构建入口

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "<Skill>\skills\firmware-build\scripts\run_build.ps1" -EnvScript "<ENV>\Activate.ps1" -Project "<WORK>\project" -BuildDirectory "<WORK>\build" -LogDirectory "<WORK>\logs"
```

脚本使用固定环境的 Python `-u` 启动 idf.py，stdout/stderr分别直接落盘，打印所属PID、日志字节与.ninja_log变化，保存开始/结束时间、真实退出码和命令参数。每次日志名唯一，构建成功返回0；失败保持实际退出码；总预算默认3600秒，可显式 `-TimeoutSeconds` 调整。预算到期只尝试终止本次创建的进程树，返回124并保留证据；若权限阻止停止，记录 `TIMED_OUT_STOP_UNCONFIRMED` 与PID，禁止直接在同一目录再次构建，先处理该进程。不会按进程名杀全部ninja/python。CLI宿主须允许此命令等待，或提供可恢复的进程会话；不要再在外面套 `Select-Object -Last` 等必须等结束才出结果的管道。

`COMMAND_PASSED` 只证明命令退出0，未证明功能或镜像验收。返回后仍执行本模块和验证模块的检查。

只在新工程或明确切换目标时使用 `-Action SetTarget`；它执行 `set-target esp32s31`，可能重建sdkconfig。随后检查最终生效配置再Build，不在每轮编译前反复set-target。配置调整用 `-Action Reconfigure`。已有其他芯片/SDK的构建缓存不复用；用新的本次构建目录，不用全局删缓存作为第一反应。

## 错误与停滞判断

- PowerShell原生stderr可能只是诊断信息。以进程真实退出码判命令成败，并读实际错误；不能由最后一行success或红字单独下结论。
- 没有新日志、`.ninja_log`计数不增长仅表示缺新进度证据，不能证明是管道阻塞或CPU少。查看该PID树的命令行/CPU变化、最新错误、依赖获取与最小编译，再选择恢复方法；重复等待没有新证据时切换诊断。
- 证实ccache相关挂起才加 `-DisableCcache`，只影响该任务；不默认永久禁用。
- 深层头文件路径超长时，可加 `-ShortToolsRoot <可写短路径>`；它调用运维短路径junction助手并改本进程环境。不要仅开LongPathsEnabled便宣称解决，也不要改编译架构参数来掩盖路径错误。
- 新生成器显式使用UTF-8文件编码；不要把“所有注释只能ASCII”当设备约束。工程路径短ASCII与文件内容UTF-8是两件事。
- 编译告警逐项判断，修正真实格式/类型/初始化问题；不要把继续添加全局 `-Wno-*` 当通用修复。

## 合并与交付

在已激活的专用任务进程调用：

```powershell
python "<Skill>/skills/verify-delivery/scripts/firmware_tools.py" merge --build "<WORK>/build" --output "<WORK>/delivery/app-version-dual.bin" --require-pocket --profile public-plus-pocket2 --report "<WORK>/delivery/structure.json"
```

按真实flasher_args合并，不套S3脚本、不改镜像头来伪装支持。合并后交给verify-delivery核对原件及实际交付副本，再按其真机流程使用授权的目标设备和端口。新版本默认具备USB串口调试条件，但应确认实际连接、设备身份与本次操作模式；不要凭示例端口烧录。


## 短路径别名与证据

idf.py可能把SUBST或junction展开为真实长/中文路径。确认日志中的实际CMake命令，不仅检查传入的短路径；不要在同一坏缓存里无限fullclean。可在新的构建目录用run_build.ps1加`-CMakeDirect`，在同一已激活进程调用固定CMake/Ninja，保留SDK检查、S31目标和原始退出码。该路线固定禁用ccache；它不是换SDK、绕过编译检查或修改全局Windows长路径策略。临时盘符必须先确认未占用，在创建它的进程树中使用，完成后撤销自己创建的映射。

run_build自动记录构建前后源码/配置摘要、实际编译输入和ELF/MAP/BIN；构建期间源码变更会使证据检查失败。之后修改源码必须重新构建，不能用旧ELF解码新BIN。下载与环境安装的回执与构建证据分开。日志文件有明确退出码和所属PID；长时间无新证据先诊断，不凭活进程保证在推进。

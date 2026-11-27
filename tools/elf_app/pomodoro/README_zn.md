# 番茄钟

**中文** | [English](README.md)

## 功能与使用

离线专注、短休息与长休息计时，默认 25 / 5 / 15 分钟，每四轮专注进入长休息。提供开始、暂停、继续和重置确认。

## 操作示例

选择阶段并开始；运行中先暂停才能切换阶段。重置需在确认窗口内再次点击。退出保存并暂停，重开后手动继续；提醒仅在前台屏幕显示。

## 源码与调用逻辑

`pomodoro.c` 处理界面、每秒托管定时回调、私有存储与防待机；`pomodoro_engine.h` 管理计时状态。以单调时间推进，每分钟及关键操作保存快照，退出回调暂停并保存。

## 兼容性与限制

内容区至少 600×900，需要时间、定时器、存储、控件、退出、提示和防自动待机能力。运行时阻止无操作待机，不保网；电源键仍可主动待机。断电可能丢失最近快照之后的进度；无后台响铃或唤醒。

[pomodoro.c](pomodoro.c) · [公共接口与生命周期](../README_zn.md#5-能力接口)

## 构建与安装

在仓库根目录启用匹配的 ESP 工具链，设置 `$compiler` 为 RISC-V 编译器完整路径后执行：

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/pomodoro -Target esp32s31 -OutputDirectory ./tools/elf_app/pomodoro/build/470
```

输出为本应用目录下的 `build/470/pomodoro.elf`。环境、安装清单和发布要求见[公共说明](../README_zn.md#8-发布与设备操作)。

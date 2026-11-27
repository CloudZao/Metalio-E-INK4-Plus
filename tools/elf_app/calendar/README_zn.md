# 万年历

**中文** | [English](README.md)

## 功能与使用

浏览 1600—2399 年公历，支持按月、按年切换、选择日期、标记今天和回到今天。

## 操作示例

选择 2024 年 2 月可查看 29 日。启动和点击“回到今天”时读取设备日期；时间未就绪时从 2000 年 1 月手动浏览，不标记今天。

## 源码与调用逻辑

`ReadToday` 检查可选日期接口和日期范围；`Leap`、`Days`、`Weekday` 计算月历，`Refresh` 更新 42 个日期格及选中日期。当前选择不写入私有存储。

## 兼容性与限制

内容区至少 600×1000。无农历、节日、提醒或后台日历同步；缺失系统日期不是有效的今天。

[calendar.c](calendar.c) · [公共接口与生命周期](../README_zn.md#5-能力接口)

## 构建与安装

在仓库根目录启用匹配的 ESP 工具链，设置 `$compiler` 为 RISC-V 编译器完整路径后执行：

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/calendar -Target esp32s31 -OutputDirectory ./tools/elf_app/calendar/build/470
```

输出为本应用目录下的 `build/470/calendar.elf`。环境、安装清单和发布要求见[公共说明](../README_zn.md#8-发布与设备操作)。

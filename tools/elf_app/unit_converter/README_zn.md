# 单位换算

**中文** | [English](README.md)

## 功能与使用

支持长度、质量、温度与面积，直接选择来源和目标单位，可交换单位、输入小数和正负数，显示 12 位有效数字。

## 操作示例

选择温度，来源 °C、目标 °F，输入 `100` 得到 `212`；低于绝对零度时提示无效。切换类别后重新检查来源和目标。

## 源码与调用逻辑

`unit_converter.c` 保存类别、单位选择和输入；`Convert` 先换算到标准单位，再换算到目标单位，温度额外校验绝对零度。`Refresh` 更新选项与结果，十进制解析和格式化复用 `common/decimal.h`。

## 兼容性与限制

复用 `AppInit`，内容区至少 600×1000；不支持当前 397 布局。状态仅在本次运行保留。

[unit_converter.c](unit_converter.c) · [公共接口与生命周期](../README_zn.md#5-能力接口)

## 构建与安装

在仓库根目录启用匹配的 ESP 工具链，设置 `$compiler` 为 RISC-V 编译器完整路径后执行：

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/unit_converter -Target esp32s31 -OutputDirectory ./tools/elf_app/unit_converter/build/470
```

输出为本应用目录下的 `build/470/unit_converter.elf`。环境、安装清单和发布要求见[公共说明](../README_zn.md#8-发布与设备操作)。

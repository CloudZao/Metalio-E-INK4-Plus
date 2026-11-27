# 进制换算

**中文** | [English](README.md)

## 功能与使用

二、八、十、十六进制同步显示同一个 32 位无符号数，二进制按字节分组；当前进制不支持的数字键禁用。

## 操作示例

选择十进制输入 `255`，十六进制显示 `FF`、八进制显示 `377`。切换输入进制保留数值；溢出时保留原值并提示。

## 源码与调用逻辑

`Click` 处理进制切换、清零、退格与数字追加；追加前用 `(UINT32_MAX-key)/base` 判断溢出。`Refresh` 更新四种表示及按键状态。

## 兼容性与限制

取值 0—4294967295，不提供负数、补码解释、小数或任意精度。内容区至少 600×1000，状态不持久化。

[base_converter.c](base_converter.c) · [公共接口与生命周期](../README_zn.md#5-能力接口)

## 构建与安装

在仓库根目录启用匹配的 ESP 工具链，设置 `$compiler` 为 RISC-V 编译器完整路径后执行：

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/base_converter -Target esp32s31 -OutputDirectory ./tools/elf_app/base_converter/build/470
```

输出为本应用目录下的 `build/470/base_converter.elf`。环境、安装清单和发布要求见[公共说明](../README_zn.md#8-发布与设备操作)。

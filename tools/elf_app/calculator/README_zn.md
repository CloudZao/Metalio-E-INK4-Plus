# 计算器

**中文** | [English](README.md)

## 功能与使用

支持小数四则运算、百分比、正负切换、退格、连续计算和重复等号；结果显示 12 位有效数字。按输入顺序计算，不应用运算符优先级。

## 操作示例

输入 `2 + 3 × 4 =` 得到 `20`。除零或超出范围后重新输入；AC 清空状态，退格只修改当前输入。

## 源码与调用逻辑

`calculator.c` 管理入口、界面与点击回调，`calculator_engine.h` 管理数字输入和计算状态。`Click` 调用 `CalcInput` 后，由 `Show` 更新结果、表达式和提示；`ApplyStyle` 使用可选样式接口。

## 兼容性与限制

内容区至少 600×900；当前 397 内容区不足。计算结果不持久保存。


[calculator.c](calculator.c) · [公共接口与生命周期](../README_zn.md#5-能力接口)

## 构建与安装

在仓库根目录启用匹配的 ESP 工具链，设置 `$compiler` 为 RISC-V 编译器完整路径后执行：

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/calculator -Target esp32s31 -OutputDirectory ./tools/elf_app/calculator/build/470
```

输出为本应用目录下的 `build/470/calculator.elf`。环境、安装清单和发布要求见[公共说明](../README_zn.md#8-发布与设备操作)。

## 以本应用为模板

先阅读 [calculator.c](calculator.c) 和 [calculator_engine.h](calculator_engine.h)：前者负责入口、ABI 检查、控件布局、点击回调和显示更新；后者负责数字输入、运算状态与计算，不直接调用宿主控件。`Click` 将按键交给 `CalcInput`，再由 `Show` 同步结果、表达式和提示。`ApplyStyle` 在宿主提供可选样式接口时设置样式。

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

保留 `app_main` 的参数、ABI、接口存在性和内容区尺寸检查；按实际需求修改布局、回调及业务逻辑。不要直接链接 LVGL 或 ESP-IDF。`calculator_engine.h` 仅是计算器私有业务实现，可按新应用职责替换；重新绘制 `icon.pbm`，并在上传时使用新的应用标识。包通过入口、资源和导入检查不代表真机交互已验证，发布前按[公共验证要求](../README_zn.md#9-发布前验证与排障)验证。

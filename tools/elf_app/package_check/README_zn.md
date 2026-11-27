# 大包诊断

**中文** | [English](README.md)

## 功能与使用

用于验证大包安装和 SD 资源按需读取，不是日常阅读应用。ELF 内附 16 MiB 确定性资源，运行时只抽样读取。

## 操作示例

安装并打开后点击“抽样校验资源”。完整成功时显示 17 / 17 段；安装时 SHA256 验证完整包，运行抽样不能替代整包校验。

## 源码与调用逻辑

`build_content.py` 生成 `assets.bin`。`Check` 用 4096 字节缓冲读取首部、中间、尾部等距的 17 段，逐字节验证确定性模式，不把整个资源复制到 RAM。

## 兼容性与限制

内容区至少 600×1000，且当前实现要求完整的现有接口结构及资源读取函数。SD 不可用或读取失败时校验失败。

[package_check.c](package_check.c) · [公共接口与生命周期](../README_zn.md#5-能力接口)

## 构建与安装

在仓库根目录启用匹配的 ESP 工具链，设置 `$compiler` 为 RISC-V 编译器完整路径后执行：

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/package_check -Target esp32s31 -OutputDirectory ./tools/elf_app/package_check/build/470
```

输出为本应用目录下的 `build/470/package_check.elf`。环境、安装清单和发布要求见[公共说明](../README_zn.md#8-发布与设备操作)。

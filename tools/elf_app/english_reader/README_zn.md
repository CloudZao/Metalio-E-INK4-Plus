# 英文阅读

**中文** | [English](README.md)

## 功能与使用

内置章节阅读、206 个词汇卡片与选择练习，内容离线可用；阅读位置和练习成绩仅在本次运行保留。

## 操作示例

先从章节目录进入正文并翻页，再切换词汇卡片或练习。退出重新打开后从初始状态开始；不读取用户书库文件。

## 源码与调用逻辑

`build_content.py` 将 `book.txt`、`words.tsv` 转为 `assets.bin` 与 `content.h`；`Show` 按模式组织内容，`Click` 更新章节、页码和答题状态。资源通过宿主 `read_asset` 按需读取。

## 兼容性与限制

内容区至少 600×1000，需要资源读取尾部接口。原文为 Lewis Carroll 的《Alice’s Adventures in Wonderland》，来自 [Project Gutenberg ebook 11](https://www.gutenberg.org/ebooks/11)。`book.txt` 及阅读附录保留完整许可，分发前核对所在地版权条件。

[english_reader.c](english_reader.c) · [公共接口与生命周期](../README_zn.md#5-能力接口)

## 构建与安装

在仓库根目录启用匹配的 ESP 工具链，设置 `$compiler` 为 RISC-V 编译器完整路径后执行：

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/english_reader -Target esp32s31 -OutputDirectory ./tools/elf_app/english_reader/build/470
```

输出为本应用目录下的 `build/470/english_reader.elf`。环境、安装清单和发布要求见[公共说明](../README_zn.md#8-发布与设备操作)。

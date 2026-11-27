# .fontpack 字库打包

**中文** | [English](README.md)

本工具将多字号、多 bpp 字形打包为 EFNT，供 SD 检索或 `font_data` 分区映射。单字号 `.ef` 转换见 [epdfont](../epdfont/README_zn.md)，设备安装见[字体扩展](../../docs/ui-font-extension_zn.md)。

## 环境与构建

需要 Python、FreeType 和 requirements 中的依赖。以下 Bash 命令在本目录执行：

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
./build_misans_mixed.sh
```

脚本使用已有 MiSans-Light `.ef` 字集模板、MiSans 字体、Noto Emoji 和 Latin Modern Math；先核对脚本列出的源文件与授权。输出位于 `fonts/MiSans-Mixed/`，并同步本目录固件包入口与 `main/assets/fonts/font.fontpack`；运行会修改这些输出。组合与许可证见[默认字库](fonts/MiSans-Mixed/README.md)。扩展字库由[独立说明](fonts/Extended-Fonts/README_zn.md)管理。

## 自定义打包

```sh
.venv/bin/python build_fontpack.py   --ttf '<源字体.ttf>' --from-ef '<模板.ef>'   --variants 25:1,30:1,30:2 -o '<输出.fontpack>' --verify
```

`--variant-map` 可指定各字号/bpp 的字体，`--sizes` 与 `--bpps` 可组合生成；字集可来自 `.ef`、`--chars-file` 或 `--intervals`。Emoji 与 Math 通过对应 merge 脚本加入，不能用缺少这些字形的包无条件覆盖产品默认包。完整参数见各脚本 `--help`。

## EFNT 格式

```text
Header 32B: magic="EFNT" | version:u16 | flags:u16 | total_items:u32
            index_offset:u64 | data_offset:u64 | reserved:u32
Index 16B: key:u64=(unicode<<32)|(size<<16)|bpp
           data_offset:u32 | data_size:u16 | reserved:u16
Data: width:u16 | height:u16 | x_offset:i16 | y_offset:i16 | advance:u16 | bitmap[]
```

索引按 key 升序，位图 MSB-first 紧排，无逐行填充。设备读取实现见 [font_loader.h](esp32/font_loader.h) 和 [font_loader.c](esp32/font_loader.c)，调用示例见 [example_usage.c](esp32/example_usage.c)。

## 安装与限制

SD 形式放在产品字体目录，Flash 形式写入 `font_data`。Flash 写入会替换原字库，先备份并检查型号、分区与容量，具体布局以[分区说明](../../partitions/README_zn.md)为准。`flash_fontpack.sh` 需要设备端口与包路径参数，只在明确需要烧录时执行。

查询不到字形时检查 unicode、size、bpp 是否与包内 key 一致；模板不存在时先用 epdfont 生成；文件过大时调整字集或规格，不越过分区容量或仅通过增大缓冲掩盖问题。字体分发继续遵守各自许可。

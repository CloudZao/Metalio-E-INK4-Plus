# 固件资源生成工具

**中文** | [English](README.md)

本目录管理固件内置资源的生成和压缩，不管理应用私有资源或交付包。资源原文件仍在相应资源目录；ELF 私有 `build_content.py` 留在应用目录。

## 构建自动调用

- `generate_home_icon_pixels.py`：将首页图标转为独立分块的无损压缩数据，由 `main/assets/assets.cmake` 调用。
- `generate_quick_control_icons.py`：生成快捷控制图标数据，由同一 CMake 文件调用。
- `compress_binary.py`：将二进制保存为长度与 zlib 数据，构建用于内置关机图。输入源文件和输出路径为两个位置参数。
- `compress_lvgl_font.py`：将 LVGL `fmt_txt` 位图重编码为原生压缩格式，固件字体构建使用它；同时向像素字体生成器提供编码函数。
- `gen_lang.py`：由资源构建生成中英文运行时字符串表及提示音嵌入；原始语言资源在 `main/assets/`。
- `pack_ap_fileman.py`：生成本地文件管理页面资源，输入 HTML、翻译及输出目录由构建指定。
- `attach_upgrade_resources.py`：固件构建后的资源附加步骤，由根 CMake 调用；它修改指定输出镜像，不修改原始图片。

参数以对应脚本的 `--help` 和 CMake 调用为准。图像与字体生成需要 Pillow；其余依赖由脚本实际导入决定。构建会写入输出路径，不能把输出指向原始素材。

## 手工再生成

`generate_pixel_theme_font.py` 从仓库的 Latin Modern Math 字体生成像素主题数字子集，复用同目录 `compress_lvgl_font.py`。字体源与许可保留在 `tools/ttf-fonts/LatinModernMath/`；脚本内资源定位以 `tools/` 为基准。修改生成器后通过 `--help` 确认输出参数，并检查生成内容及授权说明。

调用关系和显示内存约束见[UI 模块](../../main/ui/README_zn.md)；资源 Flash 布局见[分区说明](../../partitions/README_zn.md)。阅读字体转换、字库打包、A2I1 文件转换分别见[工具索引](../README_zn.md)。

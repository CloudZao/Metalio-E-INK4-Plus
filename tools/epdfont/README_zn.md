# .ef 阅读字体转换

**中文** | [English](README.md)

把 TTF/OTF 或 LVGL `fmt_txt` 字体源转换为单字号 `.ef`，供阅读页从 SD 加载。多字号 EFNT 包由 [fontpack](../fontpack/README_zn.md) 管理，系统字体选择及缺字回退见[字体说明](../../docs/ui-font-extension_zn.md)。设备格式实现以 [epdfont.h](../../main/ui/common/drawing/font/epdfont.h) 与 [epdfont.cc](../../main/ui/common/drawing/font/epdfont.cc) 为准。

## 环境

需要 Python 3、FreeType、`freetype-py` 与 `fonttools`。以下 Bash 命令从仓库根目录开始；Windows 使用虚拟环境中对应的 `Scripts/python.exe`：

```sh
python3 -m venv tools/epdfont/.venv
tools/epdfont/.venv/bin/pip install -r tools/epdfont/requirements.txt
```

Linux 缺少 FreeType 时安装系统运行库。保留字体原始授权，确认允许转换与分发。

## 单文件转换

```sh
tools/epdfont/.venv/bin/python tools/epdfont/fontconvert.py   --from-ttf '<源字体.ttf>' --size 25 --bpp 2   --intervals reading_zh -o '<输出目录>/misans_25_2.ef'
```

也可用 `--from-lvgl-c <字体源.c>` 替代 `--from-ttf`，从实际存在的 LVGL 源提取字集与字形。输出命名为 `{前缀}_{字号}_{bpp}.ef`；支持 1、2 bpp，按设备绘制需求选择，不把某个 bpp 的文件大小作为通用保证。

`--intervals` 接受 `ascii`、`latin1`、`cjk_common`、`cjk_punct`/`punctuation`、`reading_zh` 预设或十六进制区间。只生成实际所需字集；全 CJK 扫描会增加体积与耗时。完整参数使用 `--help` 查看。

## 多字号批量转换

先生成一份 `.ef` 作为码点模板，再执行：

```sh
tools/epdfont/.venv/bin/python tools/epdfont/gen_range.py   --ttf '<源字体.ttf>' --from-ef '<模板.ef>'   --sizes 10-40 --bpp 2 --jobs 6 --out-dir '<输出目录>'
```

`--sizes` 支持单值、逗号列表和范围，`--prefix` 设置文件名前缀。显式提供字体与模板，不依赖脚本内的本地示例路径。`extend_whitespace.py` 用于补充空白码点，修改字集后重新生成依赖该字集的包。

## 安装与排障

将文件放入 SD 的 `metalio/e-ink/fonts/`，通过阅读字体入口选择；实际搜索规则以固件为准，不修改源码宏替代用户字体选择。虚拟环境和本地输出分开维护，安装前检查文件完整性。

- 导入失败：核对所用 Python 环境、requirements 和 FreeType 动态库。
- 找不到模板：先生成 `.ef`，再指定 `--from-ef`。
- 缺字：核对源字体覆盖和模板码点，不能靠改变字号补齐不存在的字形。
- 设备无法加载：核对产品目录、文件格式及 SD 状态；排查前备份原字体。

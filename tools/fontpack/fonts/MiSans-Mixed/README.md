# MiSans-Mixed fontpack（综合字重 + Emoji + Math）

墨水屏 **百问 AI / A2UI** 推荐包：同一文件内按「字号 + bpp」混入不同粗细，并**默认合并 Noto Emoji 30@2** 与 **Latin Modern Math 18/28/36@2**。

| 项 | 说明 |
|----|------|
| 包名 | **MiSans-Mixed**（综合） |
| 字集 | `tools/epdfont/MiSans-Light/misans_25_2.ef` 约 6009 码点（含空白） |
| **生产产物** | `fonts_misans_25_30.fontpack`（约 **4.8 MiB**，含 Emoji + Math） |
| Emoji | Noto Emoji **30@2** |
| Math | Latin Modern Math **18/28/36@2**（A2UI 公式；与 UI 25/30 字号错开） |
| 设备查询 | Key = `(unicode<<32)\|(size<<16)\|bpp` |

> 本目录只保留一份 `fonts_misans_25_30.fontpack`（汉字 + Emoji + Math）。根目录 `tools/fontpack/fonts_misans_25_30.fontpack` 与其内容相同，供烧录脚本使用。

## 规格与字重映射

| size | bpp | 字重 / 粗细 | 源 TTF | 用途建议 |
|------|-----|-------------|--------|----------|
| **25** | **2** | **Light**（细） | `MiSans-Light.ttf` | 小号正文 / 全局 UI |
| **30** | **2** | **Light**（细） | `MiSans-Light.ttf` | A2UI 正文 `font_regular` |
| **30** | **4** | **Semibold**（半粗） | `MiSans-Semibold.ttf` | A2UI 加粗 `font_bold` |
| **30** | **2** | Emoji | `NotoEmoji-Regular.ttf` | 表情符号 |
| **18/28/36** | **2** | Math | `latinmodern-math.otf` | A2UI `Math` 公式（脚本/正文/展示） |

源 TTF 默认在 `tools/ttf-fonts/`（MiSans、Noto_Emoji、LatinModernMath）。

## 生成命令（推荐）

```bash
cd tools/fontpack
./build_misans_mixed.sh
```

脚本内部：MiSans 混包 → 合并 Emoji → 合并 Math → 写入 `fonts_misans_25_30.fontpack`（烧录默认）。

## 烧录

```bash
cd tools/fontpack
./flash_fontpack.sh /dev/ttyACM0 ./fonts_misans_25_30.fontpack
```

`build_misans_mixed.sh` 还会同步到仓库根目录 **`use_font/font.fontpack`**（`idf.py flash` / `merge_firmware.sh` 默认读此文件）。

## 手动分步（调试）

```bash
# 1) 汉字 base（临时，勿作为烧录产物）
.venv/bin/python build_fontpack.py \
  --from-ef ../epdfont/MiSans-Light/misans_25_2.ef \
  --variant-map "25:2=../ttf-fonts/MiSans/MiSans-Light.ttf,30:2=../ttf-fonts/MiSans/MiSans-Light.ttf,30:4=../ttf-fonts/MiSans/MiSans-Semibold.ttf" \
  -o fonts/MiSans-Mixed/.build_base.fontpack --verify

# 2) 合并 emoji
.venv/bin/python merge_emoji_into_fontpack.py \
  --base fonts/MiSans-Mixed/.build_base.fontpack \
  --emoji-ttf ../ttf-fonts/Noto_Emoji/static/NotoEmoji-Regular.ttf \
  -o fonts/MiSans-Mixed/.build_emoji.fontpack --verify

# 3) 合并 math 到最终包
.venv/bin/python merge_math_into_fontpack.py \
  --base fonts/MiSans-Mixed/.build_emoji.fontpack \
  --math-ttf ../ttf-fonts/LatinModernMath/latinmodern-math.otf \
  -o fonts/MiSans-Mixed/fonts_misans_25_30.fontpack --verify
```

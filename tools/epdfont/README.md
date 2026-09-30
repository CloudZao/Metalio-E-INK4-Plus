# epdfont 字体转换工具

把 TTF/OTF 或仓库内 LVGL `fmt_txt` 字体 `.c` 转成设备用的 **`.ef`**（EPD Font），拷到 SD 卡供阅读页按需加载。

格式与设备端加载说明见：[`../../.cursor/docs/epdfont-SD轻量字体.md`](../../.cursor/docs/epdfont-SD轻量字体.md)

---

## 环境

在仓库根目录或本目录均可执行。建议用本目录 venv：

```bash
cd tools/epdfont
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

依赖：`freetype-py`（TTF 渲染）、`fonttools`。  
`.venv/` 已在 `.gitignore`，不要提交。

系统还需能加载 FreeType（Linux 一般自带；WSL 缺库时安装 `libfreetype6`）。

---

## 产物命名

```
{前缀}_{字号}_{bpp}.ef
```

例：`misans_25_1.ef`（25px、1bpp）、`misans_25_2.ef`（2bpp）。

| bpp | 说明 |
|-----|------|
| **1** | 墨水屏推荐：体积小，与 I1「非白即黑」匹配 |
| **2** | 更接近灰阶 AA，体积约 ×1.8 |

设备默认路径：`/sdcard/metalio/e-ink/fonts/misans_25_2.ef`（另有备用名，见固件 `BookScreen` / `sd_paths.h`）。

---

## 1. 单文件转换：`fontconvert.py`

### 从 LVGL `.c` 导出（推荐，字集与现网一致）

仓库里若还有 `font_misans_regular_25_2.c` 一类源：

```bash
cd tools/epdfont

# 2bpp → 1bpp
.venv/bin/python fontconvert.py \
  --from-lvgl-c ../../main/display/font/font_misans_regular_25_2.c \
  --bpp 1 -o misans/misans_25_1.ef

# 保持 2bpp
.venv/bin/python fontconvert.py \
  --from-lvgl-c ../../main/display/font/font_misans_regular_25_2.c \
  --bpp 2 -o misans/misans_25_2.ef
```

### 从 TTF/OTF 生成

```bash
.venv/bin/python fontconvert.py \
  --from-ttf /path/to/MiSans-Regular.ttf \
  --size 25 --bpp 1 \
  --intervals reading_zh \
  -o misans/misans_25_1.ef
```

`--intervals` 支持预设名（逗号分隔）或十六进制区间 `0x4E00-0x9FA5`：

| 预设 | 覆盖 |
|------|------|
| `ascii` | `0x20–0x7E` |
| `latin1` | `0xA0–0xFF` |
| `cjk_common` | 常用汉字区（约 GB 级） |
| `cjk_punct` / `punctuation` | 中日标点等 |
| `reading_zh`（默认） | ASCII + 拉丁补充 + 标点 + 常用汉字 |

全平面 CJK 不要直接扫：体积与耗时都会爆。

---

## 2. 批量多字号：`gen_range.py`

按**已有 `.ef` 的码点表**作字集（默认 `misans/misans_25_1.ef`，约 6006 字），从同一 TTF 批量出多字号，输出 `misans_{size}_{bpp}.ef`。

```bash
cd tools/epdfont

# 改成你的 MiSans 路径
TTF=/path/to/MiSans-Regular.ttf

# 1bpp：10～40 px
.venv/bin/python gen_range.py \
  --ttf "$TTF" \
  --from-ef misans/misans_25_1.ef \
  --sizes 10-40 --bpp 1 --jobs 6 \
  --out-dir misans

# 2bpp：同上
.venv/bin/python gen_range.py \
  --ttf "$TTF" \
  --from-ef misans/misans_25_1.ef \
  --sizes 10-40 --bpp 2 --jobs 6 \
  --out-dir misans
```

常用参数：

| 参数 | 默认 | 说明 |
|------|------|------|
| `--ttf` | （脚本内示例路径） | 源字体，**必须改成你机器上的路径** |
| `--from-ef` | `misans/misans_25_1.ef` | 字集模板；没有则先用 `fontconvert` 做一份 |
| `--sizes` | `10-40` | 如 `25`、`20,25,30`、`10-40` |
| `--bpp` | `1` | `1` 或 `2` |
| `--jobs` | `6` | 并行进程数 |
| `--out-dir` | `misans/` | 输出目录 |
| `--prefix` | `misans` | 文件名前缀 |

首次若没有模板 `.ef`：先 `fontconvert --from-ttf ... --intervals reading_zh` 生成 `misans_25_1.ef`，再跑 `gen_range`。

---

## 3. 拷到 SD 卡

```
SD 卡/
└── fonts/
    ├── misans_25_1.ef    ← 阅读页默认
    ├── misans_25_2.ef    ← 可选
    └── …
```

挂载点一般为 `/sdcard`。换字号/粗细只需换文件，**不用重刷固件**（固件路径仍指向默认文件名时，可改名覆盖，或改 `BookScreen` 里的路径宏）。

---

## 目录结构

```
tools/epdfont/
├── README.md           ← 本文件
├── requirements.txt
├── fontconvert.py      ← 单文件：.c / .ttf → .ef
├── gen_range.py        ← 批量多字号
├── misans/             ← 生成物（可提交样例 .ef，或本地保留）
└── .venv/              ← 本地虚拟环境（忽略）
```

---

## 故障排查

| 现象 | 处理 |
|------|------|
| `freetype` ImportError | `.venv/bin/pip install -r requirements.txt` |
| `TTF not found` | 检查 `--ttf` 绝对路径 |
| `charset .ef not found` | 先造模板 `.ef`，或改 `--from-ef` |
| 设备打开失败回退 Flash 字 | 确认 SD 有 `/sdcard/metalio/e-ink/fonts/misans_25_2.ef`，且文件完整 |
| 字缺字/方框 | 字集未覆盖该码点；扩大 `--intervals` 或换更大的 `--from-ef` 模板 |

更细的格式、RAM、与 `.bin`/Flash `.c` 对比见文档：`.cursor/docs/epdfont-SD轻量字体.md`。

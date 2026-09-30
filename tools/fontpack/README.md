# fonts.fontpack 工具（EFNT）

多字号 × 多 BPP 打成单文件 `fonts.fontpack`，设备端可：

- SD：文件二分 + 单字拷贝
- Flash：`font_data` 分区 mmap 零拷贝

单字号 `.ef`（EPDFONT）在 **`tools/epdfont/`**，勿与本目录混淆。

---

## 环境

```bash
cd tools/fontpack
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

依赖：`freetype-py`、`fonttools`（打包时复用 `../epdfont/fontconvert.py` 的字集解析）。  
系统需有 FreeType（WSL：`sudo apt install libfreetype6`）。

Windows 字体 WSL 路径示例：`F:\MiSans\ttf\MiSans-Regular.ttf` → `/mnt/f/MiSans/ttf/MiSans-Regular.ttf`。

---

## 二进制格式

```
Header 32B:  magic="EFNT" | version:u16 | flags:u16 | total_items:u32
             | index_offset:u64 | data_offset:u64 | reserved:u32
Index  16B×N（按 Key 升序）:
             key:u64 = (unicode<<32)|(size<<16)|bpp
             | data_offset:u32 | data_size:u16 | reserved:u16
Data:        width:u16 height:u16 x_offset:i16 y_offset:i16 advance:u16
             | bitmap[]（MSB-first 紧凑，无行填充）
```

---

## 打包

**生产默认（含 Emoji + Math）：**

```bash
cd tools/fontpack
./build_misans_mixed.sh
```

产出 `fonts_misans_25_30.fontpack`（汉字 + Noto Emoji 30@2 + Latin Modern Math 18/28/36@2），并同步 `fonts.fontpack` 符号链接。**不要**再单独烧录无 emoji/math 包。

`.ef` 字集更新后（如 `extend_whitespace.py`），先重打 `.ef` 再跑上脚本。

### 手动 / 其他字重包

```bash
cd tools/fontpack

# 单字重示例（若需自定义包，仍建议最后 merge emoji）
.venv/bin/python build_fontpack.py \
  --ttf ../ttf-fonts/MiSans/MiSans-Semibold.ttf \
  --from-ef ../epdfont/MiSans-Light/misans_25_2.ef \
  --variants 25:1,30:1,30:2 \
  -o fonts/MiSans-Semibold/fonts_misans_25_30.fontpack --verify

.venv/bin/python merge_emoji_into_fontpack.py \
  --base fonts/MiSans-Semibold/fonts_misans_25_30.fontpack \
  -o fonts/MiSans-Semibold/fonts_misans_25_30.fontpack --verify
```

| 参数 | 默认 | 说明 |
|------|------|------|
| `--ttf` | `/mnt/f/MiSans/ttf/MiSans-Regular.ttf` | 单一源字体（与 `--variants` 联用） |
| `--sizes` | `16,20,24,32` | 字号（与 `--bpps` 笛卡尔积） |
| `--bpps` | `1,2` | BPP |
| `--variants` | — | 精确规格，如 `25:1,30:1,30:2` |
| `--variant-map` | — | 多字重：`size:bpp=/path.ttf,...`（优先，可省略 `--ttf`） |
| `--from-ef` | — | 从 `.ef` 取码点（推荐 `../epdfont/misans/...`） |
| `--chars-file` | — | UTF-8 字表 |
| `--intervals` | `reading_zh` | 无上两者时用预设区间 |
| `-o` | `fonts.fontpack` | 输出路径 |

当前产物目录（细节见各目录 `README.md`）：
- `fonts/MiSans-Mixed/` — **综合**；**25@2 Light / 30@2 Light / 30@4 Semibold**（烧录默认）
- `fonts/MiSans-Thin/` — Thin；**25@2 / 30@2+4**
- `fonts/MiSans-ExtraLight/` — ExtraLight；**25@2 / 30@2+4**
- `fonts/MiSans-Light/` — Light；**25@2 / 30@2+4**
- `fonts/MiSans-Normal/` — Normal；**25@2 / 30@2+4**
- `fonts/MiSans-Semibold/` — Semibold；**25@1 / 30@1+2**

根目录 `fonts_misans_25_30.fontpack` / `fonts.fontpack` 为 **MiSans-Mixed + Emoji** 烧录默认包（`./build_misans_mixed.sh`）。

### 方式 1：SD 卡

拷到 `/sdcard/metalio/e-ink/fonts/fonts.fontpack`（或任意路径，调用时传入）。

### 方式 2：Flash 分区 mmap

- `partitions/v2/16m_fontpack.csv` — 16MB：`font_data` 6MB + `assets` 2MB  
- `partitions/v2/32m_fontpack.csv` — 32MB：`font_data` 6MB + `assets` 10MB  

```bash
# sdkconfig: CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/v2/16m_fontpack.csv"
idf.py partition-table-flash
./flash_fontpack.sh /dev/ttyACM0 ./fonts.fontpack
```

---

## ESP32 检索库

`esp32/font_loader.h` / `font_loader.c`（示例 `esp32/example_usage.c`）。

```c
font_loader_init_flash(NULL);  // 分区 label 默认 "font_data"
GlyphInfo g;
if (get_glyph(U'你', 30, 2, &g)) { /* g.bitmap → mmap Flash */ }
font_loader_deinit();
```

SD：`font_loader_init("/sdcard/metalio/e-ink/fonts/fonts.fontpack")` + `get_glyph_from_sd(...)`。

---

## 目录结构

```
tools/fontpack/          ← 仅 EFNT / fonts.fontpack
├── build_fontpack.py
├── flash_fontpack.sh
├── charset_smoke.txt
├── esp32/font_loader.*
└── requirements.txt

tools/epdfont/           ← .ef 转换与 misans/*.ef 产物
├── fontconvert.py
├── gen_range.py
└── misans/
```

---

## 故障排查

| 现象 | 处理 |
|------|------|
| `freetype` ImportError | `.venv/bin/pip install -r requirements.txt` |
| `TTF not found` | 检查 WSL 路径 `/mnt/f/...` |
| `charset .ef not found` | `--from-ef ../epdfont/misans/misans_25_1.ef` |
| fontpack Key 未找到 | 确认打包时的 size/bpp 与查询一致 |
| `FONT_LOADER_ERR_TOO_LARGE` | 增大 `FONT_GLYPH_BMP_MAX` |

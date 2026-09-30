# A2UI-lite — LVGL renderer for e-paper (transport-agnostic)

源码在 `main/display/a2ui/`，随 main 编译（不是独立 IDF component）。

Feed JSON with `a2ui_handle_json()`. Optional host via `a2ui_set_host()`.

**Does not flush the EPD** — leave refresh to the app's existing partial/full path.
Caller must hold the LVGL lock when calling `a2ui_handle_json`.

```c
#include "a2ui.h"

a2ui_config_t cfg = A2UI_CONFIG_DEFAULT();
cfg.disp = disp;
cfg.font_regular = &font;   /* e.g. fontpack 30@2 */
cfg.font_bold = &font_bold; /* e.g. fontpack 30@4 — 加粗用更高 bpp */
ESP_ERROR_CHECK(a2ui_init(&cfg));
a2ui_set_host(host_obj);
a2ui_handle_json(json);  /* under LVGL lock */
```

## 支持的组件（摘要）

| `component` | 说明 |
|-------------|------|
| `Column` / `Row` / `Card` | 布局 |
| `Text` / `RichText` | 纯文本 / 粗细 span |
| **`Math` / `Formula`** | **设备端 LaTeX 子集公式排版**（见下节） |
| `Image` | A2I1 异步图 |
| `Header` / `Badge` / `ListItem` / `Status` / `Progress` / `Button` / `Divider` / `Spacer` | 其它 UI |

---

## Math（设备端 LaTeX 公式）

服务端在 `updateComponents` 里下发 **LaTeX 字符串**，设备解析子集后用 **TeX 盒模型** 排版，再以 LVGL 绝对定位绘制。  
**不是**把公式渲成图再传（那是 `Image`+A2I1）；也不是完整 TeX 引擎。

实现：`a2ui_math.c` / `a2ui_math.h`；百问助手分页会把公式当**整块**占位（不按行切开）。

### 组件字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `component` | `"Math"` 或 `"Formula"` | 二者等价 |
| `latex` | string | LaTeX 正文；也可用 `text` |
| `display` | bool | `true` 为展示式（大运算符等） |
| `width` | number | 最大宽度提示（公式本身不折行） |

外层可带或不带 `$...$` / `$$...$$` / `\(...\)` / `\[...\]`。

### 最小示例（斯特林公式）

```json
{
  "updateComponents": {
    "root": "root",
    "components": [
      {
        "id": "root",
        "component": "Column",
        "gap": 12,
        "padding": 8,
        "children": ["title", "stirling"]
      },
      {
        "id": "title",
        "component": "Text",
        "text": "斯特林公式",
        "variant": "title",
        "bold": true
      },
      {
        "id": "stirling",
        "component": "Math",
        "display": true,
        "width": 440,
        "latex": "$n! \\approx \\sqrt{2\\pi n} \\left( \\frac{n}{\\rm e} \\right)^n$"
      }
    ]
  }
}
```

更多样例见下节「测试样例」。

### 支持的 LaTeX 子集（常用）

| 类别 | 示例 |
|------|------|
| 原子 | 字母（数学斜体）、数字、`+-*/=<>!()[]!` |
| 分式 / 根号 | `\frac{a}{b}`、`\sqrt{x}`、`\sqrt[n]{x}` |
| 定界符 | `\left(` `\right)` / `[]` / `\{` `\}` / `.` |
| 上下标 | `^` `_`（可带 `{}`） |
| 符号 | `\approx \pm \times \cdot \leq \geq \neq \infty \sum \prod \int \partial` 及希腊字母 |
| 字体切换 | `\rm` / `\mathrm{}` / `\text{}` 直立；`\mathbf{}` 粗体 |
| 间距 | `\,` `\;` `\!` `\quad` `\qquad` |

未支持的完整宏包 / 矩阵环境等：解析失败时回退为原文 Label。

### 排版与大括号怎么来的

- **普通符号 / 斜体字母 / 希腊字母 / 运算符**：Latin Modern Math **字库字形**
- **基线**：`lv_label` 按字体 ascent 对齐到数学基线（同一行 `n` 与 `!` 共基线）
- **可伸缩括号**：内容矮 → 整字 `(`/`)`/`{`/`}`；内容高 → Unicode 零件竖拼（如 `⎛⎜⎝` / `⎧⎨⎩`）
- **分数线**：画在分式数学轴；分子/分母与线保留间隙
- **根号**：`√` 与正文共基线 + 顶部横线（绘制）

### 字体与打包

| 项 | 值 |
|----|-----|
| 字体 | **Latin Modern Math**（TeX 风格） |
| 源文件 | `tools/ttf-fonts/LatinModernMath/latinmodern-math.otf` |
| fontpack 字号 | **18 / 28 / 36 @ 2bpp**（脚本 / 正文 / 展示；与 UI 的 25/30 错开） |
| 生产脚本 | `tools/fontpack/build_misans_mixed.sh`（汉字 → Emoji → Math） |
| 烧录默认 | `use_font/font.fontpack`（`idf.py flash` / `merge_firmware.sh`） |

仅合并数学字形（已有汉字+Emoji 包时）：

```bash
cd tools/fontpack
.venv/bin/python merge_math_into_fontpack.py \
  --base fonts/MiSans-Mixed/fonts_misans_25_30.fontpack \
  --math-ttf ../ttf-fonts/LatinModernMath/latinmodern-math.otf \
  -o fonts/MiSans-Mixed/fonts_misans_25_30.fontpack --verify
cp -f fonts/MiSans-Mixed/fonts_misans_25_30.fontpack ../../use_font/font.fontpack
```

**注意**：设备必须刷过含 Math 的 fontpack，否则公式缺字/回退难看。

### 测试样例

| 文件 | 说明 |
|------|------|
| [`examples/math_formulas.json`](examples/math_formulas.json) | 斯特林 / 求根 / 质能 / 积分 / 大括号等联调 JSON |
| [`examples/math_generate_dialog.json`](examples/math_generate_dialog.json) | **模拟对话**：用户「生成一个正态分布概率密度公式」→ 助手回复杂 Math（分式/根号/希腊字母/复合指数） |

下发方式：百问会话 `updateComponents`，或直接 `a2ui_handle_json(...)`（须持 LVGL lock；需已刷含 Latin Modern Math 的 `use_font/font.fontpack`）。

#### 对话生成公式（服务端 / 模型约定）

用户自然语言要公式时，助手**不要**只回纯文本 LaTeX，应下发 A2UI：

1. 用户：`生成一个正态分布概率密度公式`（或任意「生成一个 xx 公式」）
2. 助手：回复（或会话消息附带）形如：

```json
{
  "updateComponents": {
    "root": "root",
    "refresh": "partial",
    "components": [
      {
        "id": "root",
        "component": "Column",
        "gap": 10,
        "padding": 8,
        "align": "center",
        "children": ["intro", "formula"]
      },
      {
        "id": "intro",
        "component": "Text",
        "text": "好的，一维正态（高斯）概率密度如下：",
        "variant": "body"
      },
      {
        "id": "formula",
        "component": "Math",
        "display": true,
        "width": 440,
        "latex": "$f(x) = \\frac{1}{\\sigma\\sqrt{2\\pi}}\\,\\mathrm{e}^{-\\frac{(x-\\mu)^{2}}{2\\sigma^{2}}}$"
      }
    ]
  }
}
```

完整带「用户 / 助手」角色文案的联调样例见 [`math_generate_dialog.json`](examples/math_generate_dialog.json)（文件内可选 `dialog` 字段仅作说明；设备侧以 `updateComponents` 为准）。

---

## Image（A2I1）

设备只收 **A2I1** 二进制（服务端已二值化）。布局见 `tools/a2i1/README.md`；与本地关机资源 `bg_shutdown.a2i1` 同一格式。

同一 `updateComponents` 可含多张 Image：每张图独立 surface（像素缓冲），由 **单 worker + FIFO** 依次拉缓存/HTTP 上屏；翻页仍 `abort → 毁控件 → clear`。硬上限见 `A2UI_IMG_MAX_SURFACES`（默认 3，内存保护，不是协议「一图一更新」）。单次下载峰值见 `A2UI_IMG_MAX_DOWNLOAD`（64KB，覆盖全屏 800×480 I1）。

## Image 会话缓存（SD）

| 层 | 职责 |
|----|------|
| `a2ui_img_cache` | 仅存储：有 SD 则建目录 / 开机清空 / URL→blob；无 SD 则禁用 |
| `a2ui_image` | 解析 URL → 查缓存 → HTTP → 上屏 → 延迟后 best-effort 写入；多 surface + FIFO |
| `assistant_screen` | 只拼 JSON / 分页；不直接碰存储 |

路径约定见 `boards/common/sd_paths.h`：`SD_PATH_A2UI_CACHE` = `/sdcard/metalio/e-ink/a2ui_cache`。

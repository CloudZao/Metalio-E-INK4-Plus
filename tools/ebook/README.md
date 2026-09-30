# ebook Studio（`tools/ebook`）

ESP32 友好的 `.ebook` **转换 + Web 阅读**工具集。

## 目录

| 路径 | 说明 |
|------|------|
| `epdbook/` | 格式库（writer / reader / extractors） |
| `convert_ebook.py` | CLI 转换 |
| `web/` | Web：上传转换 + 在线阅读（亮/暗主题） |
| `SPEC.md` | 二进制规范 |
| `test-book/` | 测试样书 |
| `book/` | 额外样书（可选） |
| `data/library/` | Web 转换产物 |
| `emulator/` | LVGL 墨水屏 WASM/SDL 模拟器（改阅读布局后在此 `./build_wasm.sh`） |

## Web（推荐）

```bash
cd tools/ebook
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
./run_web.sh
# 打开 http://127.0.0.1:8765
# 端口被占用时会自动顺延；或: EBOOK_PORT=9000 ./run_web.sh
# 强制结束旧实例: ./run_web.sh --kill
```

功能：上传 PDF/EPUB/MOBI/TXT → `.ebook`；书库（可直接打开已有 `.ebook` 阅读）；在线阅读（封面/目录/图文）；下载。

### Windows 单文件 exe（双击即用）

在 Windows（或 WSL 调用 Windows Python）打包：

```bat
cd tools\ebook
build_exe.bat
```

或 WSL：

```bash
cd tools/ebook
chmod +x build_exe.sh
./build_exe.sh
```

产物：`dist/ebook-web.exe`（约 90MB+，含 31 份 MiSans-Light `.ef` 字体）。双击后会启动本地服务并自动打开浏览器；**关闭黑色控制台窗口即停止服务**。书库写在 exe 同目录的 `data/` 下。

打包前会自动把 `tools/epdfont/MiSans-Light/*.ef` 复制到 `web/static/fonts/ef/` 一并打进 exe。

WSL 下执行 `./build_exe.sh` 会先同步到 `C:\Users\<你>\ebook-web-build` 再打包，完成后拷回本目录 `dist/`。

可选环境变量：`EBOOK_PORT`、`EBOOK_HOST`、`EBOOK_NO_BROWSER=1`（禁止自动开浏览器）。

### TXT 切章

转换时按行识别章节标题（与固件 `txt_chapter` 对齐并扩展），例如：

- `第N章/回/节/集/话/篇`、`第N卷/部`、`卷N`
- `序章` / `楔子` / `番外` / `前言` / `后记` / `尾声`…
- `Chapter 1` / `Part 2` / `Volume 3`
- 数字短章名（如 `11清和宫上`，对齐固件）

识别到 ≥2 章时置 `HAS_TOC`。可用 `--chapter-regex` 覆盖默认规则，`--no-split` 整书单章。

超大章（默认单章解压合计 >256KB，含图）会再按体积拆成 `原名 (1/N)`…，**不**因此置 `HAS_TOC`；设备可跨章翻页。`--no-chapter-size-split` 可关闭（不推荐）。固件硬拒 >384KB 的章。

```bash
.venv/bin/python convert_ebook.py novel.txt -o novel.ebook --inspect
```

### PDF 扫描件

Web 勾选「整页转图片」，或 CLI：

```bash
.venv/bin/python convert_ebook.py scan.pdf -o scan.ebook \
  --pdf-pages-as-images \
  --pdf-page-scale 1.5 \
  --pdf-page-max 480x720 \
  --binarize otsu
```

- `--pdf-page-scale`：渲染倍率（相对 72dpi）
- `--pdf-page-max`：编码前最大框（默认 **480×720**，阅读区全宽）；写出 Header `PAGE_IMAGES`，设备从显示区 `(0,0)` 铺满
- `--image-max`：插图最大框；`orig` 表示保留原始尺寸/比例（仍受 chunk 预算缩小）
- 有插图/扫描页时会自动抬高 `chunk_max`（≤49152，与固件一致）
- Web 书库可「清空书库」一键删除全部产物

### 二值化（A2I1）

墨水屏插图/页图默认 **Otsu** 自适应阈值；可选：

| 方法 | CLI | 适用 |
|------|-----|------|
| Otsu | `--binarize otsu` | 漫画线稿、多数彩插（默认） |
| Bayer | `--binarize bayer` | **与设备原生 EPUB 同款**（4×4 有序抖动） |
| 抖动 | `--binarize dither` | 照片、水彩、渐变（Floyd–Steinberg） |
| Sauvola | `--binarize sauvola` | 底色不匀的扫描件 |
| 固定阈值 | `--binarize fixed --threshold 128` | 手动微调 |

另可调 `--contrast`、`--sauvola-window`、`--sauvola-k`。Web：选 **PDF / EPUB** 后展开「二值化预览」，调参预览满意再转换（PDF 按页，EPUB 按插图顺序）。

### 保留彩色（不二值化）

勾选 Web「保留彩色」，或 CLI `--keep-color`：

```bash
.venv/bin/python convert_ebook.py book.epub -o out.ebook --keep-color
```

- 插图/封面以 **JPEG** 写入（非 A2I1），体积较大，仍受 chunk / 封面预算约束
- **设备阅读**时按原生 EPUB 同款路径：`DecodeImageToL8` → RGB 加权灰度 + Bayer 4×4 抖动
- 与 `--binarize` 互斥；需要转换侧控制二值化效果时请用默认 A2I1 流程

## CLI

```bash
.venv/bin/python convert_ebook.py "test-book/Node即学即用.pdf" -o out.ebook --inspect
```

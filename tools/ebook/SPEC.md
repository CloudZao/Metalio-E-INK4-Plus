# `.ebook` 格式规范 v1

面向 **ESP32-S3** 墨水屏阅读器的极简电子书容器。  
设计目标：流式读盘、分块解压、章节 `fseek` 瞬跳，运行时峰值 RAM **&lt; 64KB**（建议工作集约 8–16KB）。

**扩展名**：`.ebook`  
**字节序**：小端（little-endian）  
**字符集**：UTF-8（无 BOM）  
**压缩**：每块独立 zlib deflate（raw zlib 包装，即 `zlib.compress` / `inflate`）

---

## 1. 设计原则

| 原则 | 说明 |
|------|------|
| 排版极简 | 不存字体、字号、颜色、行距、页眉页脚；设备用固定 `.ef` 渲染 |
| 分块流式 | 文本按 `chunk_max_uncomp`（默认 4096）切块，单块解压 |
| 章节可跳 | 索引表给出每章首块绝对偏移，无需扫全文 |
| 图文同流 | 章内按源顺序交替出现 TEXT / IMAGE 块 |
| 固定字号 | Header 仅存 `default_font_px` 提示；真实字体在设备 SD |

控制字符（仅出现在 TEXT 解压后负载中）：

| 字节 | 名称 | 含义 |
|------|------|------|
| `0x1E` | RS | 段落结束（新段） |
| `0x0A` | LF | 段内软换行（同段多行）；段间空行不写入 |
| 其它 | — | 合法 UTF-8 正文；禁止嵌入 `0x00` |

段首缩进由阅读端统一处理：**转换时剥掉每段/每行源文前导空白**，不在 `.ebook` 中写入段首空格；Web / ESP32 用 `text-indent: 2em` 或等效 `pad_left` 两字宽。

**分段（写入 `0x1E`）**：

- 源文**空行** → 段落边界；空行之间若有多行且无段首空白，合并为一段（段内 `LF` 软换行）。
- 源文**连续非空行且行首有空白**（网文常见 `U+3000`/空格）→ **一行一段**，各行前导空白剥离。
- 源文**连续非空行且行首顶格** → 合并为一段，行间 `LF` 连接（软换行）。

**段间距**：源文空行仅用于分段（写入 `0x1E` 分隔）；**不**将空行编码为段首 `LF` 或单独占位行。段与段之间由阅读端固定段距（设备 `kParaGap`、Web 段落 `margin`）体现。

---

## 2. 文件总布局

```
+------------------+  offset 0
| Header (64 B)    |  固定长度
+------------------+  meta_offset
| Metadata         |  标题/作者等
+------------------+  index_offset
| Chapter Index    |  chapter_count × 32 B
+------------------+  index_offset + index_size
| Title Blob       |  各章标题 UTF-8 紧挨排列
+------------------+  data_offset
| Data Blocks…     |  流式块序列（可跨章连续）
+------------------+
| [可选 Cover]     |  cover_offset 指向独立封面块载荷
+------------------+
```

封面也可作为第 0 章前的 IMAGE 块；`cover_*` 字段便于详情页不扫数据区。

---

## 3. Header（64 字节）

| 偏移 | 长度 | 类型 | 字段 | 说明 |
|------|------|------|------|------|
| 0 | 4 | char[4] | `magic` | `"EBOK"` (`0x45 0x42 0x4F 0x4B`) |
| 4 | 2 | u16 | `version` | 当前为 `1` |
| 6 | 2 | u16 | `flags` | 见 §3.1 |
| 8 | 4 | u32 | `chapter_count` | 章节数 ≥ 1；**转换工具不写入空章**（无非空白文本且无图） |
| 12 | 2 | u16 | `default_font_px` | 设备提示字号（如 25）；**不嵌入字体** |
| 14 | 2 | u16 | `chunk_max_uncomp` | 单块解压上限，默认 `4096` |
| 16 | 4 | u32 | `meta_offset` | Metadata 绝对偏移 |
| 20 | 4 | u32 | `meta_size` | Metadata 字节数 |
| 24 | 4 | u32 | `index_offset` | Chapter Index 绝对偏移 |
| 28 | 4 | u32 | `index_size` | `chapter_count * 32` |
| 32 | 4 | u32 | `title_blob_offset` | 章标题串起始 |
| 36 | 4 | u32 | `title_blob_size` | 章标题串总长 |
| 40 | 4 | u32 | `data_offset` | 首个 Data Block 偏移 |
| 44 | 4 | u32 | `data_size` | 数据区总字节（不含封面独立区时可为整段） |
| 48 | 4 | u32 | `cover_offset` | 0 = 无独立封面 |
| 52 | 4 | u32 | `cover_size` | 封面载荷长度（含 ImagePayload 头） |
| 56 | 4 | u32 | `crc32` | 对 Header 前 56 字节的 CRC-32；校验可选 |
| 60 | 4 | u32 | `reserved` | 必须为 0 |

### 3.1 flags

| Bit | 名 | 含义 |
|-----|----|------|
| 0 | `HAS_COVER` | `cover_offset != 0` |
| 1 | `IMG_A2I1` | 图片优先为 A2I1（墨水屏 1bpp） |
| 2 | `ZLIB` | 块载荷使用 zlib（v1 必须置 1） |
| 3 | `HAS_TOC` | 含可用目录：PDF Outline / EPUB nav·NCX，或 **TXT 启发式切章成功（≥2 章）**；未置位时显示「暂无目录」。**按体积强制拆章不置本标志** |
| 4 | `PAGE_IMAGES` | 整页转图书（`--pdf-pages-as-images`）：阅读端从显示区 `(0,0)` 全屏铺满，无左右边距；每页一图 |
| 5–15 | — | 保留，写 0 |

**转换：超大章拆分**：写出前若单章解压合计（文本 UTF-8+`0x1E` + ImagePayload）超过 `chapter_max_bytes`（默认 **256KB**），按段落/块边界拆成多章，标题形如 `原名 (1/N)`。须严格小于固件硬上限 **384KB**（`DEVICE_CHAPTER_MAX_UNCOMP` / `kMaxChapterUncompForPaginate`）；可用 `--no-chapter-size-split` 关闭（调试）。已转换旧书不受影响，需重转才拆章。

---

## 4. Metadata

变长，紧跟 Header（通常 `meta_offset == 64`）：

| 字段 | 类型 | 说明 |
|------|------|------|
| `title_len` | u16 | |
| `title` | utf8×N | 书名 |
| `author_len` | u16 | |
| `author` | utf8×N | 作者，可空 |
| `lang_len` | u16 | |
| `lang` | utf8×N | 如 `zh` / `en`，可空 |
| `extra_len` | u16 | |
| `extra` | utf8×N | 可选 JSON 扩展。转换工具约定字段：`book_id`（字符串书籍 ID，默认 uuid4 去横线）、`source`、`file`、`has_toc`、`pages_as_images`、`binarize`、`keep_color`、`image_mode` 等。未知键忽略。无 `book_id` 的旧书视为空 ID，不损坏。 |

无对齐填充。

---

## 5. Chapter Index

每个章节 **固定 32 字节**：

| 偏移 | 长度 | 类型 | 字段 | 说明 |
|------|------|------|------|------|
| 0 | 4 | u32 | `data_offset` | 本章**第一个** Data Block 的绝对文件偏移 |
| 4 | 4 | u32 | `data_length` | 本章所有块在文件中占用的字节总和 |
| 8 | 4 | u32 | `uncompressed_total` | 本章解压后文本+图元数据合计（统计用） |
| 12 | 2 | u16 | `block_count` | 本章块数 |
| 14 | 2 | u16 | `title_len` | 标题字节数 |
| 16 | 4 | u32 | `title_offset` | 标题在文件中的绝对偏移（Title Blob 内） |
| 20 | 4 | u32 | `flags` | bit0=章内含图；其余 0 |
| 24 | 8 | u8[8] | `reserved` | 0 |

设备打开目录：读 Index → 按 `title_offset/title_len` 读标题。  
跳章：`fseek(data_offset)` → 顺序读 `block_count` 个块。  
阅读端（Web / ESP32）对旧文件中的空章（仅空白/`0x1E`）在目录与翻章中跳过，不展示「本章无内容」。

---

## 6. Data Block

每块固定 **8 字节头** + 载荷：

| 偏移 | 长度 | 类型 | 字段 | 说明 |
|------|------|------|------|------|
| 0 | 1 | u8 | `type` | `0x01` TEXT / `0x02` IMAGE |
| 1 | 1 | u8 | `bflags` | bit0=zlib 压缩（与全局 flags 一致时置 1） |
| 2 | 2 | u16 | `comp_size` | 磁盘载荷长度 |
| 4 | 2 | u16 | `raw_size` | 解压后长度；**必须 ≤ `chunk_max_uncomp`** |
| 6 | 2 | u16 | `reserved` | 0 |
| 8 | … | u8[] | `payload` | `comp_size` 字节 |

未压缩时：`comp_size == raw_size`，`bflags.bit0 = 0`（v1 工具默认始终压缩文本；小图可按策略不压）。

### 6.1 TEXT 载荷（解压后）

纯 UTF-8 + `0x1E` 段分隔。段内可含普通空格与 `\\n`（软换行），**读写端不得 strip / 折叠空白**。示例：

```
第一段第一行\n第一段第二行\x1E第二段文字\x1E
```

切块规则：尽量在 `0x1E` 或 UTF-8 码点边界切开；单段超过 `chunk_max_uncomp` 时按码点硬切。  
设备与 Web 阅读器按 `\\n` 折行，按空格正常占位。

**扫描 PDF（整页图）**：转换工具 `--pdf-pages-as-images` 将每页光栅化为 IMAGE 块，并置 Header `PAGE_IMAGES`；默认页图框 **480×720**（铺满阅读区全宽）；`chunk_max` 可升至设备硬上限 **49152**（与固件 `kMaxChunk` 一致，可容纳 480×720 A2I1）。页图另受 `--pdf-page-max` / `--pdf-page-scale` 约束。插图框 `--image-max` 亦支持 `orig`（不按最大框预缩小，仅受 chunk 预算约束，比例保持原图）。设备 / Web 模拟器对 `PAGE_IMAGES` 书：正文区无左右边距，图从 `(0,0)` 起按视口缩放显示。

**A2I1 二值化（仅转换侧，默认）**：写出仍为 `fmt=3` A2I1；可选方法 `otsu`（默认）/ `bayer`（与固件 `GrayToL8Dither` 同构）/ `dither` / `sauvola` / `fixed`，以及对比度、固定阈值、Sauvola 窗口与 k。Web 可对 **PDF 页** 与 **EPUB 插图** 做「原色 | 灰度 | 二值」预览后再转换。阅读端（Web / ESP32）解码逻辑不变。

**保留彩色（`--keep-color`）**：插图/封面以 **JPEG**（`fmt=1`）写入，转换侧不二值化；Header **不**置 `IMG_A2I1`。设备阅读时经 `DecodeImageToL8`：RGB 加权灰度 + 4×4 Bayer 有序抖动 → L8，与原生 EPUB 插图路径一致。Web 阅读端按彩色展示。彩色 JPEG 体积大于 A2I1，仍受 `chunk_max` / 封面预算约束（自动降质量与缩小）。

### 6.2 IMAGE 载荷（解压后 = ImagePayload）

| 偏移 | 长度 | 类型 | 字段 |
|------|------|------|------|
| 0 | 1 | u8 | `fmt`：`1`=JPEG, `2`=PNG, `3`=A2I1, `4`=灰度 raw |
| 1 | 1 | u8 | `reserved` |
| 2 | 2 | u16 | `width` |
| 4 | 2 | u16 | `height` |
| 6 | 2 | u16 | `stride`（A2I1/raw 用；JPEG/PNG 为 0） |
| 8 | 4 | u32 | `data_len` |
| 12 | … | u8[] | 图像字节 |

**注意（分轨）**：
- **正文 IMAGE 块**：整块解压后 ImagePayload 必须 ≤ `chunk_max_uncomp`（翻页流式缓冲）。
- **独立封面**：`cover_offset` 指向 ImagePayload（**无** Data Block 头），预算与 chunk **无关**；转换工具默认 ≤ 24KB，设备 `kMaxCoverPayload` 同步。失败则 `cover_offset=0`（详情占位）。
- 墨水屏产品默认编码为 **A2I1**（`fmt=3`）；JPEG/PNG 仍可解码但非默认。
- **规范位置**：工具写出与补丁时保证 `cover_offset == data_offset + data_size`（紧贴 Data 区末尾）；阅读端按 Header 偏移读取即可。
- **自定义封面（工具侧）**：
  - 转换：`--cover IMAGE` / Web「自定义封面」注入或覆盖源封面（含 TXT 无封面场景）；与 `--no-cover` 互斥。
  - 已有文件：`set-cover` / `clear-cover`（或 Web `POST|DELETE /api/book/<id>/cover`）仅改尾部封面与 Header 的 `cover_*` / `HAS_COVER` / `crc32`，**不改**正文 Data、Index、`IMG_A2I1`。
  - 用户**显式**指定封面时，编码失败必须失败整次操作，禁止静默写成无封面；源文件自动提取失败仍可 soft-drop（与既有行为一致）。

大图转换阶段必须预缩放；正文图用 `prepare_inline` 压进 chunk，封面用 `prepare_cover` 压进封面预算。

独立封面便于详情页一次 `fseek` 读入，无需扫数据区。

---

## 7. ESP32 内存预算（参考）

| 缓冲 | 建议大小 | 用途 |
|------|----------|------|
| 块头 | 8 B | 栈上 |
| 压缩读缓冲 | ≤ `chunk_max_uncomp` | 读盘 |
| 解压缓冲 | ≤ `chunk_max_uncomp` | inflate |
| zlib 工作区 | ~6–12 KB | 视配置 |
| 行排版暂存 | ~1–2 KB | 当前页 |

默认 `chunk_max_uncomp = 4096` 时，峰值易控在 **32KB 以内**，留足 LVGL/系统余量。

**章级物化分页（当前固件）**：EPUB / `.ebook` 打开时仍会把**当前章**解成 `ContentBlock` 再折成全章 `pages_`。因此：

- 转换侧默认拆掉超大章（§3.1，256KB 文本）
- 固件拒绝 `uncompressed_total` > **384KB** 的章，并对分页 `bad_alloc` **软失败**（不 abort）

伪代码：

```c
fseek(fp, chapter[i].data_offset, SEEK_SET);
for (n = 0; n < chapter[i].block_count; n++) {
    read header(8);
    read payload(comp_size) into in_buf;
    if (bflags & 1) inflate(in_buf → out_buf, raw_size);
    if (type == TEXT) render_utf8(out_buf);
    else decode_image(out_buf);
}
```

---

## 8. CRC

`crc32` 使用 ISO-HDLC / zlib 多项式 `0xEDB88320`，初值 `0xFFFFFFFF`，结果取反。  
设备打开时校验 Header CRC；不匹配则拒绝打开（损坏/截断文件）。

---

## 9. 版本兼容

- `version == 1`：本文档。
- 未知 `flags` 高位应忽略。
- 未知 `type` 块：跳过 `comp_size` 字节继续。

---

## 10. 与本仓库阅读栈的关系

| 现有 | `.ebook` |
|------|----------|
| `.epub` / `.txt` + `.idx` | 预编译为单一 `.ebook`，设备侧更轻 |
| SD `.ef` 字体 | 仍由设备加载；本格式不嵌字体 |
| A2I1 | 推荐插图/封面格式（`tools/a2i1`） |

固件解析器可后续落在 `main/reader/`；本目录仅规范 + 桌面转换工具。

# ebook-emulator

本仓 Web / 桌面用的 **GDEM0397T81P 墨水屏 1:1 LVGL 阅读模拟器**（480×800）。

路径：`tools/ebook/emulator/`（请在此修改；勿再改外部 `/home/peng/lvgl-ui/ebook-emulator`）。

与 ESP32 工程共用阅读核心：

- `BookSession` / `EbookDocument`（`main/reader`）
- 与 `book_screen.cc` 对齐的排版：`.ef` 正文、段首两字宽缩进、`PAGE_IMAGES` 全屏铺满等
- 物理面板 800×480，固件 `ROTATE_270` → LVGL **480×800**

## WASM（嵌入 ebook Studio Web）

```bash
cd tools/ebook/emulator
source ~/emsdk/emsdk_env.sh
./build_wasm.sh
```

产物写入 `tools/ebook/web/static/wasm/ebook_emulator.{js,wasm}`。

## 原生 SDL（可选）

```bash
cd tools/ebook/emulator
cmake -B build-ebook -S .
make -C build-ebook ebook_sim -j

export EBOOK_EPDFONT=/path/to/misans_25_2.ef
export EBOOK_PATH=/path/to/book.ebook
./build-ebook/bin/ebook_sim -b sdl
```

## 目录

| 路径 | 说明 |
|------|------|
| `src/emulator/book_reader_emulator.cpp` | 阅读 UI（对齐 `main/.../book_screen.cc`） |
| `src/font/` | epdfont / fontpack 模拟后端 |
| `src/esp_compat/` | ESP API 桩 |
| `lvgl/` | vendored LVGL（无嵌套 git） |
| `build_wasm.sh` | Emscripten 一键打包 |

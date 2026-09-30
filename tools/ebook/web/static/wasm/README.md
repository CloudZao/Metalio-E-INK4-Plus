# WASM 构建产物

在本仓编译：

```bash
cd tools/ebook/emulator
source ~/emsdk/emsdk_env.sh
./build_wasm.sh
```

本目录生成：

- `ebook_emulator.js`
- `ebook_emulator.wasm`

Web 端通过 `/wasm/ebook_emulator.js` 加载 LVGL 墨水屏模拟器（480×800）。

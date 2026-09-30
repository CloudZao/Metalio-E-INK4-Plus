# A2I1 — 墨水屏 1bpp 图片工具

把彩色/灰度原图转成 **A2I1**（与 `main/display/a2ui` 网络图同一格式），纯 1bpp，扩展名 `.a2i1`。

面板参考：[GDEM0397T81P](../../datasheet/GDEM0397T81P.pdf) 3.97"，1bit 黑白，逻辑分辨率 **480×800**。

依赖：

```bash
pip install -r tools/a2i1/requirements.txt
```

## Windows GUI（推荐）

双击或运行：

```text
tools/a2i1/A2I1_GUI.exe
```

功能：

- 打开 `.a2i1` 预览，显示宽高 / stride / 文件大小 / 是否匹配面板
- 打开图片 → 阈值 / Floyd / 无抖动转换 → 另存为 `.a2i1`
- 按 GDEM0397T81P datasheet **复刻模组**：外形 56.24×96.62 mm、有效区 51.84×86.40 mm、节距 0.108 mm；竖屏 AA 480×800，图从 (0,0) 起绘；**默认 1:1**

源码运行：

```bash
python tools/a2i1/a2i1_gui.py
```

重新打包 exe（Windows）：

```bat
tools\a2i1\build_windows.bat
```

## 格式：A2I1

与 A2UI 服务端下发的图一致（`A2UI_I1_MAGIC`）。

| 偏移 | 长度 | 字段 |
|------|------|------|
| 0 | 4 | magic：`A2I1` |
| 4 | 2 | `width` |
| 6 | 2 | `height` |
| 8 | 2 | `stride`（通常 `ceil(width/8)`） |
| 10 | 2 | `reserved` = 0 |
| 12 | 8 | LVGL I1 调色板：index0=黑、index1=白（ARGB `A,R,G,B`） |
| 20 | `stride×height` | 位图：MSB 左；**1=白、0=黑** |

格式只存非黑即白位图；**是否抖动由转换参数决定**，不强制 Floyd。

## 转换 CLI

源图可放本目录；产物放到 `main/xingzhi-assets/` 根目录打包进 `resources`。

```bash
python3 tools/a2i1/jpg_to_a2i1.py tools/a2i1/bg_shutdown.png \
  -o main/xingzhi-assets/bg_shutdown.a2i1 \
  --preview /tmp/bg_shutdown_bw.png \
  --method threshold
```

| `--method` | 说明 |
|------------|------|
| `threshold`（默认） | 灰度 ≥ 128 → 白，线稿/插画 |
| `floyd` | Floyd–Steinberg，照片 |
| `none` | Pillow 无抖动量化 |

可选：`--size 480x800`、`--threshold 140`。

## 固件侧

- **A2UI**：HTTP/缓存直接按 A2I1 解析，`lv_image` 显示  
- **关机**（优先 SD，否则内置）：
  1. 若已挂载且存在 `/sdcard/metalio/e-ink/wallpaper/bg_shutdown.a2i1` → 用 SD  
  2. 否则用 `resources` 分区内置 `bg_shutdown.a2i1`  
  落墨：直写 EPD 帧缓冲后局刷

替换 SD 图：

```bash
python3 tools/a2i1/jpg_to_a2i1.py your.jpg -o bg_shutdown.a2i1 --method threshold
# 拷到卡上：/sdcard/metalio/e-ink/wallpaper/bg_shutdown.a2i1
```

# A2I1 图片转换

**中文** | [English](README.md)

将彩色或灰度图转换为 1 bpp `.a2i1`，可预览并保存黑白输出。GUI 预览参考 397 面板，其他设备须显式选择合适尺寸；图片转换不保证硬件显示效果。

## 环境与 GUI

在仓库根目录执行：

```sh
python -m pip install -r tools/a2i1/requirements.txt
python tools/a2i1/a2i1_gui.py
```

GUI 提供原图、阈值/Floyd/无抖动转换和 A2I1 预览，显示宽高、stride 与文件大小。若已有 Windows 打包程序可直接运行；重新打包使用 `tools/a2i1/build_windows.bat`，不把本地 exe 存在视为源码运行的前提。

## 命令行

```sh
python tools/a2i1/jpg_to_a2i1.py '<源图.jpg>'   -o '<输出.a2i1>' --size 480x800 --method floyd
```

`threshold` 按阈值二值化，`floyd` 使用 Floyd–Steinberg，`none` 使用无抖动量化；`--threshold` 设置阈值，`--preview` 输出预览 PNG。尺寸是示例，不作为 470 默认尺寸。输出路径由调用方指定，不覆盖原图。

## A2I1 格式

```text
0..3    magic A2I1
4..5    width:u16
6..7    height:u16
8..9    stride:u16
10..11  reserved:u16 = 0
12..19  ARGB palette: index 0 black, index 1 white
20..    stride * height bitmap bytes; MSB first, 1 white, 0 black
```

多字节整数为小端，stride 通常为宽度除以 8 向上取整。格式只存黑白结果，不强制采用某种抖动算法。

## 设备侧使用

设备解析由 [A2UI 图像实现](../../main/ui/common/drawing/a2ui/README.md)维护。壁纸安装与关机画面选择通过设备界面完成，路径见[SD 存储](../../docs/sd-storage_zn.md)。内置资源由所选板型的 assets 配置决定，不以转换输出目录推断所有板型的打包行为。保留原图和授权，避免把转换产物当作唯一源文件。

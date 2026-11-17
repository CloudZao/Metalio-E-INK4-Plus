# 简繁体 UI 与阅读扩展字库

[English](README.md)

## 安装

本目录维护独立的 SD 字库文件、说明、校验信息和原始授权。将 `metalio/e-ink/fonts/extended-fonts.fontpack` 复制到 SD 卡以下位置：

```text
metalio/e-ink/fonts/extended-fonts.fontpack
```

可以通过设备「传输」页面直接上传 `extended-fonts.fontpack`；开启自动分类后，`.fontpack` 与 `.ef` 一样保存到 `fonts/`。保持文件名不变；如已有同名文件，选择覆盖。上传完整保存后重新打开书籍。关闭自动分类时，需先进入 `fonts/` 再上传。

需要支持 UI 与指定 MiSans `.ef` 阅读字体缺字回退的固件。完整复制后安全弹出并重新挂载 SD 卡；文件变化不会主动触发重绘。保留 Flash 主字库，已有字形仍优先使用 Flash。此文件用于 UI 与指定阅读字体的缺字补充，不是阅读设置中可选择的 TTF、OTF 或 `.ef` 字体，也不是 Flash 升级镜像。

## 字符和规格

每种规格包含 104,774 个 Unicode 字符，其中 102,998 个为 Unicode 17.0 统一及兼容汉字，涵盖简体、繁体、基本区、扩展 A～J 区及兼容汉字区。另包含 MiSans 提供的常用标点、拉丁字母及符号。总字形条目为 838,192，文件大小为 255.42 MiB。

提供 `20:2`、`25:2`、`30:2`、`35:2`、`40:2`、`45:2`、`50:2` 和 `30:4`，分别表示像素字号和 bpp。阅读字体按 `misans_<字号>_2.ef` 的固定名称识别，字号限于上述七档；原 `.ef` 字形优先，缺字时查询相同规格的扩展字形，并保留原行高和基线。扩展不提供其他字号或自动缩放，不替代公式专用字体规格。

同一码点优先使用 MiSans，其次为 MiSans L3，最后为遍黑体。`30:4` 的 MiSans 字形使用 Semibold；MiSans L3 和遍黑体补充字形保留原字重，不代表所有生僻字均有独立粗体设计。

## 显示限制

字符覆盖不等于地区字形或异体序列覆盖：本文件不按简繁语言自动切换同一码点的大陆、台湾、香港字形，不处理 IVS、连字或多字符塑形，也不进行简繁转换。源字体的地区字形差异仍然存在。现有固定高度控件可能裁切个别较高字形；较多 SD 缺字会增加读取耗时。

## 来源与授权

使用小米 [MiSans 与 MiSans L3](https://hyperos.mi.com/font/download)、[遍黑体 V2.9.5795](https://github.com/Losketch/Plangothic-Project/releases/tag/V2.9.5795) 和 [Unicode 17.0 字符数据](https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt)。遍黑体基于思源黑体，采用大陆字形约定。

文件内为源字体渲染的位图，不包含源字体轮廓。随应用或设备分发时保留 `licenses/` 中的小米原始协议及遍黑体 SIL Open Font License；各来源继续适用其原始授权条款。

`manifest.json` 提供字形数量、源字体哈希和文件 SHA-256。校验值同时见 `SHA256SUMS.txt`。

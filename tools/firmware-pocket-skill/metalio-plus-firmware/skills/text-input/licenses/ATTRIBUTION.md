# 文字输入的数据来源与许可

适用表：`pinyin-4262-wordfreq311-v2`。只在工程确实采用本模块数据时加入对应说明。

## 固件帮助页的简短文字

可直接使用同目录 `help-notice.txt`。帮助页说明实际采用的来源；完整许可证、频率数据及修改说明放在随BIN交付的“文字输入来源与许可”目录，能由用户找到。仅写“仅供学习”不能替代许可要求。

## 数据与程序分别标识

| 实际内容 | 来源与版本 | 许可及随附文件 |
| --- | --- | --- |
| 拼音映射生成与默认读音 | pypinyin 0.55.0，mozillazg、闲耘；上游拼音数据pinyin-data | MIT；`pypinyin-0.55.0-MIT.txt`、`pinyin-data-MIT.txt` |
| 单字词条排序权重及由其形成的候选次序 | wordfreq 3.1.1 `large_zh.msgpack.gz`，Robyn Speer及其数据贡献来源 | CC-BY-SA-4.0；`CC-BY-SA-4.0.txt`、`wordfreq-3.1.1-data-notice.txt` |
| wordfreq程序声明 | 官方3.1.1 wheel的版权文本；本包不分发完整wordfreq程序 | Apache-2.0；`wordfreq-3.1.1-copyright.txt`、`Apache-2.0.txt` |
| 字表转存工具、最小查询/编辑核心与业务接线示例 | 本项目实现；外部数据仍按上列条件使用 | 自有部分随主Skill交付授权；不得将第三方数据改标为自有或统一标为MIT |

官方入口：

- https://github.com/mozillazg/python-pinyin/tree/v0.55.0
- https://github.com/mozillazg/pinyin-data
- https://pypi.org/project/wordfreq/3.1.1/
- https://creativecommons.org/licenses/by-sa/4.0/

固定下载地址、包SHA、实际数据成员SHA及许可文本来源见 `../generation/sources.json` 与 `../generation/generation-report.json`。pinyin-data附件取自其许可文件固定提交；实际映射输入以pypinyin 0.55.0轮子为准，不冒称取用了最新pinyin-data全部内容。

`wordfreq-3.1.1-data-notice.txt` 是从固定发行轮子的METADATA中原样提取的“License”及后续来源/论文声明，不冒充另一个版本的NOTICE文件。它包括Google Books、Leeds、Wikipedia、ParaCrawl、OpenSubtitles及SUBTLEX等来源；保留原声明，不推断每个来源对中文数据的贡献比例。SUBTLEX须署名且保持其可免费获取的说明；相关作者与论文已在该原始声明中保留，包括Cai、Brysbaert的SUBTLEX-CH。

## 本项目做了哪些改变

从wordfreq固定中文大表中选择4262个兼容字符的**精确单码点词条**；采用其centibel权重排序，同权重按码点；11个没有权重的字符保留在末尾。不聚合多字词、不作简繁映射、不引用或回退旧Jun Da排名。拼音采用默认读音加显式常用多音修正。对应输入、权重、生成报告随包提供。

本模块将频率派生数据（含排序TSV、权重表和生成头文件的数据部分）按CC-BY-SA-4.0提供，同时保留映射部分的MIT声明。继续改编或分发这些数据时保留署名、来源、许可链接及改动说明，按适用条件提供；这不表示整个应用或主Skill所有文件都自动变成同一许可证。具体固件组合交付及其他依赖由主Skill的合规说明一起核对。

执行 `table_tool.py --notices-dir <独立交付目录>` 会导出本目录全文、实际候选TSV、权重表、生成报告和来源清单。生成C++头文件也写有数据来源提示。若更换排序源或不用本模块，不应继续展示与实际产物不符的来源声明。

## 边界

此表不保证读音全部逐项人工校对，也不代表按读音统计的候选概率或手机输入法质量。保留旧字符集合只为兼容需求，不包含旧字体位图；新字形及其许可由字体模块独立处理。当前实现不包含云端输入服务或第三方商业输入法程序。

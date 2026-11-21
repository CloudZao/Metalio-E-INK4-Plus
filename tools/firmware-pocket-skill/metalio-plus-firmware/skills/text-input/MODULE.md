# Plus 文字输入

主 Skill 按需求读取本模块；只有显示、没有输入框时无需加载。提供固定拼音逐字选字、英文、数字、符号，不提供整句预测、学习或云端输入。V0.2 表版本为 `pinyin-4262-wordfreq311-v2`，有402音节、4262个不同汉字；它不是字体文件。

1. 从需求确定字段、单行/多行、光标、UTF-8字节上限和初始模式；网址可英数，正文可中文。不要把某记事本的3072字节、TF路径、TXT后缀或屏幕方向变成固定要求。
2. 读取 [接入与验收](references/接入与验收.md) 第1—4节。把已提交文本、待选拼音、候选页及已显示画面版本分开；候选只能选择用户已看到的版本。动态“字母/确认”键、光标点击同样检查显示代次。
3. 使用 [冻结词表](assets/pinyin-candidates.tsv)，不在上下文展开整表，不重新下载字典或凭模型重排候选。运行 [table_tool.py](scripts/table_tool.py) 校验并生成C++只读头文件。wordfreq数据仅在编写Skill时取料，普通用户生成应用不安装wordfreq或pypinyin。
4. 优先复用 [ime_core.h](assets/ime_core.h) 的查询、UTF-8编辑和候选版本检查；[input_flow.cc](examples/input_flow.cc) 演示最小接线，[可复用核心说明](references/可复用核心与来源.md) 解释显示确认和保存边界。无需采用特定UI框架、屏幕坐标或业务文件格式。宿主选择等价实现时保留相同可观察行为。
5. 接入应用的文本、字体、显示及 [Plus触摸规则](../device-interaction/references/touch-power.md)。短震优先由HAL新Down统一触发，输入法不重复Pulse；旧画面保护拒绝的选字不能伪造成功。
6. 按规范第5节验证UTF-8、上限、翻页、清空、模式切换、快速连点和弹窗。按 [署名与许可](licenses/ATTRIBUTION.md) 将简短来源说明接入帮助页，随交付保留许可证及对应数据；不以一句“仅供学习”替代适用许可。报告实际适配、源码/测试证据及真机待验项，交回主流程继续构建和BIN交付。

基础边界：成功插入后才清拼音；插入失败保留草稿/光标。清空只清拼音，有拼音时换行键先提交小写字母，无拼音才换行或单行确认。输入法不直接存文件、配网或返回口袋；这些由业务与集成模块负责。

```powershell
python "$TextModule\scripts\table_tool.py"
python "$TextModule\scripts\table_tool.py" --header "$Work\project\main\generated\pinyin_table.h"
python "$TextModule\scripts\table_tool.py" --notices-dir "$Work\delivery\文字输入来源与许可"
```

`$TextModule` 为本模块位置，`$Work` 为独立任务目录。默认校验随包TSV；`--table`仅用于校验同版本其他副本，任何字节变化均拒绝。生成头文件只含数据；需同时接入核心/控件才有输入行为，仍不等于固件已编译或真机键盘已验收。字体须覆盖全部候选，不因缺字偷偷删表。仅英数配网不必编入完整拼音表。

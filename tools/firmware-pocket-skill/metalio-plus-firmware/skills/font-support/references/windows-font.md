# Windows 字体接线

设`$Font`为本模块目录、`$Work`为当前工程。默认工程生成器已准备一套32px/500完整字体；不要再编入第二份相同数据。独立复用时：

```powershell
python -B "$Font\scripts\prepare_font.py" --size 32 --weight 500 --output "$Work\main\generated\body"
# 固定菜单才使用 --chars-file <UTF-8文案>；缺字报错，不丢弃请求字符。
```

复制`components/plus_font`至工程components；main的同一个`idf_component_register`加入`generated/body/font.cc`、依赖`plus_font`，保留原有项。`Font.Open(plus_font_data, plus_font_data_size)`只初始化一次；数组有效期覆盖全部绘制，勿复制完整字库到RAM。多规格用不同输出目录和`--symbol-prefix`；别同时编译font.cc又EMBED相同font.ntf。

调用`Font::Find/Contains`核对码点，`MeasureText`与`DrawText`共用真实度量。`DrawText`透明绘制，更新短文字先擦旧区域。缺字/非法UTF-8计数必须检查。它不自动排版、复杂塑形、缩放；换行/截断按相同advance，不能按UTF-8字节切字。

画布合同：字形是连续位，无逐行补字节；`Surface1bpp`按行补齐。竖屏684宽的stride=86，总104576字节，不能直接喂给物理1216×684的mono接口。默认`main/app_view.h::ToNative4`把逻辑字形帧转为415872字节原生gray4，旋转一次、偶X低nibble。横屏则不旋转。保留此桥或验证等价替代。

`main/app_view.h`是实际生产字体/UI绘制器，Windows验证和固件调用同一函数，不以HTML或独立Pillow样张冒充生产输出。已有MSVC时：

```powershell
& "$SkillRoot\skills\device-interaction\scripts\verify_native_ui.ps1" -Output "$Work\checks\native-ui" -Python $Python
```

脚本可用`-VsDevCmd`指定现有MSVC环境，输出必须在Skill外的全新目录。没有主机C++编译器时仍可用原生ESP工具链构建目标，并记录主机生产帧检查未执行；不因此切WSL或静默安装大型编译环境。固件运行仍需实际页面与触摸验证。

`plus_font_data_license`包含完整OFL与NOTICE；默认工程生成器将实际依赖许可汇入`generated/license_generated.h`，主应用直接引用全文并按生产字体分页显示，不复制整个许可文本到RAM。单独接入时可直接引用字体许可符号。替换UI时不能移除许可可达性。字体源码包保留本模块LICENSE、OFL和NOTICE；BIN交付另附完整许可并保留设备内查看入口。

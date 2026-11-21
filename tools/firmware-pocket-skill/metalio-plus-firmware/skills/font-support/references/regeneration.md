# 仅在已有规格/字符不足时重新生成

源身份以`assets/fonts/fonts-manifest.json`和`licenses/FONT-NOTICE.txt`为准：固定Google Fonts提交中的Noto Sans SC变量字体，SHA256 `a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da`。生成器不自动下载或切换来源。普通prepare不需要源TTF。

在Windows现有Python环境按`scripts/generator-requirements.txt`准备可选Pillow/fontTools，记录FreeType实际版本。源字体校验通过后输出到新目录：

```powershell
python -B "$Font\scripts\generate_fonts.py" --source "$SourceTtf" --output "$Work\font-expanded" --sizes 32,40 --weights 500,600 --chars-file "$Work\needed.txt"
python -B "$Font\scripts\prepare_font.py" --assets "$Work\font-expanded" --size 32 --weight 500 --output "$Work\main\generated\expanded"
```

固定源wght轴400/500/600，coverage>=112为墨点，保留ls基线、bearing、真实advance。字形保持1bpp，不作二次缩放/膨胀/灰阶边缘。新字号/字符不是源样本已验收范围；记录原始与产物SHA、码点、缺字、工具版本、体积、生产渲染与真机状态。跨平台FreeType不同不能仅凭同名工具宣称字节一致。

补字符要取业务实际需要集合；完整汉字/输入覆盖、静态标签和可预见外部文本分开核验。现有字库允许明确缺字回退，但不能把问号替代当原字符覆盖成功。新增字体及衍生位图继续OFL，保留原版权、完整许可及来源说明。

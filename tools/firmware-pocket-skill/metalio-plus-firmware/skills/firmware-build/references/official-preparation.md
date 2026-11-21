# 固定官方工程准备（按需）

适用 Plus commit `ad8edf4150a688737c0d7c00cae4413079b262ef` 与 IDF commit `fff9895c82d744c7237be8847347bdd1b07c6643`。`$Raw` 为运维核验后的 PlusRawMirror 根目录；`$Project` 必须是尚不存在的短 ASCII 路径。

```powershell
python "$Skill/skills/firmware-build/scripts/prepare_official_project.py" --mirror $Raw --path $Project
```

脚本先核验官方归档的固定 SHA，再逐一核验并提取 `assets/official-build-fixes/source-selection.json` 中 798 个文件。随后加入本 Skill 的开发补全：UART UHCI/IOT 组件声明、固定依赖锁、从固定 `lang_home.cc` 机械生成的语言头、资源生成的 CMake 顺序约束。`skill-development-provenance.json` 区分原始选材与新增文件。镜像不修改，已有工程不覆盖。

语言生成器也可单独调用：

```powershell
python "$Skill/skills/firmware-build/scripts/generate_language_header.py" --project $Project --language zh-CN
```

它核对固定源码摘要和语法；已有不同头文件拒绝覆盖。新增语言/上游版本需重新审查，不能修改校验值强行通过。

运行官方整机构建后，由 SDK 原生入口合并：

```powershell
idf.py --preview -C $Project -B "$Work/build-official" build
idf.py --preview -C $Project -B "$Work/build-official" merge-bin -o official-plus-full.bin
python "$Skill/skills/verify-delivery/scripts/verify_merged_image.py" --build "$Work/build-official" --bin "$Work/build-official/official-plus-full.bin" --output "$Work/official-segments.json"
```

检查退出码。实际合并输出路径以 SDK 返回为准。此检查逐段核对构建源与完整 BIN、范围和空隙；它不证明业务功能、口袋兼容或真机显示。官方完整镜像仅作该工程路线的产物，默认双模式目标仍需迁移必要业务到紧凑自包含应用。

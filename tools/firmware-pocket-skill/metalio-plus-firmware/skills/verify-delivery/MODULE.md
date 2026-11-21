# 验证与真机闭环交付

输入必须是本次源码构建的产物，输出是同一份双模式 BIN、使用说明和与其摘要绑定的验证记录。运行在原生 Windows，默认具备 USB 串口条件。真实设备、端口和操作授权仍按 [真机流程](../../workflow/device-validation.md) 确认。用户已授权的本设备调试可持续推进，不重复逐次询问；未知设备、改变操作范围或需要破坏原数据时停下来说明。

## 构建与镜像

先按 [验收清单](references/acceptance.md) 检查真实初始化、配置、资源与业务证据；不能把存在函数、编译成功、预览正确当成设备功能通过。

激活本次固定工具链后，以下 `$Skill`、`$Work`、文件名取本次真实值。`build` 必须对应这次源码，输出选新版本文件名；命令失败即停止相应阶段。

```powershell
python "$Skill/skills/verify-delivery/scripts/firmware_tools.py" merge --build "$Work/build" --output "$Work/artifacts/app-version-dual.bin" --require-pocket --profile public-plus-pocket2 --report "$Work/evidence/structure.json"
python "$Skill/skills/verify-delivery/scripts/verify_merged_image.py" --build "$Work/build" --bin "$Work/artifacts/app-version-dual.bin" --output "$Work/evidence/segments.json"
python "$Skill/skills/verify-delivery/scripts/validation_record.py" init --bin "$Work/artifacts/app-version-dual.bin" --output "$Work/evidence/validation.json" --skill-version "实际包版本" --material-version "实际注册表版本"
```

`firmware_tools` 验证 S31 checksum/SHA、分区 MD5/布局、元数据与标准 app descriptor；`verify_merged_image` 验证每个源段与最终 BIN 字节一致、无重叠或未声明数据。二者都不证明业务、资源完整或真机通过。支持本版未签名/未加密 DIO/80MHz/16MB 绑定，配置不符需核实，不删检查绕过。

## 真机与结果

首次接入和准备交付时读取 [真机流程](../../workflow/device-validation.md)，在当前工程上下文里完成连接、获授权操作、串口取证、用户屏幕/触摸反馈及修正复测。分别完成独立与指定 Plus 口袋模式；在口袋测试中不从 0x0 写完整镜像覆盖管理器。

通过状态来自真实命令或用户报告，记录在 WORK。修改后 BIN 摘要改变，旧真机 PASS 不自动继承；建立新记录并重测受影响行为及两种启动路径。未接通设备或缺少必要结果时，仍可交付“待真机验收 BIN”，但任务的完整硬件验收保持 pending。

`validation_record.py check --bin ... --record ... --require-complete` 校验产物绑定、证据文件和类型，退出 2 表示还有 pending/fail，退出 1 表示记录无效；它不是自动真机测试器，也不能把填写 PASS 变成实际通过。

## 最终接收副本

完成检查后用二进制复制到用户实际取走的位置，并在该位置重新解析、核对摘要和构建源段：

```powershell
python "$Skill/skills/verify-delivery/scripts/deliver_artifact.py" --source "$Work/artifacts/app-version-dual.bin" --build "$Work/build" --destination "$Work/delivery/app-version-dual.bin" --report "$Work/delivery/copy-verification.json"
```

源文件、目的文件和报告必须不同，已有目的文件不覆盖。复制工具不接收运行验收记录，因此报告始终保留 `acceptance_status=pending_evidence`；是否已验收必须另看同一 BIN 的完整验证记录。后续复制、上传或可选 ZIP 解压后，再核对实际接收 BIN 的大小和 SHA-256；不猜损坏原因，不强制以 ZIP 代替校验。最终同时提供简明使用说明、验证记录和待处理项，勿把源码审查或镜像报告写成硬件已通过。

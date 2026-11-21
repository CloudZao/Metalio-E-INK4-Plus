# Plus 可组合应用底座

适用Plus4.7/S31、固定ESP-IDF提交fff9895c82d744c7237be8847347bdd1b07c6643及公开指南1.1与Plus pocket2设备绑定。底座是提炼的派生源码，不是镜像原件；无需回到作者工程。provenance.json保留来源，历史验证只属于对应历史版本，新组合以本包验证记录及本轮WORK证据为准。

## 生成与扩展

在独立短ASCII工作路径运行，参数替换为用户应用：

```powershell
& $Python -B "$SkillRoot/skills/firmware-build/scripts/create_plus_project.py" --output "$Work/app" --app-id my_app --name "我的应用" --version 1.0.0 --storage none
```

可选`--wifi`复制并编译真实Wi-Fi后端、同时声明独立需TF；它不自动实现设备配网页面。按[联网能力](../../app-capabilities/references/network.md)接扫描、选择、密码键盘、持久化和连接状态。可选`--pinyin`生成新版候选表、输入核心和许可；它不自动生成键盘页面。二者可组合，也可另选合规实现，不限定天气/记事本。

存储none/direct-tf/checkpoint分别对应0/正schema；持久化模式钩子故意返回未实现错误，需业务真实接线。不要为让探针启动把真实需求改成NONE。[存储](../../app-capabilities/references/storage.md)提供DIRECT_TF小文本/状态示例；CHECKPOINT按口袋合同实现，不把TF直读伪装为迁移流程。

## 已提供的真实接点

- `components/epdiy`与`plus_hal`：S31送行/波形/I2C/触摸/震动/前光/按键/TF、显示完成回执、共享轨与Flash门。无需另起第二套按键/马达线程。
- `main/pocket`和桥：独立/集成身份、元数据、首屏物理完成后确认、可靠保存/返回。协议前置，而非最后贴标签。
- `main/app_view.h`：Noto真实绘制、中文顶栏、共享命中、横竖布局、完整许可分页。`app_lifecycle.h`：输入不等屏刷、合并最新完整帧、自动休眠计时和用户长任务豁免。main是`CAPABILITY_DEMO_ONLY`无状态演示，不是用户成品。
- `components/plus_font`、`main/generated/body`：32px/500默认中文资源；可按布局选择其他尺寸或子集，不把18包全链入BIN。
- `main/generated/app_assets.h`与`assets/posters.json`：与真实二进制一致的格式/尺寸/长度描述。默认中文灰阶4bpp锁屏415872字节、黑白1bpp深休103968字节；生产HAL初始化、显示及失败还原共用asset_contract.h。替换素材须同步描述并验证，不能只换文件而留旧1bpp入口判断。
- `main/generated/license_generated.h`：实际可达帮助全文；由同一依赖清单生成，不以一个丢弃字符串当已展示。

Noto逻辑竖屏mono的行stride为86，不等于物理面板mono的152；桥负责旋转。QueueGray4收完整物理1216×684、偶数X低半字节、白15。入队不等于物理完成；布局切换与首屏确认以DisplayedRevision为准。

## 从示例到自由业务

Agent可以替换业务模型、视图与绘图库，但保留可验证的显示、触摸、存储、取消、返回和电源合同。按需求读取能力小例，组合新题材；遇到未覆盖硬件接口才窄读固定官方资料。生成后可先做硬件探针；无USB时继续业务，标记未验，不挂起整个创作。

主循环不要每次触摸等待整次刷新；保留业务状态和最新画面。长按请求在锁屏分支前处理；取消任务需真实释放、可靠保存后再同一路PowerOff。自动深休在保存后重新核对用户活动，不能把刚来的触摸睡掉。Flash操作按同任务成对PauseForFlash/ResumeAfterFlash，TF写入不是Flash写入。

有效sdkconfig同时由defaults、S31 defaults、Kconfig/CMake及生成结果决定；不复制整机云语音/字体分区。main栈初始8192不是所有应用预算，新增网络/JSON/协议要看调用叠加、.su和实际高水位。保留看门狗并定位真实等待，不用关watchdog掩盖问题。

完成业务后更新scaffold-status并保留初始来源。改一个scaffold_only字段不构成功能验收；BIN、主机行为、USB和用户真机分别记证据。

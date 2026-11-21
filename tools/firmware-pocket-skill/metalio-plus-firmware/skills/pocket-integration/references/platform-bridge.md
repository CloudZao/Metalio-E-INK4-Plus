# Plus 标准平台桥接

本桥接实现公开指南1.1，固定真实Plus设备布局，不锁产品版本。`Initialize(hooks,error)` 完成模式识别、实际镜像/metadata/完整摘要校验、集成active run与用户核对以及业务恢复。`ConfirmStartup(first_frame,error)` 在恢复成功、首屏物理完成之后执行适用的OTA确认并提交started；网络/API成功不是启动条件。两者构成应用的pocket_open顺序，确认前不得进入正常可变业务。

`LastError()` 提供 None/InvalidArgument/Unsupported/Unavailable/Corrupt/IoError/SaveFailed/ReturnFailed；`ErrorMessage(Error)` 是简短提示，具体error字符串可用于安全日志。初始化失败应显示对应错误，不能一概说TF损坏。回调bool的恢复失败归Corrupt/恢复失败，业务save失败归SaveFailed；回调应在error中说明自己的实际失败原因，不覆盖旧档。

`ReturnToPocket` 的集成顺序是核对控制→业务已暂停并save→复核控制→saved→完整管理器复核→切启动→重启。失败撤销saved时必须成功写started才能继续编辑。`CanEdit()` 是业务输入门禁。独立返回则save后留在应用，不写口袋控制、不重启。

所有调用从一个应用任务串行发起；业务后台任务必须先实际停止写入。Hooks.restore/save不得递归调用桥接，写内部Flash时自行取得HAL Flash lease；桥接不会在调用业务钩子时占有该lease。深休调用Save（Checkpoint），正常返回调用Exit，CHECKPOINT策略据此处理需要导出的资料。

启动完整镜像哈希与EPD刷新使用PauseForFlash/ResumeAfterFlash互斥；失败不生成成功回执。管理器核验使用真实分区内容及标准保留ID pocket_manager，ESP描述符project_name/version与metadata一致，不要求固定品牌字符串或产品版本。

构建及主机测试只能证明相应代码路径。实际TF拔卡/损坏、启动回滚、Flash/屏幕竞争、断电、业务完整保存、独立与集成往返仍需真机验收。

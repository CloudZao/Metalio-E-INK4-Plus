# 按失败阶段排查

| 症状 | 第一组证据与动作 | 不要误判 |
|---|---|---|
| 口袋拒绝识别 | 对照源/导出BIN长度SHA、合并偏移、元数据profile、校验器报告 | 文件名dual不证明兼容；复制损坏先修复制 |
| 可识别但启动重启/退出 | 串口复位原因、backtrace+同次ELF、有效sdkconfig、PSRAM/分区/素材初始化 | 元数据正确不证明驱动能启动 |
| 首页正常但触摸/保存/联网失效 | 生产事件队列、命中区域/显示revision、工作线程、真实存储与DHCP/HTTP各层 | UI预览或探针不能证明用户业务 |
| 启动提示TF/存档错误 | 先读桥接Error分类：Unsupported查协议/绑定，Unavailable查资源占用，Corrupt查记录，IoError查实际文件操作；恢复回调再区分业务原因 | 版本/接口拒绝不等于坏卡；不能统一建议插原卡、重建身份或删除存档 |
| 正确密码仍未连接 | 先看停止/重启/认证/获取IP的实际阶段；停止后AP查询的ESP_ERR_WIFI_CONN必须结合STOP、radio及netif判断；查当前请求原因码是否被清理事件覆盖 | 可能尚未调用connect，不能先归咎密码/热点；不得把所有错误返回当已停止 |
| 锁屏/深休或返回失败 | 取消是否真正drain、保存回执、Flash/显示共享锁、海报descriptor/长度、返回合同 | requested stop不等于released；不能显示成功后丢数据 |
| 构建不前进 | runner退出/子进程/日志增量/编译最小复现、ccache、multilib路径长度 | ninja旧行数与Python缓冲猜测不足以认定根因 |
| BIN能运行但显示错误 | 实际字库/缺字/字重、mono stride、gray nibble、旋转、生产初始化 | 资源生成对而入口旧校验仍会失败 |

用firmware-build/scripts/capture_build_evidence.py关联本次源码、配置、ELF/MAP/BIN与日志；修改前后保留快照。官方参考包、当前WORK工程、旧产物三者显式区分。相似症状未经证据不强行同根因；修完针对真实触发路径验证。

诊断日志允许保留阶段、请求编号、接口错误和数字原因码，但不写SSID密码。修正后检查实际接收BIN的SHA及两态受影响行为，不能把主机SDK桩成功写成热点真机成功。

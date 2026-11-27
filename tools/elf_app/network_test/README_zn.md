# 网络测试

**中文** | [English](README.md)

通过正式 ELF 网络接口验证 HTTP/HTTPS、WebSocket/WSS、TCP/TLS、固定远端 UDP、DNS 和 ICMP Ping。配套电脑端使用 Service Lab 的“服务 → 网络测试”，不需要另启回显脚本。支持 397、470，要求内容区至少 600×1000，以及支持缓冲容量、空闲断网计时、网络准备、DNS 和 Ping 接口的宿主网络能力。固件与测试 ELF 需配套更新。应用进入后调用 `network_prepare` 自动尝试联网，不发送业务请求；准备失败后可检查 Wi-Fi 设置，再点击“读取服务与证书”重试。

## 使用

1. 电脑和设备连接同一可信局域网，在 Service Lab 中启用网络测试，选择“正常回显”。
2. 在设备应用中填写页面显示的主机和 HTTP 端口，点击“读取服务与证书”。应用读取各协议端口及本地测试 CA，仅将 CA 用于本应用的 HTTPS、WSS 和 TLS 连接，不修改系统信任根。
3. 点击“全部测试”，或分别选择 HTTP、WebSocket、TCP、UDP。结果按每页三项显示内容校验、字节数、耗时，以及失败时的宿主和底层数值错误码，可用上一页、下一页查看。
4. “持续 WS”保持同一连接重复收发二进制消息，方便检查退出、断网和待机。点击“停止”结束。待机或离页撤销句柄，恢复后重新读取服务再测试。

“断网计时”默认关闭：首次请求获接纳后，前台请求间隙也持续保网。开启后，无活动网络工作时遵循系统空闲断网计时；再次关闭恢复持续保网，不修改系统设置或禁止自动待机。测试 ELF 要求宿主提供 `network_set_idle_timer`。应用使用系统左右滑动识别：右滑执行默认返回，左滑默认无操作，不等同于结果分页；结果使用上一页、下一页按钮。系统手势开关及输入框自身操作优先级仍然生效。

准备操作通过局域网 HTTP 读取 CA，不验证电脑身份，只适用于已确认的本地测试服务。设备时间须正确，电脑网卡地址变化后需重新启用服务并读取证书。

DNS 和 Ping 可在填写主机后单独执行，不要求先读取 Service Lab 配置。DNS 显示首个 IPv4 地址；输入 IP 只验证直接地址路径，验证域名解析时应填写可解析的主机名。Ping 向所填目标发送四次 32 字节 ICMP Echo，间隔 250 毫秒、回复超时一秒，显示地址、回复/发送数量和平均往返耗时；四次均收到有效回复才判为通过。目标应允许 ICMP，例如已知可响应的网关。电脑系统或防火墙不回复 ICMP 时，该项失败不表示其他协议失败；应用不修改防火墙。

## 测试范围

- HTTP：GET 固定内容、POST 二进制回显、404 状态、9000 字节分块响应、延迟超时、HTTPS。
- WebSocket：文本、1024 字节二进制、空消息和 WSS，检查消息类型与内容。
- TCP：3072 字节流回显和 TLS，按累计字节检查拆分数据，不假定一次接收就是完整消息。
- UDP：128 字节数据报回显，超时视为未收到，不自动重传或声称可靠交付。
- TLS 拒绝证书：不提供本地测试 CA 时，要求连接失败；与此前正常 TLS 结果一起判断证书配置。
- HTTP 取消：撤销延迟请求，确认旧句柄不能继续访问；后台实际收尾另由 Service Lab 的活动连接数和设备日志观察。
- DNS：通过 `network_resolve` 取得首个 IPv4，并读取托管诊断结果。
- Ping：通过 `network_ping` 校验四次有效回复及统计；操作系统响应 ICMP，不由 Service Lab 回显。

自动测试预期 Service Lab 使用“正常回显”。延迟、不回复和主动断开用于单项或持续测试时观察异常，不能将这些场景下的预期失败解释为全部测试通过。MQTT 不在范围内。

退出、待机、唤醒、重复进入、断网、功耗和 RAM 低水位需要真机验证。服务器写出数据或应用读到数据不表示墨水屏物理刷新完成。

## 源码与构建

`network_test.c` 管理界面、前台测试状态机、确定性数据校验和连接清理；`icon.pbm` 是原始图标。应用不创建线程、不直接访问网络驱动或文件系统。应用显式申请 8 KiB 测试接收缓冲、按测试负载指定发送容量，并将 HTTP 测试响应上限设为 1 MiB；读取服务配置及证书时单独指定 4 KiB 接收缓冲。这些容量属于测试应用的选择，不是宿主联网能力的固定限制。

在仓库根目录启用配套 ESP 工具链和 Python，分别执行；`$compiler470`、`$compiler397` 为对应编译器完整路径：

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler470 -AppDirectory ./tools/elf_app/network_test -Target esp32s31 -OutputDirectory ./tools/elf_app/network_test/build/470
./tools/elf_app/build_app.ps1 -Compiler $compiler397 -AppDirectory ./tools/elf_app/network_test -Target esp32s3 -OutputDirectory ./tools/elf_app/network_test/build/397
```

输出为各目录中的 `network_test.elf`。每次构建后按[安装清单说明](../README_zn.md#本地安装清单示例)生成同目录 `manifest.json`，使用应用 ID `network_test`，重新计算大小与 SHA256；397 为 `esp32s3`，470 为 `esp32s31`，ABI 为 1。构建目录不代表统一交付目录。也可通过 Service Lab 现有应用商店上传并上架匹配目标的 ELF。

公共接口、资源上限和生命周期见[托管联网](../README_zn.md#托管联网xz_app_cap_network)。

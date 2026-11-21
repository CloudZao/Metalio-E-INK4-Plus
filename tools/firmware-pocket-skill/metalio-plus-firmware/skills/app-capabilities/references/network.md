# 设备端配网与网络请求

本页在联网需求中读取。默认提供设备端“搜索→选择/手输SSID→输入密码→连接”界面。用户可另选配网方式，但不能把开发电脑联网、烧录时硬编码密码或口袋曾联网当成设备已经联网。

## 已有可编译实现

包内 `../../pocket-integration/assets/wifi-standard/` 提供：

| 文件 | 能力 |
|---|---|
| pocket_wifi.h/.cc | 指南1.1 wifi_current标准公共配置只读解析、wifi_local私有存储、优先级、手动候选 |
| pocket_wifi_connection.h/.cc | 真正ESP Wi-Fi STA初始化、异步扫描、连接/DHCP、新连接代次、有限重连、停止无线确认 |
| pocket_wifi_connection_model.h | 可显示SSID、信号等级、有效IP判定、去重AP列表 |

复制五个文件到 main/wifi；编译两 .cc、加入头目录；组件依赖 `esp_wifi esp_netif esp_event esp_timer freertos heap` 及已有 pocket_core/fatfs。只增加这些文件到CMake并不等于完成配网页面。生成器的 --wifi 是编译接入，下面动作接线仍需完成。

## 启动与优先级

1. 先经真实分区判断模式，集成模式核验身份/run/镜像。不要为网络读取而跳过口袋校验。
2. TF挂载并归设备使用；建立应用私有配置目录。集成传 `/sdcard/firmware-pocket/shared` 和本次已验证 run_id；独立传空shared/空run_id，不创建公共身份。私有目录可以位于稳定业务根的私有子目录，切换启动方式不应意外换业务档。
3. 在桥接 Hooks.restore 收到的已验证 `const RunContext*` 中取 run_id；nullptr就是独立，不在持桥接锁的回调里再调用 Integrated/CanEdit 等桥接函数。调用 [InitializeDeviceWifi](../examples/network_device_setup.h)。公共配置 Missing/Corrupt 可退到私有网络/设备配网；私有配置错误用独立中文提示保留（不能被无线ready消息盖掉），禁止假报保存成功，仍可扫描。不要格式化、生成新口袋身份或偷偷换存储根。
4. 新集成run优先公共网络最新值，同run用户刚手动选的值优先并可跨重启；独立只使用应用私有配置。应用自己重新建立无线连接，口袋后台不替应用保持连接。
5. 公共读取不提交候选；用户实际保存新凭据才生成当前run的私有待提交标记。忘记私有网络不删除公共权威。

本实现的私有凭据也保存在TF，因此采用它时声明独立配网需要TF（tf_required_standalone=true）；独立运行不等于无TF运行。产品明确要无卡配网时需另实现私有NVS存储，公共只读协议保持，不能把现有TF代码说成已支持NVS。

公共配置只按随包[公开指南1.1](../../pocket-integration/references/public-guide-1.1.md)读取：RecordStore `shared/wifi_current` schema 1；payload 为 **1字节SSID长度、1字节密码长度、SSID原始字节、密码原始字节**。SSID 1..32字节，密码0..64字节，无NUL、无尾随内容；空密码表示开放网络。按原字节比较，不去空格、不要求UTF-8、不做Unicode归一化。宿主的多网络列表、容量、优先级及PKWIFI1均是宿主私有扩展，应用不依赖或改写。缓存可合并自己的私有凭据，但每次公共读取只导入宿主选定的一组。

`wifi_local` 中的PKAWIFI1封装/PKWIFI子列表仅为应用自有私有格式，保留旧应用私有凭据兼容；这不代表公共协议采用同样格式。`WifiSsidLabel` 只供界面安全展示不可显示的原始字节，连接和持久化始终传原字节。默认AP列表保留可连接的非UTF-8 SSID，不能把转义名称当成真实SSID。

公共候选 `app_dir/_pocket/wifi_offer` 使用 **16字节原始run_id + 上述两长度字节和SSID/密码**。32字符十六进制run_id只存在上下文/JSON表示中，不原样写入二进制候选。宿主 `status/wifi_result` 使用指南规定的JSON字段，由公共适配层严格解析；`accepted`才表示宿主采用，应用保存、提交候选、关联或业务HTTP成功均不冒充已采用。

## 页面实际接线

- 搜索按钮只投递 `RequestWifiScan()`，不能等配置读取成功才可用。
- AP列表来自 `WifiConnectionSnapshot().access_points`；支持重新扫描和手动SSID。密码在设备键盘输入、默认掩码，不进入HTML预览、日志、业务checkpoint。
- 用户提交后由内部RAM存储worker执行 `SaveAndConnectDeviceWifi`：先持久化私有配置，再排队连接。明确“存储失败”“已保存但队列忙”“正在连接”。
- 切换连接前，服务先关闭link事件接纳，实际停止STA并等待STOP/netif down；AP查询返回ESP_ERR_WIFI_CONN时，只有已观察真实STA_STOP、radio已停且netif确实down才接受。再经默认事件循环barrier确认已排队旧事件处理完成，才重启并连接；不以固定150ms等待当成排空保证。准备阶段有界失败则停止本次连接，不假报成功。若初始化在事件屏障注册前失败，仅SDK驱动与AP查询均明确NOT_INIT、radio未启动且实际netif down，才确认没有无线活动，允许继续保存退出；未知错误不能当作stopped。回调另检查当前请求代次与原始SSID/BSSID归属；认证等首个有效失败原因先保存，再关闭事件接纳和主动断开，避免清理断开码8覆盖认证失败码202。同SSID快速重连、暂停时旧事件和满事件队列仍列为目标真机专项。
- `WifiConnectionView.failure/reason_code/message` 区分重置未完成、认证或握手失败、关联失败、已关联无IP、启动失败及超时；数字原因码随提示展示。主页、网络列表、密码页共用 [WifiPageStatus](../examples/network_device_setup.h) 的状态选择规则，先显示错误再处理离线早返回。保存失败和“已保存但队列忙”另存为操作结果，不被稍后的radio ready消息盖掉；没有密码进入View。
- 图标只看 `connected`，它要求关联、属于本连接的新IP和实际netif状态；天气/HTTP状态另显示。启动确认不等待联网成功。
- 返回/深休先取消HTTP及业务任务，再 `SuspendWifiConnection(timeout_ms)` 等实际driver stop确认。超时不得当成功；可留在原界面重试。无线worker和事件循环保留为本次应用生命周期内资源，并非销毁所有线程；成功返回/深休由系统复位释放。取消返回可调用 ResumeWifiConnection 后重新选网。
- 本版示例不自动向口袋推荐网络。确需推荐时使用 `ReadPendingWifiOffer`、已验证run及公共适配层的 `WriteWifiOffer`，仅提交成功才 ClearPendingWifiOffer；候选失败不能回滚已确认的业务保存。不直接改权威。

## HTTP/HTML业务接线

`assets/plus_services/bounded_http_body.h` 提供真正读空缓冲的有界reader。适配ESP时read调用 `esp_http_client_read`，complete调用 `esp_http_client_is_complete_data_received`；即使fetch_headers后complete已经true，也先读出缓存，再在EOF检查完整性。负数、空、截断、超限分别失败；配置真实请求超时，取消在每次有界读之间检查，不能中止一个未设置超时的阻塞IO。

先在Windows用真实响应验证所选来源的状态、编码、HTML结构与解析器，保留脱敏样本；再固化到设备。正文上限由业务指定，不能默认1KB适配所有HTML。HTTPS保留证书/域名检查，校时问题单独处理。连接代次或页面任务已失效时丢弃结果，离线不显示伪造天气。

来源/服务条款与当前地址在接入时核实；这里不保证历史天气地址永久可用，不默认需要付费API、不借官方聊天身份。真实网络与设备扫码/配网需要目标设备验收，主机夹具不是联网证据。


## 可重复验证与证据边界

`python scripts/run_host_tests.py --output <Skill外证据目录>` 执行真实RecordStore、私有凭据和标准wifi_current字节边界/公共只读回归。

`python scripts/run_wifi_production_tests.py --output <Skill外证据目录>` 直接编译随包真实 `pocket_wifi_connection.cc`，只把ESP-IDF/FreeRTOS边界替换成可控SDK桩，运行连接、stop、事件、DHCP和设备状态接线。覆盖合法ESP_ERR_WIFI_CONN、新IP、缺STOP、netif仍up、未知AP错误、缺/拥塞屏障、错误密码后改正、202/8错误保护、非本次事件、保存失败、队列忙及非UTF-8 SSID。可选 `--baseline-wifi <明确提供的旧模块目录>` 复现旧生产代码缺陷，不是安装或使用本Skill所需输入。

这些是主机生产代码路径回归，不能替代ESP32-S31实际SDK编译、真实事件时序或用户2.4GHz热点验证。最终仍在目标设备测试正确/错误密码、同SSID改密码重连、快速切换、缺DHCP、断电和正常返回；只有实际收到新IP才显示已联网。

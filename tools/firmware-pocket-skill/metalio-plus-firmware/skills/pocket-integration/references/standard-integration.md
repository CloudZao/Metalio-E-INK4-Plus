# 标准接入实现

唯一线协议事实源是 [公开指南 1.1](public-guide-1.1.md)。本页解释本模板的实现，不增加指南之外的准入字段。固定设备常量在 `assets/pocket-standard/pocket_layout.h` 与 `pocket_boot_layout.h`。

## 控制记录

`PKREC01\0` 32 字节小端头，schema=1，payload 最大4096字节，CRC32覆盖头前28字节及payload。普通 `RecordStore::Load/Save` 选择最高有效generation，写另一槽、flush/fsync/close、读回核对；双槽可不同代不同内容。一槽坏可降级，等代冲突/未知格式/IO阻断，溢出拒绝。LoadConfirmed/SaveConfirmed仅供应用私有额外策略，不用于标准协议key。

| key | 格式 | 写入责任 |
|---|---|---|
| shared/identity | user_id LF，兼容历史LF0LF/LF1LF | 宿主 |
| status/run | run_id,user_id,app_id,version,image_sha256,phase 全部字符串JSON | 宿主 |
| status/receipt | run_id,user_id,app_id,phase 全部字符串JSON | 当前应用适配层 |
| shared/wifi_current | SSID长度、密码长度、原始字节 | 宿主 |
| app_dir/_pocket/wifi_offer | 16字节原始run_id + wifi_current编码 | 当前应用适配层 |
| status/wifi_result | run_id,offer_generation,result 全部字符串JSON | 宿主 |

严格JSON拒绝未知/缺失/重复键（包括转义后重复）、非字符串、尾随数据、BOM。key顺序与JSON空白不影响语义。所有已定义字段值的语法均为ASCII，字符串转义先解码再校验。app_id是小写字母开头的字母/数字/下划线；用户ID另用指南8.2的历史兼容语法，不能混为同一规则。

run只有六个字段。RunContext的image_bytes、storage_mode、state_schema从验证过的真实运行镜像填入；DecodeRun不会要求它们。摘要覆盖实际ESP应用镜像至附加SHA结尾，包含内嵌身份头，不是整个独立BIN。

## 初始化与保存

按真实分区和运行地址分类，未知布局报Unsupported，不根据TF目录猜模式。ValidateRunningImage逐段读取实际Flash，推导长度，检查ESP芯片/配置、段界限、XOR校验、SHA、固定0x120身份头及ESP描述符一致性。管理器必须flags=1、保留ID pocket_manager，并满足完整标准metadata；版本仅与它自己的ESP描述符一致，不锁定具体产品版本。

集成启动只接受身份/版本/镜像SHA一致的active run；prepared/closed/missing禁止进入业务。恢复初始化成功、首屏实际完成、适用的OTA启动确认成功才提交started。重启恢复同一run也重新写started，不沿用saved继续修改。

业务暂停后执行save并校验，写saved，复核返回目标，切boot再重启。saved后失败保持暂停；如要继续编辑，先成功写started。独立ReturnToPocket执行保存后返回true且留在应用，不生成协议记录。NONE可以无回调；其他模式必须restore/save。正常返回和深休保存是不同动作。

CHECKPOINT同一run只有私有归属标记和业务校验都通过才可恢复自己的Flash；新run从本用户、本app的TF目录恢复。损坏/过新schema/IO不能当首次运行。DIRECT_TF直接flush原业务，不复制整套大文件。控制记录绝不混入私有回滚。

### 应用业务存储初始化责任

应用固件负责自身业务存储目录的检查、必要创建、初始化、读取、保存和恢复，不得依赖固件口袋预先创建业务目录或存档文件。

首次运行时，应用应在确认存储环境和路径有效后，按需准备自身业务目录。目录或业务存档尚不存在可以是正常的首次运行状态，不得因此直接阻断启动。

只有确认不存在既有业务存档时，才允许初始化默认状态。存储不可用、读取失败、历史存档损坏或版本不兼容，不得作为首次运行处理，不得擅自覆盖原有资料。

存储位置遵循现有接入协议及所选存储模式，本条不额外指定具体目录。无存档需求的应用不必创建业务目录。

业务初始化失败时，应保留原资料并提供可操作的错误提示及符合协议的安全恢复方式，不得伪造保存成功或交接回执。

## Wi-Fi

SSID 1至32原始字节，密码0至64字节，无嵌入NUL；不要求UTF-8，不trim或归一化。offer最大114字节，run_id前缀是16字节解码值。结果generation是1至uint64最大值的无前导零十进制字符串。应用的ReadWifiResult只读结果；WriteWifiResult和SaveRun是宿主/主机测试辅助，不在应用桥接调用。

候选提交不等于采用或联网。只在正常保存返回后由宿主验证；业务返回不依赖候选采用成功。独立公共读取为Unavailable，真正两槽Missing才是NoConfig，坏档不伪装无网络。

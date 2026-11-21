# 小业务存储和双模式接线

先选NONE/CHECKPOINT/DIRECT_TF。无需存档不添加TF；本文的TextStore和CounterState是实作DIRECT_TF小例，不能宣称有Flash CHECKPOINT实现。

## 可复用实现

- `assets/plus_services/durable_text.h`：TextStore(root,key,byte_limit)。先Load，Missing才允许新建；Corrupt/IO/未知schema不覆写。Save校验UTF-8和字节预算，核对前版本未改变，复用RecordStore双槽/CRC、真实同步与读回，成功才替换Saved()；草稿由业务持有，失败保留。
- 文本默认3072字节只是例子，最大参数4000以容纳当前RecordStore有界记录；更大文本另设计流式存储。权威格式是两个 .pkr，并非伪装的TXT；需要用户可编辑TXT时显式设计导入/导出和冲突检测。
- `assets/plus_services/counter_state.h`：实际64位小状态例，包含格式标识、溢出校验，复用TextStore。不将计数器业务强加给用户。
- [direct_tf_hooks.cc](../examples/direct_tf_hooks.cc)：实际plus_pocket::Hooks接线。应用私有浅根按稳定AppID生成；两个启动模式同根，桥接控制档仍留口袋原位置。挂载失败不创建替代目录，不重写公共身份。
- 同一应用只由一个存储worker写同一key；读取到别的writer已改变记录时停下并明确重载，不覆盖。需多来源协作时设计独立事件/回执而非多个writer抢同一权威档。

## 接线顺序

恢复先完成→显示真实首屏→平台启动确认→started→可编辑。保存/深休/返回前先停止或排空会写业务数据的后台任务，再调用实际保存钩子；返回口袋的saved由桥接在业务成功后提交。返回失败不能继续改数据却留旧saved。

小状态可直接用 CounterState.Load/Save；游戏界面只在Save成功后提示已保存。TEXT保存失败不把草稿换成Saved()；损坏读取不初始化为空白再覆盖。串口诊断不打印正文、账号和Wi-Fi凭据。

CHECKPOINT按包内口袋桥接说明实现“业务状态+自身run归属”本机提交：同run恢复自己的Flash，新run读TF；深休保存本机，真正返回才导出。未完成这份实现时不要把DIRECT_TF例改成CHECKPOINT声明以通过元数据检查。

## 首次运行的业务目录准备

首次运行时，业务目录可能尚不存在。应用必须自行检查并按需创建合法的业务目录，不得假定固件口袋已经完成初始化。

RecordStore::Load 不负责创建目录，目录不存在可能返回 IoError。应用应先确认存储环境有效并准备目录，再执行存档读取。真正没有存档时允许初始化，其他存储错误必须保留原资料并明确处理。

CHECKPOINT 和 DIRECT_TF 均应按现有接入协议管理各自业务存储的初始化、保存和恢复。NONE 不创建无意义的业务存档。

首次启动、首次保存、退出及再次进入时，应正确完成业务状态的持久化与恢复。

## 升级/重置只按需加

迁移先确认旧源/新目标及冲突，完整复制并读回后才标记旧源完成；新档有效时不灌回旧档，已迁移旧源不在用户删除新档后自动复活。主档、公开快照、外部事件和去重账本职责不同；schema/实例/事件ID不靠目录名猜测。此包未宣称提供通用自动迁移器。

主机检查实际双槽、坏档保留、UTF-8边界、缺目录失败、外部修改拒绝及同根恢复。真实卡拔出、异常断电、MSC所有权和两个模式往返仍需真机。双槽与读回不是所有卡在任意断电瞬间绝不丢数据的保证。


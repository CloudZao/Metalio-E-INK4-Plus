# WorkBuddy 音频流协议

[English](audio_stream_en.md)

设备通过 WebSocket 连接 `/xiaozhi/api/workbuddy/device/asr/stream?format=opus`。主机配置和接口路径在 `main/cloudzao_endpoints.c` 维护；主机配置已包含 `/xiaozhi`，拼接时不重复添加。授权请求头和服务端的 ready、partial、final、error 消息沿用现有协议。

## 音频包

服务端发送 ready 后，设备开始采音。设备复用共享音频服务的 `opus_codec` 任务与百问 AI 使用的 Opus 编码器，使用 16 kHz、单声道、60 ms 帧长、复杂度 0 和默认 DTX。每个 WebSocket 二进制消息包含一个原始 Opus 包，包正文最多 6400 字节，不使用 Ogg 容器，也不添加百问协议的二进制头。

音频服务按完整帧采样，经共享编码队列输出 Opus 包。WorkBuddy 的回调仅将完整包写入有界 PSRAM 包队列，不维护 PCM 队列或独立编码器；网络任务仅发送包并累计校验。包队列使用的本地长度前缀不会上传。松手后停止采音，等待已入队和正在执行的编码及回调完成，再排空 Opus 包队列并发送 finish。最短录音检查以包对应的原始采样数为准，不以压缩后的字节数判断。

设备端不设置固定录音时长，持续按住时边采集边上传，缓冲容量保持有界。服务端及 ASR 的会话限制仍然适用。

## 结束与校验

全部音频包发送成功后，设备发送文本消息：

```json
{"type":"finish","audioBytes":1234,"audioCrc32":"89abcdef"}
```

- `audioBytes`：本轮所有成功发送的 Opus 包正文的累计字节数。
- `audioCrc32`：按发送顺序连续累计包正文的 CRC-32/ISO-HDLC，与 Java `java.util.zip.CRC32` 一致；初值和末尾异或值均为 `0xffffffff`，反射多项式为 `0xedb88320`，输出为八位小写十六进制字符串。
- 校验不包含 WebSocket 帧头、掩码、包长度字段、finish 消息或解码后的 PCM。Opus 包之间不插入分隔字节，每轮重新初始化校验。

现有服务端接口通过 `format=opus` 选择 Opus 模式，按消息边界逐包解码，并在解码前累计长度与 CRC；不指定格式的旧设备仍使用原 PCM 协议。Opus 模式的 finish 不发送 `pcmCrc32`，不能使用 PCM 校验逻辑处理 Opus。设备收到服务端 final 后才提交电脑指令；采音、编码、包队列溢出、发送失败或连接中断时，本轮作废，不发送 finish，也不跨连接复用累计值。CRC 仅验证已统计上传字节的一致性，无法发现统计前已丢失的采样。

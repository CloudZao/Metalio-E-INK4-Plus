# WorkBuddy audio stream protocol

[简体中文](audio_stream.md)

The device connects over WebSocket to `/xiaozhi/api/workbuddy/device/asr/stream?format=opus`. The host and endpoint path are maintained in `main/cloudzao_endpoints.c`. The host configuration already includes `/xiaozhi`; do not add it twice. Authorization headers and the server's ready, partial, final and error messages follow the existing protocol.

## Audio packets

Capture starts after the server sends ready. The device reuses the audio service's `opus_codec` task and Baiwen AI's shared Opus encoder with 16 kHz mono audio, 60 ms frames, complexity 0 and default DTX. Each binary WebSocket message contains one raw Opus packet of at most 6400 bytes, without an Ogg container or a Baiwen binary protocol header.

The audio service captures complete frames and produces Opus packets through its shared encoding queue. The WorkBuddy callback only writes complete packets to a bounded PSRAM packet queue; WorkBuddy has no PCM queue or independent encoder. The network worker only sends packets and accumulates the integrity check. Local packet length prefixes are never uploaded. Releasing the input stops capture, waits for queued and active encoding and callbacks to complete, then drains the Opus packet queue and sends finish. The minimum recording check uses the original sample count represented by the packets, rather than the compressed byte count.

There is no fixed recording duration on the device. Capture continues while the input is held, using bounded buffers. Server and ASR session limits still apply.

## Finish and integrity check

After all audio packets have been sent successfully, the device sends a text message:

```json
{"type":"finish","audioBytes":1234,"audioCrc32":"89abcdef"}
```

- `audioBytes`: the total payload size of all Opus packets successfully sent in this turn.
- `audioCrc32`: CRC-32/ISO-HDLC accumulated over packet payloads in send order, matching Java's `java.util.zip.CRC32`. The initial value and final XOR are both `0xffffffff`; the reflected polynomial is `0xedb88320`. The result is an eight-digit lowercase hexadecimal string.
- The check excludes WebSocket headers, masks, packet length fields, the finish message and decoded PCM. No separator bytes are inserted between Opus packets. The checksum is initialized afresh for each turn.

The existing server endpoint selects Opus mode with `format=opus`, decodes packets using message boundaries, and accumulates byte count and CRC before decoding. Older devices without the format parameter continue to use the original PCM protocol. In Opus mode, finish does not include `pcmCrc32`; PCM integrity checks cannot be applied to Opus. The device submits the computer command only after receiving the server's final result. Capture failures, encoding failures, packet queue overflow, send failures or disconnection invalidate the turn without sending finish. Counters are never reused across connections. CRC only checks the consistency of uploaded bytes already included in the counters; it cannot detect samples lost before accounting.

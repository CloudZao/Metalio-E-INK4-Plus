# WorkBuddy audio stream

[简体中文](audio_stream.md)

Service URLs are managed in `main/cloudzao_endpoints.c`.

## Capture and delivery

The device reuses the shared audio service for capture and Opus encoding. Complete audio packets pass through a bounded PSRAM queue; there is no separate encoder or PCM queue. Releasing the input stops capture, waits for encoding and callbacks to finish, then sends the remaining queued audio. The minimum recording check uses the original sample count.

The device has no fixed recording duration. Capture and upload continue while the input is held, using bounded buffers. Server and ASR session limits still apply.

## Completion and failures

The device submits the computer command only after audio delivery completes and recognition results arrive. Capture failures, encoding failures, queue overflow, send failures or disconnection invalidate the turn without submitting incomplete content. Each connection maintains its own send state. Integrity checks cover only counted upload bytes and cannot detect samples lost before accounting.

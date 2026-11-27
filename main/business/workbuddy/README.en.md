# WorkBuddy

[中文](README.md) | **English**

WorkBuddy binds the e-paper device to a computer assistant for voice commands, replies and message history. Service, computer and device prerequisites all apply.

## Configuration and binding

- Configure `cloudzao_workbuddy_host` in `main/infra/network/endpoints/cloudzao_endpoints.c`.
- The service must support WorkBuddy local assistants, Baidu realtime ASR and WebSocket forwarding; keep the corresponding computer assistant online.
- Authorize first use by scanning the QR code. Device credentials are stored separately in NVS; the device does not hold Tencent access tokens or log credentials/message bodies.
- Authorization is separate from computer connectivity and does not approve computer actions for the user. Coordinate authorization configuration when changing service.
- Rebind through the top-right menu. The confirmation keeps the current conversation; new authorization must succeed before replacing credentials and clearing the old account's page messages. Refreshing/leaving does not revoke authorization already submitted on the phone.

## Operations

Domestic home pages include WorkBuddy; International variants hide it. Home is paginated with six entries per page, noninteractive page dots and wrapping physical page keys. Ordinary back retains the home page; Home returns to the first page. Pixel theme uses buddy, other themes workbuddy, with proportionally scaled icons.

Entry displays an icon during binding checks and chooses a stable welcome message for an empty session. Connection states distinguish unknown, querying, failed, online and offline. Status is queried on entry and roughly every 30 seconds, extended by request duration; failures do not imply offline.

- The top-right menu offers Sync messages and Rebind; tap outside to dismiss.
- Hold the screen or BOOT for voice, speak after the listening waveform appears, and release to stop. Audio readiness is awaited in background without forcing a hardware reset; failures use top notifications.
- Tap left/right to page. Physical short presses turn one page; holds advance ten pages per second until release, leave, standby or a boundary. Reaching the first page does not exit.
- Bezel Back closes overlays first, otherwise goes to the previous conversation page or exits at the beginning. Home always returns home.
- Network-dependent refresh is hidden offline. QR instructions/refresh appear below an available code.

## Messages and pagination

Replies update the accepted command's existing bubble rather than appending duplicates. Waiting means no reply is currently available, not computer completion. Sending focuses the new user message; later updates retain reading position.

Sync loads the default history page, up to 20 messages, and displays fetched user/assistant text oldest first. Repeated sync replaces the current history snapshot; empty results retain the conversation. Tools, attachments and metadata are not shown as text. Screen pagination does not automatically fetch additional history.

Text preserves newlines, blank lines and indentation without Markdown/rich-text parsing or inferred roles. Wrapping uses the current font/bubble width and pagination uses actual content height; only current-page widgets are created. The footer shows current/total pages.

## History memory budget

Bodies, message containers, line and page indexes allocate only in PSRAM, accounting for actual blocks/alignment without internal-RAM fallback. There is no fixed message/page truncation count.

- History budget: 1/32 of total PSRAM, capped at 256 KiB.
- After a history allocation, retain at least 1/4 of total PSRAM and at least 2 MiB free. This does not constrain later allocations by other modules.
- Each body is capped at 4 KiB, truncated on UTF-8 boundaries.
- Insufficient budget/allocation failure reclaims oldest messages and retries, retaining positions still cached. Clearing/destroying the page frees history allocations.

Network JSON staging and current-page LVGL widgets are outside this budget. HTTP staging is capped at 32 KiB; larger responses fail explicitly rather than parsing truncated data. These are device resource policies, not total server history limits.

## Audio, tasks and standby

### Capture and delivery

WorkBuddy reuses the shared audio service for capture and Opus encoding. Complete audio packets pass through a bounded PSRAM queue without a separate encoder or PCM queue. Releasing the input stops capture, waits for encoding and callbacks to finish, then sends the remaining queued audio. The minimum recording check uses the original sample count.

Recording has no fixed device duration: hold the screen or BOOT to capture and upload, with audio memory independent of duration. Service and ASR session limits still apply; recognition and command text are limited to 2000 UTF-8 bytes.

### Completion and failures

The device submits the computer command only after audio delivery completes and the final recognition result arrives from the service. Capture failures, encoding failures, queue overflow, send failures, service errors or disconnection invalidate the turn without submitting incomplete content; leaving or standby also stops the turn. Readiness and recognition waits have separate timeouts. Network failures or timeouts do not automatically resend accepted commands.

Each connection maintains its own send state. Integrity checks cover only counted upload bytes and cannot detect samples lost before accounting.

### Tasks and standby

Network tasks own independent contexts and use cancellation on leave, rebind and standby. Audio workers own capture transitions; NVS uses internal-RAM stacks. LVGL lifecycle does not wait for network/hardware cleanup. Hardware readiness, task-stack and concurrency failures provide explicit feedback.

Foreground soft network retention respects manual Wi-Fi disablement, does not refresh inactivity time and does not prevent automatic standby. Standby stops recording/reply/status activity and releases network retention. Waking restores connectivity without recording or resending. Computer work already submitted may continue; Sync can retrieve results. See [power policy](../../sysmgmt/power/policy/README.en.md).

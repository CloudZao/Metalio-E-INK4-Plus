# Display and refresh settings

[简体中文](README_zn.md)

ESP32-S31 LCD output uses only the data phase. Each DMA row carries active pixels from its first byte, with no leading dummy byte; the active data window matches the row width. Post-data clocks remain controlled by the board timing configuration. After SPV rises, each phase waits according to the CKV high-time configuration, then starts the LCD output for XCL, XLE and XSTL together, without an additional full-line delay.

If a standby or shutdown wallpaper is still decoding after one second, a loading notice appears at the top of the existing frame. Faster decodes display the wallpaper directly. Loading preserves the current orientation and pixels outside the notice. Standby wallpapers draw directly into the panel framebuffer while retaining the underlying page's orientation and dimensions. LVGL rendering pauses after parking and resumes after page resources are restored. The frame is saved before showing a notice and restored directly if decoding fails. Reader fonts remain available until standby content is ready. Before wake redraws the page, the physical target framebuffer is filled with white so fullscreen wallpaper pixels cannot remain outside the normal UI bounds.

Standby, shutdown, and reading-background decoders resolve wallpaper selections from the in-memory cache without reading NVS from tasks with PSRAM stacks. Startup hydrates the selections first. A selection is retained while SD is unavailable; an unavailable path uses the existing fallback for that entry point.

Once the home screen is ready, startup first performs one complete 48-phase GC16 refresh to white, then reveals the home screen with a wipe. The home screen is not drawn during cleaning. The reserved border receives only whitening pulses during the full refresh and remains undriven after success. Refresh failures retain the existing clear-and-recover path. Later clock, battery and network updates use the ordinary refresh policy.

Settings → Screen saves refresh mode and black depth independently. Clear, Balanced and Fast share one row. Phase counts describe implementation rather than menu choices. VCOM appears only after a valid calibration record is read.

Navigation uses ten strips from left to right in the current orientation. Clear and Balanced use 20-phase whitening and black/white drawing; Fast uses eight phases. Adjacent strips start one phase apart. Monochrome transitions total 49 or 25 phases. Grayscale totals are 77 for Clear (20 + 48 + 9), 66 for Balanced (20 + 37 + 9) and 54 for Fast (8 + 37 + 9). White-to-white pixels are undriven. Only a successful refresh updates the baseline. Gray timing and strip delays are stored separately. These erasures apply to navigation wipes; ordinary DU/GL updates do not add them. Missing buffers or refresh failures fall back to full GC16. Latency and ghosting require panel verification.

### Refresh modes

Entering or leaving standby and shutting down use the same cleaning sequence as startup: refresh the old frame to white with the complete 48-phase GC16 waveform, then reveal the target with a wipe. Equal white pixels retain conditioning pulses during cleaning. One refresh worker runs both stages using the existing animation buffer, with no additional full-screen allocation. A cleaning failure stops subsequent drawing; standby and shutdown retain their existing retry behavior. Parking or power-off waits for the target frame, and wake restores the frontlight after that frame completes. These transitions take longer and visibly flash; ordinary navigation and wallpaper loading notices keep their existing policy.

- **Clear (default):** full 16-level grayscale with 48-phase GC/GL and 20-phase DU, prioritizing detail.
- **Balanced:** 36-phase GC, 37-phase GL and 20-phase DU.
- **Fast:** 36-phase GC and 37-phase GL. Ordinary DU has 14 phases (13 black-driving and 14 whitening); navigation erasure and black/white drawing use eight. Faster waveforms may leave ghosting.

Startup, standby and shutdown whitening, the first frame after a mode change, manual full refresh, scheduled ghost clearing and fault recovery always use full 48-phase GC. Diagnostic patterns use the selected mode unless they explicitly request full GC. Phase counts are not elapsed times.

Legacy Balanced (full) maps to Clear, and Fast (trimmed) maps to Balanced. Current Clear, Balanced and Fast selections retain their meanings; the former extreme setting maps to Fast. Text does not use a separate special GL16 mode.

### Row underflow and GC recovery

Waveform sources remain in PSRAM. Gray merge tables are allocated on demand for the selected mode; LUTs, row queues and DMA buffers remain in internal RAM. Allocation failure falls back to full GC. The producer must keep I2S supplied.

Grayscale bookshelf content selects gray transitions. The IRAM/O3 producer inlines aligned 32-bit compact-LUT output. Portrait rows reuse a band; landscape rows batch strips and change direction only at boundaries. This adds no queue or buffer. Reason 2 with equal read/write indices and normal DMA refill is a real underflow, not a readiness race to suppress.

The scan ISR uses release/acquire queue publication. `line_ready` is a prefill signal only. A real underflow invalidates the baseline and forces full GC on the next refresh. Stream fault reasons identify missing ownership (1), an empty row queue (2) or premature completion (3). Diagnostics retain phase, row, queue indices, readiness, prefill, maximum DMA EOF/refill times and planned/completed successful frames. Logging occurs after scanning stops, outside the ISR, without another buffer.

### Preparation before drawing and refresh

Preparation converts and analyzes I1 / L8 pixels without selecting refresh policy. L8 conversion pairs pixels along the panel's X axis and writes complete 4bpp bytes, preserving adjacent nibbles at odd edges. Quantization remains unchanged and uses the existing buffers. Small landscape I1 updates retain the scalar path. The refresh worker handles change detection, waveform selection, scanning and baseline updates after successful draws.

I1 is the default. `GrayRole` marks content allowed to use L8: bookshelf backgrounds and covers, images, wallpapers, photos, antialiased text and assistant images. Ordinary UI, icons and decoration retain independent monochrome sources. Pure monochrome caches do not enable L8. Selection considers visibility, clipping, opaque coverage, top layers and system masks. Format changes occur after layout and before flush, reusing the draw buffer and redrawing. `FastMonochrome` overrides a subtree without changing its sources or caches.

In grayscale mode, ordinary UI sources become monochrome before native blending; image tone is applied once. LVGL alpha blends composed layers without business-specific flush corrections. Transparent cover margins remain cropped. Shutdown, standby and diagnostic output have separate paths. The shared PSRAM buffer holds at least one full-screen I1 frame and 64 rows of L8 at the maximum logical width, supports rotation, and adds no full-screen coverage map.

Image tone mapping is applied when an image is drawn into either the L8 main layer or an ARGB8888 rounded-corner layer. Subsequent layer composition preserves transparency without darkening the image again. Source images and cached pixels remain unchanged.

Software drawing retains I1, L8, A8, AL88 and ARGB8888, plus RGB888, RGB565 and RGB565A8 required by JPEG/PJPG decoding. RGB565 byte swapping, XRGB8888 and premultiplied ARGB8888 are disabled. Display output remains I1/L8, with ARGB8888 for rounded transparent layers.

Decoding and ZIP processing preserve luminance. New covers, saved wallpapers and assistant caches use A2L8 with an eight-byte header followed by L8 pixels; raw transfers preserve their files. Existing A2I1 remains readable without rebuilding, but lost grayscale cannot be recovered. Overwriting preserves filename and selection; magic bytes identify the format and new files use `.a2l8`. Caches remain bounded. Only visible background-image bindings affect display mode.

Reading text antialiasing preserves glyph coverage during rasterization and darkens gray edges by approximately 80% toward black. After glyph overlaps are blended, 32-bit block checks skip blocks containing only pure black and white; only intermediate grays in the remaining blocks are processed, without rewriting pure-black pixels. Disabling antialiasing renders text in black and white without affecting image grayscale. `epd_reader_aa` stores only the reading state needed for waveform selection; the submission queue captures a snapshot. Existing grayscale differential updates, clearing, scroll completion and reading waveform policies remain unchanged.

Ordinary differential updates retain DU black-to-black reinforcement. For changed pixels targeting pure black, GL16 keeps the source table's black-driving pulses and suppresses whitening. Unchanged black pixels receive reinforcement during the selected table's last four white-to-black driving pulses. Both modes operate within the intersection of active dirty rows and columns. Unchanged gray and white pixels remain undriven; other changed GL16 targets use the original table. Source tables remain read-only, with no extra phases or per-pixel output checks. Full refreshes, recovery, diagnostic patterns, full-frame GL scroll settling and erase animations retain their original conditioning. Effects on ghosting, black depth and subsequent erasure require panel verification.

When a visible settings card closes during text reading, the next submission drives every pure-black target with the selected waveform's white-to-black black-driving sequence, including text outside the former card area. This applies with text antialiasing enabled or disabled. The request merges into the closing frame without an extra full-screen whitening pass, scan phases or framebuffer allocation. Unchanged white and gray pixels remain undriven; other changed pixels update normally. Recovery and scheduled full GC cleaning take precedence. Hiding an already hidden card does not repeat the request. Equal black-driving pulses do not equalize grayscale history; ghosting and regional tone differences require panel verification.

Fast waveform initialization processes gray transitions in two passes: it measures the longest trimmed sequence, then recomputes and writes each transition. Working storage holds one transition at a time, and no full intermediate sequence table remains after initialization. Waveform parameters and output actions are unchanged.

Home icons use the LVGL decoder interface and retain SPNG’s 16-row tile boundaries. `tools/assets/generate_home_icon_pixels.py` packs the source PNGs into independent lossless zlib tiles, preserving ARGB8888 or A8 pixels. ARGB8888 sessions allocate a 16-row buffer in PSRAM; A8 icons use a temporary full-image buffer to preserve native scaling boundaries. Both free their buffers on close without retaining an image cache. Other resources retain their original paths. Both built-in shutdown images are packed by `tools/assets/compress_binary.py` as their original length followed by zlib data, with temporary decode storage freed after use. The artwork, complete font pack and dual OTA partition layout are preserved.

Black depth displays the latest validated ble_chip VCOM in millivolts. Until the boot read finishes, it shows the reading status. If the read fails or no valid record exists, Default / Medium / Strong controls and the saved selection return, using −1200 / −1600 / −2000 mV. Non-default settings retain the display lifespan and background discoloration warning. A later successful read or write hides the controls and takes priority; subsequent failures retain the validated value. The display worker applies changes after the current waveform; this is a driver setting, not a voltage measurement. The HTML factory tool handles calibration and cloud sync. See the [factory guide](../../tools/vcom/README.md) for storage, recovery and limits.

### Black depth

The 30-phase image table references [read-pico-reader](https://github.com/wegooo-cell/read-pico-reader/tree/main/components/e0470_epaper_waveform) and retains its Apache-2.0 license. S31 / E0470A14-AF scan timing and TPS65185 power handling are preserved. Display output requires acceptance on the corresponding panel.

### Rendering and resource preparation

`ScreenPaintCoalesce` merges repaint requests and retains the latest target during physical updates. Common navigation delays screen construction and validates the originating screen and standby state before execution; the back stack changes only when navigation executes. Pages do not need individual refresh-busy checks. The low-level `ScreenLoadReplace` loads an already-created screen; expensive preparation belongs in background work before that call.

Screen construction, widget rebuilding and LVGL rendering share one admission gate with physical updates, including nested rendering on the same UI task. Input, cancellation, short UI submissions and other LVGL timers continue while rendering waits. A queued frame is distinct from an active physical update and must not pause the entire UI. `EpdWait` is not allowed inside synchronous rendering; operations that wait for the panel must finish rendering before submitting or waiting for an update.

The update task runs above the UI and below the refill tasks, blocking on semaphores while waiting for scans or refill. Spatial gray transitions inspect each band's LUT on every phase. Mappings that depend only on the old or target gray value use paired-pixel lookup; mixed mappings retain the original table, and inactive bands output zero. Mode data reuses the existing LUT buffer and preserves the waveform's per-pixel output.

Shared book-cover cells create placeholders first. One background worker reads, decodes and scales images, retains only the latest pending target and waits for UI acknowledgement after each image to bound pending results. Sequence checks discard stale results after navigation, and deleted controls are never updated. Load failures retain placeholders; ordinary I/O or allocation failures are not cached as permanent missing images.

On S31 boards, LVGL asynchronous calls and cancellation use the shared UI queue instead of modifying asynchronous timers from background tasks. Cancellation matches all calls with the same callback and context that have not started; it does not free the context. The UI consumer releases dispatch records. A full queue or allocation failure returns `LV_RESULT_INVALID`, which callers must handle as with the original API. Other LVGL objects and timers must still be accessed on the UI thread or under its lock.

On S31 boards, linker wrappers serialize NVS C APIs that can access Flash, including read-only open and reads, against complete panel waveforms. The outer NVS call drains and holds shared I2C; lock order is Flash → I2C → NVS. Log settings are persisted outside the file lock. Do not call NVS while holding I2C or from active waveform or refill callbacks. Return values and synchronous behavior remain unchanged. Direct NVS C++ access and other raw Flash operations are outside this protection.

Ordinary I2C admission excludes controlled Flash ownership from its contention timeout. Actual transfers retain a separate timeout. Nonblocking attempts and indefinite waits preserve their original behavior.

Crash-image export takes the Flash gate before the file lock. Only SDK Flash operations drain and hold I2C; SD file operations do not hold I2C. Ordinary I2C lock holders must not acquire the Flash gate or log file lock. Flash is erased only after the SD copy is synced successfully. Mount changes, unmounting or clearing logs cancel pending erasure of a previous copy.

### Shared notices

Include `screen_common.h` and call `ScreenShowToast("Message")`. The notice uses rounded white text on black below the status bar, sizes to its text, wraps long messages and disappears after three seconds. Repeated calls replace it and restart the deadline; null or empty text is ignored.

The interface copies text and must run on the LVGL thread or under the UI lock. Background tasks include `ui/common/runtime/lvgl/ui_dispatch.h` and use `ScreenLvAsync`, retaining a valid text context until the callback runs.

SD data read/write errors show one notice per boot. Repeated errors do not create repeated notices. Standby defers presentation until resume. This indicates an I/O-path failure, not a confirmed defective card.

### Reading touch zones

Custom touch zones must retain at least one Menu zone. Replacing the last one keeps the configuration unchanged and shows a notice; assign Menu elsewhere before moving it. Historical configurations without a Menu zone, or with an invalid format, fall back to defaults.

Bookmark decorations and quick-panel icons reuse the embedded lossless image decoder, preserving A8/AL88 pixels and original dimensions. Drawing sessions allocate image buffers in PSRAM on demand and release them on close without retaining decoded caches. The Chinese and English file-transfer pages are stored separately as gzip data and share one logo served by the device at `/cloudzao-logo.png`, without internet access or an SD card.

# ELF apps and development

[中文](README_zn.md) | **English**

This directory owns the app catalog, build tools, loader contracts and public development interfaces. See the [Store guide](../../docs/app-store_en.md) for Store operations and installation transactions. [app_api.h](../../main/business/app_store/app_api.h) defines authoritative signatures; [app_runtime_math.def](../../main/business/app_store/app_runtime_math.def) defines allowed runtime imports.

## App catalog

- [Calculator](calculator/README.md): Decimal arithmetic with sequential evaluation.
- [Unit Converter](unit_converter/README.md): Length, mass, temperature and area conversions.
- [Calendar](calendar/README.md): Gregorian date browsing and selection.
- [Base Converter](base_converter/README.md): Unsigned 32-bit binary, octal, decimal and hexadecimal conversion.
- [English Reader](english_reader/README.md): Offline chapters, vocabulary cards and quizzes.
- [Package Check](package_check/README.md): Package download and on-demand resource diagnostics.
- [Pomodoro](pomodoro/README.md): Offline focus and break timer.
- [Gravity Maze](gravity_maze/README.md): Tilt controls, scoring and looping music.
- [Recorder](recorder/README.md): Single-slot recording, saving and playback.
- [Imaging](imaging/README.md): Camera preview, images and display effects.
- [Network tests](network_test/README.md): Protocol transfers, DNS/Ping and lifecycle checks.

A build target does not guarantee app compatibility. Each app documents required content dimensions and capabilities; producing a 397 ELF does not mean its layout runs on the 397.

## Development contents

- [1. Supported environment](#1-supported-environment)
- [2. Quick start](#2-quick-start)
- [3. Build and source organization](#3-build-and-source-organization)
- [4. ABI and lifecycle](#4-abi-and-lifecycle)
- [5. Capability interfaces](#5-capability-interfaces)
- [6. Icons and content assets](#6-icons-and-content-assets)
- [7. App coding rules](#7-app-coding-rules)
- [8. Publishing and device operations](#8-publishing-and-device-operations)
- [9. Validation and troubleshooting](#9-validation-and-troubleshooting)
- [10. Host implementation and licensing](#10-host-implementation-and-licensing)

## 1. Supported environment

- The 470 uses ESP32-S31 with the RISC-V `ilp32f` toolchain; the 397 uses ESP32-S3 with its matching Xtensa toolchain. Both produce ELF32, little-endian, ET_DYN packages with `XZ_APP_ABI_VERSION == 1`, but machine code is not interchangeable. Apps must also check content dimensions and board capabilities; successful compilation does not imply that every sample supports that board. Desktop ELF binaries and complete ESP-IDF firmware images are not app packages.
- The entry is `int app_main(int argc, char** argv)`. `argc == 1`; `argv[0]` points to `xz_app_api_t`, not a command-line string. Return zero for successful initialization or nonzero for failure.
- The host owns ELF loading, widgets, callbacks and cleanup. Registered callbacks may run after the entry returns; apps must not create permanent background tasks.
- The host provides managed black-and-white drawing, three-axis acceleration sampling, in-app back navigation, white-flash screen transitions, timed vibration, foreground audio playback, single-slot recording, resource images, foreground camera preview and explicit full refresh; see section 5. Managed HTTP/HTTPS, WebSocket/WSS, TCP/TLS and UDP interfaces are available; pedometer, background alarm, standby wake and arbitrary filesystem interfaces are not. Hardware presence or internal firmware classes do not grant ELF access. Store networking is not an app networking API.
- Native apps share the firmware execution environment and are not sandboxed. Install trusted apps only. SHA256 checks integrity, not publisher identity. LAN HTTP does not authenticate transport; do not expose the service publicly.

## 2. Quick start

Work from the repository root with the firmware-matched toolchain and Python 3 available as `python`. The standard script expects a directory and main C source with matching names, plus a 128×128 binary PBM (P4) icon. Create a new directory and save the complete example below as `tools/elf_app/hello/hello.c`:

```powershell
New-Item -ItemType Directory -Path ./tools/elf_app/hello
Copy-Item -LiteralPath ./tools/elf_app/calculator/icon.pbm -Destination ./tools/elf_app/hello/icon.pbm
```

```c
#include "app_api.h"

static const xz_app_api_t* app;
static void* value_label;
static unsigned count;

static void Count(void* context) {
    (void)context;
    count = (count + 1) % 10;
    const char text[2] = {(char)('0' + count), 0};
    app->set_text(value_label, text);
}

int app_main(int argc, char** argv) {
    if (argc != 1 || !argv || !argv[0]) return -1;
    app = (const xz_app_api_t*)argv[0];
    if (app->abi_version != XZ_APP_ABI_VERSION ||
        !XZ_APP_HAS_FIELD(app, set_text) || !app->label ||
        !app->button || !app->set_text ||
        app->width < 240 || app->height < 320) return -1;

    const int zh = app->language && app->language[0] == 'z';
    void* title = app->label(app->root, 24, 24, app->width - 48, 48,
                            zh ? "点击计数" : "Tap counter");
    value_label = app->label(app->root, 24, app->height / 2 - 40,
                            app->width - 48, 80, "0");
    void* button = app->button(app->root, 24, app->height - 100,
                            app->width - 48, 64,
                            zh ? "加一" : "Add one", Count, 0);
    if (!title || !value_label || !button) return -1;

    if (XZ_APP_HAS_FIELD(app, set_style) && app->set_style) {
        const xz_app_style_t style = {36, XZ_APP_ALIGN_CENTER, 0, 0, 0};
        app->set_style(value_label, &style);
    }
    return 0;
}
```

Replace the compiler placeholder and build; matching `readelf` and `objcopy` must be beside the compiler:

```powershell
$compiler = '<full path to riscv32-esp-elf-gcc.exe>'
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/hello -Target esp32s31 -OutputDirectory ./tools/elf_app/hello/build/470
```

Publish the generated `tools/elf_app/hello/build/470/hello.elf`. Building does not upload, publish or update device firmware. The copied icon is suitable for the example; replace it for your release.

For 397 apps, use the matching ESP32-S3 compiler and keep output separate:

```powershell
./tools/elf_app/build_app.ps1 -Compiler '<full path to xtensa-esp32s3-elf-gcc.exe>' -AppDirectory ./tools/elf_app/hello -Target esp32s3 -OutputDirectory ./tools/elf_app/hello/build/397
```

`-Target` defaults to `esp32s31`, preserving the existing output path. For `esp32s3`, the default output is `<app>/build/esp32s3`; `-OutputDirectory` overrides it. The script chooses architecture flags and matching binary tools, and checks runtime imports. A local installation manifest must use `target: esp32s3` and the generated ELF's size and SHA256. Install it with the ELF in the same device app folder. The 397 supports native monochrome output; image areas default to dithering, with 16/8-level modes unavailable.



## 3. Build and source organization

[build_app.ps1](build_app.ps1) compiles `<directory-name>.c`, with SDK and `tools/elf_app/common/` include paths. It runs an optional `build_content.py`, checks dynamic imports, then embeds `icon.pbm` and optional `assets.bin`. It does not automatically compile other `.c` files. Small modules may follow existing private header implementations; multi-source apps need an equivalent custom build process.

The standard flags are below. Custom builds must retain the target architecture, position-independent code, entry point, import validation and resource handling:

```text
-mabi=ilp32f
-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv
-nostdlib -nostartfiles -ffreestanding -fPIC -shared
-fdata-sections -ffunction-sections
-Wl,--gc-sections,-e,app_main,--strip-all
-fvisibility=hidden -Os
```

Do not assume imports for `printf`, `malloc`, `memcpy`, `memset`, the C++ standard library, ESP-IDF or LVGL. Compilers may generate helper calls for copying, zeroing or arithmetic. Builds reject unlisted imports. Adjust the code or provide required app-local implementations first. New shared imports require host review and coordinated runtime-list and firmware changes; do not only relax the build check. Pomodoro demonstrates local copy/zero implementations, and Calculator demonstrates allowed arithmetic helpers.

## 4. ABI and lifecycle

- Use the current header rather than a privately modified struct copy. ABI 1 grows by appending compatible fields. Do not reorder, remove or change existing field types; breaking changes require a new ABI version.
- Check the ABI, then `XZ_APP_HAS_FIELD(api, member)`, then capability bits and function pointers. An ABI 1 host may lack newer functions. Never read a tail member before checking its boundary.
- `root`, `width` and `height` describe the area below the system status bar. Lay out within those bounds; do not redraw the system status bar. The host adds no app title.
- Pass `host_context` to hosted services and `asset_context` only to resource reads. Do not interchange or fabricate them.
- All APIs and callbacks run on the UI thread. Entry, button, submit, timer and exit callbacks must return promptly without busy waiting, long computation or large synchronous I/O.
- Callback data must remain valid for the app lifetime, typically static state, never an entry-local address. Text creation and updates copy text. The host does not manage arbitrary app threads or asynchronous objects.
- On unload, page deletion or entry failure, the host stops the audio session and acceleration subscription, cancels timers, releases keep-awake and vibration holds, deletes the keyboard, calls the exit callback once, then releases widgets and the ELF. A nonzero entry result is retained for diagnostic logging; the host releases the ELF immediately and the loading page shows a run error. Standby is an overlay rather than exit and does not guarantee an exit callback. After wake, the host waits for the physical refresh to finish before resuming app timers, sampling and audio.

Check only capabilities the app needs. This example requires foreground timing; a label/button app need not demand every new capability:

```c
if (!XZ_APP_HAS_FIELD(api, set_keep_awake)) return -1;
const uint32_t required = XZ_APP_CAP_TIME | XZ_APP_CAP_TIMER |
                          XZ_APP_CAP_EXIT | XZ_APP_CAP_KEEP_AWAKE;
if ((api->capabilities & required) != required || !api->host_context ||
    !api->monotonic_ms || !api->timer_start || !api->timer_cancel ||
    !api->set_on_exit || !api->set_keep_awake) return -1;
```

### ELF loading flow

The host rechecks the local manifest, file size, SHA256, target and ABI, then streams the ELF, allocates loadable segments, relocates and resolves imports. It does not keep a whole-package buffer. The runtime invokes the entry and shows app content after successful initialization and the first loading-page submission. A failed entry leaves a returnable error page, stops managed services, runs exit cleanup, deletes partial widgets and releases the ELF. Unsupported relocations or imports prevent entry execution. Native ELF code shares the firmware address space; validation is not a security sandbox.

App back handling precedes default navigation; HOME remains system-owned. Timers, sampling and audio resume after the physical display is ready following standby.

## 5. Capability interfaces

`capabilities` describe implemented services, not current SD, clock or resource readiness. Check each operation result. Use the descriptions below with the header's full signatures.

### Basic UI

- `label`, `button` create widgets under a parent and return opaque handles. Button `context` identifies app state; presses automatically bind the system click vibration and follow the system vibration switch. Do not cast handles to LVGL objects.
- `set_text` copies text into labels, buttons or input fields, not containers or progress bars.
- Optional `set_style` controls font size, alignment, radius, border and black-filled white-text appearance. Zero font size preserves the font. It does not load font files or provide arbitrary colors or animation.
- Optional `set_enabled` controls the disabled state, not visibility. Disabled buttons do not replace useful error feedback.

### Time: `XZ_APP_CAP_TIME`

Legacy optional `get_local_date` returns 1 on success and 0 for unready/invalid input; it does not use zero-success semantics. `get_local_time` returns `XZ_APP_OK` with local date/time, weekday (Sunday is 0) and UTC epoch seconds, or `XZ_APP_ERR_NOT_READY` for an unready clock. `monotonic_ms` measures the current boot without clock-adjustment jumps; continuity across reboot or deep sleep is not guaranteed.

### Timers: `XZ_APP_CAP_TIMER`

`timer_start(host, interval_ms, repeat, callback, context)` supports 200ms–24h with eight concurrent timers. Zero `repeat` is one-shot; nonzero repeats. It returns a nonzero ID or zero on failure. One-shot timers are removed before invocation. Callbacks may cancel themselves or create timers. `timer_cancel` returns `XZ_APP_ERR_NOT_FOUND` for unknown or completed IDs. IDs belong to the current run only.

These are foreground callbacks, not background alarms. Standby/inactive pages skip callbacks without replaying missed ticks or waking the device. Compute countdowns from monotonic elapsed time, not tick counts, and define how standby affects remaining time. Timers alone do not inhibit standby.

### Private storage: `XZ_APP_CAP_STORAGE`

`storage_read/write/remove` accept keys of 1–48 ASCII letters, digits, underscores or hyphens, not paths. Each app may store 32 keys of up to 16KiB. Reads return actual byte counts; a null buffer queries size. Insufficient capacity is an error, not truncation. Missing keys return `XZ_APP_ERR_NOT_FOUND`. A zero-length value is valid and is not a missing key.

Writes and removes return result codes; a zero-length write may use a null data pointer. Data lives at `SD_APP_ROOT/settings/apps/<app-name>--<appId>/<key>.bin` in the [SD layout](../../main/infra/storage/sd_paths.h), separate from installation. Updates retain state; uninstall deletes state, recovery backups and staging files. FAT case rules apply; lowercase keys are recommended. SD export or standby may report not ready. Do not bypass the API. Writes stage data at `SD_APP_ROOT/temp/app_store/<app-name>--<appId>/data-<key>.bin.part` and clean it after success or failure. Recovery backups, `<key>.bin.cloud-backup`, remain in the state directory. Reads may fall back to an interrupted-write backup; arbitrary power loss is not guaranteed safe. Staging files left by power loss are never used for recovery; writing the same key replaces them, or they can be removed while the app is inactive. Names follow the manifest's fixed `installedName`, and IDs isolate apps sharing a name. `storage_remove` deletes the key's committed file, backup and staging file.

Version and validate stored formats, including length and checksums when appropriate, before restoring state. Save meaningful interactions and rate-limit periodic writes. Do not write every frame/second or rely solely on exit saving.

### Additional widgets: `XZ_APP_CAP_WIDGETS`

- `container`: zero `scroll_y` creates a fixed region; nonzero allows vertical scrolling. Compose lists from containers, labels and buttons; there is no separate list data model. Use the drawing interface for black-and-white graphics.
- `clear_children`: deletes all children of a container without deleting the container itself; never call it inside a draw event.
- `input`: single-line input with 1–256 characters and an optional submit callback. Character limits are not UTF-8 byte capacities; allocate buffers accordingly.
- `get_text`: labels, buttons and input fields; returns UTF-8 byte length excluding the terminator. Null buffer queries size. Add one for actual capacity. Insufficient capacity returns `XZ_APP_ERR_LIMIT` and clears the output when space permits.
- `keyboard`: explicitly bind an input; fields do not open it automatically. One per app, system English/symbol layout, no Chinese IME. The host initializes keys; apps do not call internal LVGL/keyboard functions. It aligns to its parent's bottom. Reserve visible space without covering the input.
- `keyboard_close`: cancels pending confirmation and deletes the keyboard, allowing a new input binding. Submit callbacks may read and close it. Deleting the input first leaves no dangling writable target.
- `progress`, `set_progress`: 0–100 with values above 100 clamped; no animation or automatic percentage text.

### Exit: `XZ_APP_CAP_EXIT`

`set_on_exit` registers one short callback, replacing the prior registration; null clears it. It may attempt to save but cannot start timers, open a keyboard or reacquire keep-awake. Power loss, crashes and unavailable storage do not guarantee callback execution or saving.

### Notifications: `XZ_APP_CAP_NOTICE`

`notify` copies text into the standard top notification while the app is foreground and not in standby. Do not use it for continuous progress, counters, countdown ticks or debug spam. Supply text for the current `language`.

### Keep awake: `XZ_APP_CAP_KEEP_AWAKE`

`set_keep_awake(host, enabled)` inhibits automatic idle standby without keeping networking or audio active, and does not prevent manual power-key standby. Repeated enable calls do not accumulate holds. Disable on pause; unload and entry failure release it automatically. Use only when required, not as a default permanent hold.

### Drawing: `XZ_APP_CAP_DRAWING`

`drawing(parent, x, y, width, height)` creates a black-and-white drawing area up to 1024×1600 and returns an opaque handle. The host owns the pixel buffer and frees it with the widget. `draw(widget, commands, count)` copies commands into the host canvas: at most 1024; zero commands clears it to white. Each `xz_app_draw_t` describes a line (kind 0), rectangle (kind 1) or ellipse (kind 2). Coordinates are relative to the drawing area and must stay inside it; rectangles and ellipses include both corners. Line width is 1–8; nonzero `filled` fills rectangles and ellipses black; `reserved` must be zero. Line endpoints may point in any direction. Any invalid command rejects the whole batch with `XZ_APP_ERR_INVALID` and leaves the canvas unchanged. Drawing suits low-frequency redraws such as mazes and boards, not per-frame animation.

### Acceleration: `XZ_APP_CAP_ACCELERATION`

`set_acceleration(host, enabled)` subscribes to three-axis acceleration in mg — gravity, not angular velocity. `get_acceleration` returns a sample from the last 500ms; without a subscription, outside the foreground, in standby, or with an expired sample it returns `XZ_APP_ERR_NOT_READY`. Coordinates are not auto-rotated; in portrait, positive X points down and positive Y points left. Sampling runs only in the foreground, stops automatically on unload and standby, and resumes after wake once the refresh finishes. Disable the subscription when not needed to avoid continuous sensor drain.

### Foreground time and back navigation

`foreground_ms(host)` returns accumulated foreground milliseconds; standby, white-flash waits and background pages do not count. Use it for actual play time. `set_on_back(host, callback, context)` registers back-key handling: nonzero means the app consumed the key (for example returning from a level to a menu); zero falls back to the host default of leaving the page. A later registration replaces the earlier one; null clears it. The HOME key always follows system navigation.

### Screen transition: `XZ_APP_CAP_SCREEN_CHANGE`

`request_screen_change(host)` asks the next physical commit to flash white before showing the target screen; it does not block the UI. After the request the host pauses app timers, sampling and audio until the white-flash frame physically completes, so business logic never runs over a stale image. Use it for whole-screen changes such as level switches; do not call it frequently.

### Vibration: `XZ_APP_CAP_VIBRATION`

`vibrate(host, duration_ms)` requests a single 1–15000ms pulse and respects the system vibration switch and intensity. With the switch off it is a no-op returning success. While a previous pulse is still running it returns `XZ_APP_ERR_BUSY` without stacking. The host inhibits inactivity standby during the pulse and releases it automatically; power-key standby remains under system control.

### Audio playback: `XZ_APP_CAP_AUDIO`

Managed foreground playback, mutually exclusive with recording. `audio_get_caps` reports the supported formats, sources and output parameters. Ogg-wrapped mono Opus (`XZ_APP_AUDIO_OGG_OPUS`) and 16kHz mono 16-bit PCM WAV (`XZ_APP_AUDIO_WAV_PCM`) are supported, from the app asset section (`XZ_APP_AUDIO_ASSET`, bounded by `offset`/`length`) or a relative path under the product SD root (`XZ_APP_AUDIO_SD`, validated per segment; `.`, `..` and invalid file names are rejected). `audio_play` accepts the request and a host background task opens, decodes and outputs it; success only means the request was accepted. A new request replaces current playback. `audio_pause` pauses or resumes — pausing closes the file and returns the audio hardware; `audio_stop` requests a stop with background cleanup. `audio_set_volume` sets the shared system volume 0–100, taking effect and persisting immediately. `audio_get_status` reports state, result code, position, loop count and current system volume, copied according to the caller's `struct_size`.

System audio such as assistant sessions and recording tests takes priority: preempted playback fails or stops with `XZ_APP_ERR_BUSY` in `result`. Standby, leaving the page and white-flash waits pause playback and return the hardware automatically; it continues once the app is foreground again. During playback the host keeps PA and operating frequency and inhibits inactivity standby; apps should still stop unneeded playback before exit.

### Managed networking: `XZ_APP_CAP_NETWORK`

Check the `network_close` field boundary, capability and function pointers first. `network_open(host, config, &id)` copies all inputs and starts asynchronously; success means acceptance. Set the config `struct_size`. HTTP/HTTPS and WS/WSS use full URLs; TCP/TLS/UDP use a hostname or IPv4 address and separate port. IPv6, UDP binding, broadcast, multicast, MQTT and listening servers are not exposed.

`network_get_status` copies up to the caller's `struct_size`. `network_send` copies one pending payload; acceptance does not mean transmission. Wait for `pending_send` to clear before the next send. HTTP takes its method, headers and body in `network_open` and does not use `network_send`. Supported methods are GET, POST, PUT, PATCH, DELETE and HEAD. Redirects and retries are not automatic. A 4xx/5xx status is a received HTTP response; inspect network result and `http_status` separately. Cancellation or timeout does not prove a server-side write did not execute.

`network_read` returns the byte count read. No record returns `XZ_APP_ERR_NOT_READY`; zero is a real empty message. TCP/TLS merge consecutive receive data into a stream record, read up to the caller's `capacity` and retain unread bytes; nonempty data rejects a zero-capacity read. HTTP, WS and UDP each read one complete record: check `queued_messages` and `next_length` and provide enough space. Insufficient space returns LIMIT without removing that record. HTTP/TCP/TLS chunks are not application message boundaries; accumulate and frame them in the app. WS and UDP preserve message boundaries. WS text has zero flags; binary messages and other protocols set `XZ_APP_NET_BINARY`. A full HTTP receive buffer applies backpressure until the overall deadline; other protocols report a limit and close, without silent truncation or drops.

The ELF specifies `receive_buffer_size`, `send_buffer_size` and HTTP `response_size_limit` for each request. Receive capacity must be positive; send capacity may be zero, and an HTTP request body must fit that capacity. A zero response limit permits streaming subject to receive capacity and deadline, without a fixed total-length cap. The host checks integer ranges, addition overflow and actual allocation results, without applying a fixed internal-RAM floor or a per-worker reserve multiplier to app requests. WS capacities may not exceed the existing implementation's 65535-byte limit; IPv4 UDP capacities may not exceed 65507 bytes. These are transport implementation or protocol boundaries. Each app may retain four handles; at most four workers may be active globally, including detached cleanup. Each connection has 32 record slots. URLs are at most 1024 bytes; TCP/TLS/UDP hosts at most 253 bytes. Up to eight request headers have names of at most 64 bytes and values of at most 512 bytes. Header line breaks, URL user information and overrides of transport or WS handshake control headers are rejected.

`network_get_header` reads only cached HTTP Content-Type, Content-Length, ETag, Last-Modified, Location, Retry-After, Content-Range and Accept-Ranges values. Each value is at most 1024 bytes; names are case insensitive. Capacity includes the terminator; returned length excludes it. Headers not received return NOT_READY, absent values NOT_FOUND and uncached names UNSUPPORTED.

`timeout_ms` is 1000–60000ms. HTTP uses an overall deadline from acceptance; other protocols use it for initial network readiness, connecting and each send, without an idle lifetime limit. DNS and backend shutdown still have their own blocking constraints; cancellation is not instantaneous. TLS verifies server identity using system roots by default. Optional `ca_pem` supplies up to 8192 bytes of PEM roots scoped to that connection. There is no verification bypass. The host adds no device identifier, system credentials or cloud business headers to app destinations.

`network_close` immediately revokes a handle and requests background cleanup. Queued records in COMPLETE, CLOSED or ERROR remain readable until close. Exit, leaving, standby, system updates and white-flash foreground suspension revoke all handles. Apps reconnect after resume; sends are never automatically replayed. Workers hold host data only and never invoke ELF callbacks. Standby and power-off wait for cleanup before hardware changes. System updates reject new app network work and wait for existing workers to drain before flash writes or firmware downloads; a drain timeout stops preparation before modifying the firmware partition. Actual allocation or task-creation failure returns MEMORY; exhausted active slots return BUSY. Firmware RAM diagnostic thresholds do not gate ELF network requests.

`network_prepare(host, timeout_ms, &id)` reuses system network preparation to resume and wait for networking, without sending HTTP, WebSocket or other business requests or creating a remote application connection. Check the field boundary, capability and pointer before calling it, including from `app_main`. Success means asynchronous acceptance. The timeout is 1000–60000 milliseconds. Handles share ordinary request slots and use `network_get_status` and `network_close`: OPENING means pending, COMPLETE means system networking is ready, and ERROR reports failure. Readiness does not guarantee Internet or destination reachability. Closing requests cancellation, but the underlying wait may continue until its deadline. Preparation changes no saved network configuration and does not override manual Wi-Fi off.

After the first accepted `network_open` or `network_prepare`, a foreground app keeps a soft network hold by default, including gaps between requests. Check the `network_set_idle_timer` field boundary and pointer before use. `network_set_idle_timer(host, 1)` releases the page hold so system idle-disconnect timing applies when no network workers remain; `network_set_idle_timer(host, 0)` restores continuous foreground holding. Only 0 and 1 are valid; repeated calls do not accumulate references. This changes neither system settings nor the last user-activity time, so enabling an already-expired timer can disconnect immediately. Leaving, standby, white-flash suspension and exit release the page hold. Returning to the foreground retains the current session's timer choice. Active requests and cleanup keep their own soft holds. Neither hold prevents automatic standby or overrides manual Wi-Fi off.

#### DNS and ICMP Ping

`XZ_APP_CAP_DNS` and `XZ_APP_CAP_PING` advertise independent address resolution and ICMP Echo. Check capability bits, field boundaries and pointers first. Both use the system lwIP IPv4 network and share the existing four handles and workers, foreground holding, cancellation and cleanup. They change no DNS-server, Wi-Fi or trust settings and do not use cellular-module AT commands.

`network_resolve(host, hostname, timeout_ms, &id)` asynchronously returns the system resolver's first IPv4 address. Names are at most 253 bytes and accept ASCII letters, digits, dots, hyphens and underscores; use Punycode for internationalized names. The 1000–60000ms deadline includes network preparation and resolution. Literal IPv4 input may return without a DNS query, and names may resolve from the system cache. IPv6, multiple-address enumeration, custom DNS servers and other record types are not exposed. Native `getaddrinfo` cannot be forcibly interrupted: closing revokes the handle immediately, but the worker slot and hold remain until it returns. Cancellation and deadline checks discard late results.

`network_ping(host, config, &id)` accepts `xz_app_ping_config_t`. Set `struct_size`, target, network-preparation deadline, count, interval, per-reply timeout, payload size and TTL. Network preparation and resolution have a 1000–60000ms deadline. The ELF chooses the probe count; zero means continuous until close. The interval may be zero; the reply timeout must be positive. Payload sizes of 0–65507 bytes follow IPv4 limits; TTL is 1–255. Payload memory is allocated as requested without applying firmware RAM diagnostic margins. Ping runs in the existing host worker, checks cancellation and closes its socket, without adding a detached background task.

Use `network_get_status` for progress and `network_get_diagnostics` for `xz_app_net_diagnostics_t`. Set the output `struct_size`; the host copies up to that capacity. DNS is OPENING while preparing or resolving and COMPLETE after obtaining an address. Ping is OPENING while preparing or resolving, then CONNECTED while probing. A finite run completes as COMPLETE if at least one valid reply arrives; no replies produce ERROR/TIMEOUT. Inspect `sent`, `received` and `timeouts` for partial loss; completion does not mean every probe succeeded. Statistics include address, sequence, reply TTL, last/minimum/maximum/average round-trip milliseconds and operation duration. Millisecond precision permits zero; RTT fields remain zero without replies. Packet counters saturate at UINT32_MAX. Statistics are readable during a run and unavailable after close. Probe handles do not use `network_send`, `network_read` or HTTP header access.

ICMP replies depend on routing, target and firewall policy; no reply does not prove HTTP or another service is unavailable. The operating system handles ICMP rather than Service Lab. Choose a known target allowing ICMP. Received replies are checked for source, ICMP type, identifier, sequence, checksum and echoed payload.

Networking adds `XZ_APP_ERR_TIMEOUT` and `XZ_APP_ERR_CANCELLED`; `error_code` is an optional numeric backend code. Byte counters include app bodies or payloads only and saturate at UINT32_MAX. They exclude protocol overhead and do not prove peer processing. These interfaces do not sandbox native ELF code. See the [network test app](network_test/README.md).

### Return values

`XZ_APP_OK` is zero. Errors are `XZ_APP_ERR_INVALID` for arguments, `XZ_APP_ERR_NOT_READY` for unavailable resources, `XZ_APP_ERR_NOT_FOUND` for missing objects, `XZ_APP_ERR_LIMIT` for capacity, `XZ_APP_ERR_IO` for storage, capture or unclassified decoding failures, `XZ_APP_ERR_MEMORY` for allocation failure, `XZ_APP_ERR_UNSUPPORTED` for unsupported formats or features, and `XZ_APP_ERR_BUSY` for resources held by other services. Not all functions use these results: check null widget handles, zero timer IDs, nonnegative text/storage byte counts and 1 from `get_local_date`. Void functions have no status. Nonzero does not universally mean failure.




### Single-slot recording

Query input support with `record_get_caps`. `record_start` prepares the microphone and temporary file asynchronously; `record_pause` pauses or resumes writes while retaining the microphone. `record_stop` requests asynchronous saving, and `record_get_status` reports state, effective duration, result and the saved relative path. Before accessing the appended ABI fields, check the `record_get_status` field boundary, `XZ_APP_CAP_RECORDING` capability and function pointers.

The format is fixed to 16kHz mono 16-bit PCM WAV, with one recording per app. A successful save replaces the previous recording; preparation failures, empty takes and write failures preserve it. The host controls the private path and exposes no arbitrary file writes. Recording and playback are mutually exclusive and return `XZ_APP_ERR_BUSY` when occupied. System audio takes priority and preemption ends recording. Leaving the page, standby or exit stops capture and saves in the background; a stop request does not mean saving has completed. Recording inhibits inactivity standby but permits power-key standby.

### Images and display effects

Check `XZ_APP_CAP_IMAGE`, the `image_get_status` field boundary and function pointers before creating an aspect-fit centered image with `image(host, parent, x, y, width, height)`. Each app may create four regions, each side limited to 1600 pixels and the area limited to 1 Mi pixels. Image regions do not scroll and retain the system pull-down gesture.

`image_load_asset(host, widget, offset, length)` asynchronously reads one complete image from `.xz.app.assets`; the encoded range must be 1 byte to 4 MiB. Supported inputs are baseline/progressive JPEG, restricted PNG, A2I1, A2L8 and the existing EBGR grayscale format. PNG supports non-interlaced 8/16-bit grayscale or true color and 8-bit indexed color; transparency is flattened onto white. The host retains bounded L8 original pixels and produces a separate display copy. Success means accepted; query `loading` and `result` using `image_get_status`. Failure preserves the previous display. New requests, camera selection and widget deletion invalidate older results; app exit stops delivery.

Check `XZ_APP_CAP_IMAGE_TONE`, the `image_get_effects` field boundary and function pointers. Query the current `xz_app_image_effects_t`, change the required fields, then call `image_set_effects`. Every request starts from retained original L8 pixels without accumulating transform errors or changing other system images' fixed darkening policy. Failure preserves the previous original and display images.

- Tone: `contrast`, `brightness`, `shadows` and `highlights` are -100–100%, defaulting to 0. They control contrast around mid-gray, a full-range brightness offset, a shadow-weighted curve and a highlight-weighted curve. Shadow and highlight adjustments preserve black and white endpoints.
- Software exposure: `exposure` is -200–200 in units of 0.01 EV, defaulting to 0. It approximately linearizes L8 using sRGB, multiplies by 2^EV and re-encodes the result. It changes neither the sensor nor details already lost in the source.
- Gamma: `gamma` is 25–400 in units of 0.01, defaulting to 100. The exponent is 1/Gamma; increasing it brightens midtones.
- Filters: `sharpness` is -100–100%, defaulting to 0. Negative values blend toward a 3×3 box blur; positive values sharpen using that neighborhood. `invert` is 0 or 1, defaulting to 0.
- Geometry: `rotation` is clockwise 0, 90, 180 or 270 degrees; `mirror_x` and `mirror_y` are 0 or 1. `crop_x/y/width/height` use ten-thousandths of the original L8 dimensions, defaulting to `(0,0,10000,10000)`. Crop dimensions must be positive and stay within the source; the smallest result is one pixel. Cropping precedes mirroring and rotation; the result is aspect-fit and centered in its widget. Cropping uses the bounded decoded L8 image and does not re-decode a higher-resolution source.
- Output: `mode` selects 16 gray levels, 8 levels, 4×4 ordered black-and-white dithering or binary output. `threshold` is a binary conversion parameter, 0–255; -1 uses midpoint 128. It affects binary mode only, independently of the system's text I1 threshold, and does not adjust shadows. Eight-level output does not change waveforms or guarantee faster refresh. Unsupported native gray modes return `XZ_APP_ERR_UNSUPPORTED`; monochrome boards default to dithering.

Tone processing applies software exposure, brightness, shadows, highlights, contrast, Gamma and inversion, then neighborhood filtering and finally quantization or dithering at output coordinates. Every stage clamps to the valid range. The host uses a fixed 256-entry table and a separate display buffer; disabled filters perform no neighborhood sampling.

### Custom pixel algorithms

Check `XZ_APP_CAP_IMAGE_PIXELS`, the `image_set_pixels` field boundary and the relevant pointers before processing tightly packed L8 pixels in app-owned buffers. L8 uses one byte per pixel, 0 black and 255 white, without a palette or row padding.

`image_read_pixels(host, widget, processed, offset, buffer, length)` copies pixels: `processed=0` reads the original, and 1 reads the display image. Each call is limited to 64 KiB and returns bytes copied or a negative error; EOF returns 0. `image_get_status.source_width/height` and `width/height` describe those views; `revision` increases after successful updates. When reading chunks across UI callbacks, pause preview and avoid source/effect changes, or compare revisions before and after reading and retry if they differ. Never continue calling after leaving the app.

`image_set_pixels(host, widget, width, height, pixels, length)` copies an app buffer into a new original image without retaining its pointer. Each side is limited to 1600 and the area to 1 Mi pixels; length must equal width times height. Current widget processing parameters then apply. It stops camera delivery to that widget and invalidates older decode requests. To display pixels already processed by the app, first reset the corresponding tone and geometry parameters to neutral values; host output quantization still applies.

Apps can implement their own threshold algorithms, histograms, edge detection, text overlays or watermarks without a new host endpoint for every algorithm. Processing operates on bounded grayscale images; RGB hue/saturation, JPEG/PNG encoding/export and arbitrary filesystem access are not provided.

### Camera preview and full refresh

After checking `XZ_APP_CAP_CAMERA`, call `camera_preview(host, widget, 1)` to deliver current foreground camera frames to an image region; zero stops preview. Capture reuses one hardware session and releases the display DMA gate between frames. Only the latest result is retained, with at least 800ms between deliveries and the next capture. Actual display rate depends on capture and panel refresh, and is not a video frame rate. Image effects also apply to camera frames; stopping preserves a frame for adjustment. `camera_get_status` reports running state, result and delivered frames, not physical panel refreshes.

Standby, navigation and exit cancel capture. Background teardown does not access ELF memory; preview resumes on demand after waking when the page is interactive. Capture failures stop automatic retry; the app may enable preview again. Stopping and immediately restarting invalidates the old task and its pending frames; its completion cannot overwrite the new request. Allocation or memory budget failures return `XZ_APP_ERR_MEMORY`; unsupported encodings return `XZ_APP_ERR_UNSUPPORTED`. Preview writes no photos or recordings and requests no network access. An implemented interface does not imply a camera is connected. USB cameras remain mutually exclusive with USB mass storage.

Check `XZ_APP_CAP_FULL_REFRESH` before `request_full_refresh(host)`. The host marks the next commit for full refresh and coalesces duplicate pending requests. The call preserves content and does not wait for physical completion. The 470 and 397 use their respective full-refresh implementations, independently of white-flash animations. Requests require the app to be foreground and outside standby.

## 6. Icons and content assets

Icons live in `.xz.app.icon` inside the ELF; no separate image upload is needed. The script converts `icon.pbm` with [build_icon.py](build_icon.py) to XZIC and adds a non-loadable, read-only PROGBITS section. The host reads it without executing the app. Legacy packages without icons use a generic icon; the standard script requires the PBM.

XZIC v1 is little-endian: bytes 0–3 are `XZIC`; uint16 values at offsets 4/6/8/10 are version 1, width 128, height 128 and format 1. A uint32 at offset 12 is payload length 2048. Pixels start at offset 16, row-major and MSB-first, with 1 black and 0 transparent. Total length is 2064 bytes. Listing metadata is not part of this icon format.

Place large dictionaries or content in `assets.bin`; the script adds `.xz.app.assets` without SHF_ALLOC or compression. Check the `asset_context`, `asset_size`, `read_asset` boundaries and function pointer. `read_asset(asset_context, offset, buffer, length)` returns the requested length or zero on failure. Validate `offset <= asset_size` and `length <= asset_size - offset`, read small synchronous chunks and parse the app-defined format.

The loader streams the package rather than allocating package-sized PSRAM, but executable/data segments and relocation still consume memory. The 32MiB package limit is not a RAM guarantee. Do not place bulk content in ordinary loadable constants or permanent copies. See `package_check` and `english_reader` for asset builds and on-demand reads. Asset-sourced audio playback references byte regions inside `.xz.app.assets` directly; the host reads them in the background, so apps neither copy audio into loadable segments nor read the file themselves.

## 7. App coding rules

- Separate business logic, UI and resource formats while reusing the existing framework. Keep core calculations/state machines host-independent where practical; avoid heavy frameworks for small apps.
- Validate external lengths, ranges, versions and results. Size UTF-8 buffers in bytes including termination, and avoid formatting overflow.
- Check widget creation and use only necessary objects. Update changed content rather than rebuilding screens. E-ink is unsuitable for high-frame-rate animation or per-second full-screen rebuilds/notifications.
- Lay out from `width` and `height` and check actual device sizes. Use fixed containers for fixed regions and vertical scrolling for real long lists. Areas without vertical overflow should allow the system pull-down menu rather than consume gestures solely due to scroll flags.
- Provide Chinese and English text using `language`. Common UI sizes are 25/30; math subsets at 18/28/36 are not guaranteed to contain Chinese. Availability depends on installed fonts; do not request arbitrary sizes or assume font synthesis.
- Keep references within the app lifetime and do not bypass hosting with drivers, threads, LVGL or filesystem calls. Sharing an address space does not authorize bypassing resource management.
- Do not expose URLs, tokens, headers, configuration, private content or user data in notices/logs. The host uses DEBUG/INFO/WARN/ERROR for necessary diagnostics. No public app logging callback exists; `printf` and `ESP_LOGx` are not assumed imports.
- Ensure redistribution rights for content, icons, dictionaries and dependencies. Retain required notices/licenses. Checksums do not replace source review.

## 8. Publishing and device operations

1. Build the ELF with a stable app ID: a lowercase letter followed by lowercase letters, digits, underscores or hyphens, up to 48 characters. Versions are three nonnegative numeric components, `major.minor.patch`, without `v`, prerelease suffixes or leading zeroes except zero itself; each component has at most six digits.
2. Upload through the app store administration interface. Provide bilingual names, summaries, categories, descriptions and release notes, plus languages, provider and version. The server manages this listing metadata; it is not inferred from arbitrary ELF strings. It generates `uploadedAt`. Icons and optional content are embedded in the ELF.
3. Publish after uploading. An app ID/version pair cannot be overwritten; bump the version when content changes. Server target/ABI filtering does not replace entry capability checks.
4. Connect the device and verify service configuration as described in the [Store guide](../../docs/app-store_en.md#usage). The platform must confirm fixed-MAC binding and board, chip, firmware and ABI compatibility. Firmware flashing and ELF installation are separate. Upgrade firmware first when new interfaces are required.
5. Refresh the Store, install, and open from the Store or home. Tap an app row for details. Multiple installs queue; tap a progress button to cancel. Leaving cancels incomplete downloads.
6. Check for updates in details and confirm installation. Unpublishing keeps local installations. Uninstall deletes private state, recovery backups and staging files. Back up important data; do not edit installation manifests as a substitute for updates.

Packages are limited to 32 MiB. Supply bilingual publication metadata and follow administration-side validation. See the [Store guide](../../docs/app-store_en.md) for catalogs, installation and updates.

### Sources, build outputs and installation test packages

Keep sources, original assets, build scripts and bilingual READMEs in `tools/elf_app/<app>/`. ELF binaries, installation JSON, build directories and generated assets are excluded from Git. The shared delivery location for installation test packages has not been decided; a build output directory is only for builds.

Keep 397 and 470 outputs separate. Each installation test ELF needs a matching `manifest.json` for that model, with current target, version, byte size and SHA256. The build script produces ELF/resources, not installation JSON. Store-produced installation metadata and hand-authored local manifests require their own checks; recalculate size and digest after rebuilding.

### Local installation manifest example

This is a local installation format, not a cloud service protocol. Run after building Hello from the repository root. Replace the example `id`, names and version with the actual package identity for release. For 397, select its output and `esp32s3`, and ensure the app supports its dimensions.


```powershell
$ErrorActionPreference = 'Stop'
$elfPath = './tools/elf_app/hello/build/470/hello.elf'
$manifest = [ordered]@{
    id = '0123456789abcdef0123456789abcdef'
    appId = 'hello'
    name = 'Hello'
    nameEn = 'Hello'
    description = 'Local installation example'
    descriptionEn = 'Local installation example'
    installedName = 'hello'
    version = '0.1.0'
    target = 'esp32s31'
    abiVersion = 1
    size = (Get-Item -LiteralPath $elfPath).Length
    sha256 = (Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash.ToLowerInvariant()
}
$jsonPath = Join-Path (Split-Path -Parent $elfPath) 'manifest.json'
$json = $manifest | ConvertTo-Json
[IO.File]::WriteAllText($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($jsonPath), $json, [Text.UTF8Encoding]::new($false))
```

Copy `hello.elf` and `manifest.json` together into SD `metalio/e-ink/apps/hello/`. This is an installation path, not the shared delivery location. Match `installedName`, directory and ELF basename. Local `size` is a JSON number and SHA256 uses lowercase hexadecimal. Regenerate after every rebuild rather than only changing the version or reusing an old manifest. See [Store data handling](../../docs/app-store_en.md).

## 9. Validation and troubleshooting

Validate entry results, old-host rejection/fallback, both languages, target-size layout, buttons/scroll gestures, repeated entry/exit, timer cancellation, keyboard reopen/close, missing/corrupt storage, SD export/unavailability and save compatibility after updates. Timing apps also check pause/resume, phase boundaries, clock adjustments and standby semantics. Simulations do not replace hardware input, e-ink refresh, power and full-flow tests.

- `Unsupported runtime symbol`: inspect generated imports; do not only edit the allowlist.
- Installed but not runnable: check target, ABI, capabilities and entry result. Inspect operation/stage logs from `AppStore`, `AppRuntime`, `AppServices` without exposing full URLs or tokens.
- Font warnings/missing glyphs: check installed sizes and glyph coverage rather than suppressing warnings.
- No keyboard: explicitly call `keyboard`, check parent visibility, target, existing keyboard and returned handle. The host initializes keys.
- No date: handle `get_local_date == 0` or `get_local_time` returning `XZ_APP_ERR_NOT_READY` instead of assuming a valid today.
- Save failure: distinguish first-use absence, capacity, invalid data and SD readiness. Provide useful recovery feedback without overwriting data that should be retained.
- HTTP 404: check that the selected service supports the App Store. The store URL is managed by `cloudzao_app_store_url` in `main/infra/network/endpoints/cloudzao_endpoints.c`.

## 10. Host implementation and licensing

Host implementations are in the [runtime page](../../main/ui/apps/app_store/app_store_runtime.cc), [foundation services](../../main/ui/apps/app_store/app_runtime_services.cc) and [storage/loader](../../main/business/app_store/app_store.cc). These are host maintenance entry points, not internal APIs third-party ELFs may link directly.

ELF Loader in `components/elf_loader` is sourced from Espressif esp-iot-solution commit `5fbb2597b88f21ec41e952f81292ccd6ae569761`, retaining Apache-2.0 licensing; local CMake explicitly declares lwIP. Reading content provenance/licenses are documented in the [English Reader attribution](english_reader/README.md). ELF packaging does not remove required license appendices.

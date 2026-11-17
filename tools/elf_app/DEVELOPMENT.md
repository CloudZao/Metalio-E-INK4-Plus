# ELF App Developer Guide

[中文](DEVELOPMENT_zn.md) | **English**

This guide covers third-party ELF app development, builds and publishing. [app_api.h](../../main/app_store/app_api.h) is authoritative for interfaces; [app_runtime_math.def](../../main/app_store/app_runtime_math.def) defines permitted compiler runtime imports. See the [Store user guide](../../docs/app-store.md) for Store operations and network configuration, and [sample apps](README.md) for existing sources.

## 1. Supported environment

- The target is ESP32-S31, using the firmware-matched RISC-V toolchain. Packages are ELF32, little-endian, RISC-V, ET_DYN with `ilp32f`; the app ABI is `XZ_APP_ABI_VERSION == 1`. Desktop ELF binaries, other chip firmware and complete ESP-IDF firmware images are not app packages.
- The entry is `int app_main(int argc, char** argv)`. `argc == 1`; `argv[0]` points to `xz_app_api_t`, not a command-line string. Return zero for successful initialization or nonzero for failure.
- The host owns ELF loading, widgets, callbacks and cleanup. Registered callbacks may run after the entry returns; apps must not create permanent background tasks.
- There are no public audio, HTTP, accelerometer, pedometer, background alarm, standby wake, arbitrary filesystem or general image-widget interfaces. Hardware presence or internal firmware classes do not grant ELF access. Store networking is not an app networking API.
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
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/hello
```

Publish the generated `tools/elf_app/hello/hello.elf`. Building does not upload, publish or update device firmware. The copied icon is suitable for the example; replace it for your release.

## 2.1 Use Calculator as a template

Read [calculator.c](calculator/calculator.c) and [calculator_engine.h](calculator/calculator_engine.h) first. The C file handles entry, ABI checks, widget layout, click callbacks and display updates; the private header handles input and calculation state without calling host widgets. `Click` passes keys to `CalcInput`, then `Show` updates the result, expression and hint. `ApplyStyle` uses optional host styling when available.

Build the original calculator from the repository root:

```powershell
$compiler = '<full path to riscv32-esp-elf-gcc.exe>'
./tools/elf_app/calculator/build.ps1 -Compiler $compiler
```

The output is `tools/elf_app/calculator/calculator.elf`. To start another app, copy the directory and keep the directory name and main source name consistent:

```powershell
Copy-Item -LiteralPath ./tools/elf_app/calculator -Destination ./tools/elf_app/my_app -Recurse
Rename-Item -LiteralPath ./tools/elf_app/my_app/calculator.c -NewName my_app.c
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/my_app
```

Keep the `app_main` argument, ABI, interface availability and content-area size checks. Adapt layout, callbacks and business logic to the new app; do not link LVGL or ESP-IDF directly. Replace the calculator-specific `calculator_engine.h` as needed, create your own `icon.pbm` and upload under a new app ID. Passing entry, resource and import checks does not verify device interaction; follow section 9 before publishing.

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
- On unload, page deletion or entry failure, the host cancels timers, releases the keep-awake hold and deletes the keyboard, calls the exit callback once, then releases widgets and the ELF. Standby is an overlay rather than exit and does not guarantee an exit callback.

Check only capabilities the app needs. This example requires foreground timing; a label/button app need not demand every new capability:

```c
if (!XZ_APP_HAS_FIELD(api, set_keep_awake)) return -1;
const uint32_t required = XZ_APP_CAP_TIME | XZ_APP_CAP_TIMER |
                          XZ_APP_CAP_EXIT | XZ_APP_CAP_KEEP_AWAKE;
if ((api->capabilities & required) != required || !api->host_context ||
    !api->monotonic_ms || !api->timer_start || !api->timer_cancel ||
    !api->set_on_exit || !api->set_keep_awake) return -1;
```

## 5. Capability interfaces

`capabilities` describe implemented services, not current SD, clock or resource readiness. Check each operation result. Use the descriptions below with the header's full signatures.

### Basic UI

- `label`, `button` create widgets under a parent and return opaque handles. Button `context` identifies app state. Do not cast handles to LVGL objects.
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

Writes and removes return result codes; a zero-length write may use a null data pointer. Data lives at `SD_APP_ROOT/settings/apps/<app-name>--<appId>/<key>.bin` in the [SD layout](../../main/boards/common/storage/sd_paths.h), separate from installation. Updates retain state; uninstall deletes state, recovery backups and staging files. FAT case rules apply; lowercase keys are recommended. SD export or standby may report not ready. Do not bypass the API. Writes stage data at `SD_APP_ROOT/temp/app_store/<app-name>--<appId>/data-<key>.bin.part` and clean it after success or failure. Recovery backups, `<key>.bin.cloud-backup`, remain in the state directory. Reads may fall back to an interrupted-write backup; arbitrary power loss is not guaranteed safe. Staging files left by power loss are never used for recovery; writing the same key replaces them, or they can be removed while the app is inactive. Names follow the manifest's fixed `installedName`, and IDs isolate apps sharing a name. `storage_remove` deletes the key's committed file, backup and staging file.

Version and validate stored formats, including length and checksums when appropriate, before restoring state. Save meaningful interactions and rate-limit periodic writes. Do not write every frame/second or rely solely on exit saving.

### Additional widgets: `XZ_APP_CAP_WIDGETS`

- `container`: zero `scroll_y` creates a fixed region; nonzero allows vertical scrolling. Compose lists from containers, labels and buttons. There is no separate list data model or arbitrary drawing API.
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

### Return values

`XZ_APP_OK` is zero. Errors are `XZ_APP_ERR_INVALID` for arguments, `XZ_APP_ERR_NOT_READY` for unavailable resources, `XZ_APP_ERR_NOT_FOUND` for missing objects, `XZ_APP_ERR_LIMIT` for capacity, `XZ_APP_ERR_IO` for storage and the defined `XZ_APP_ERR_MEMORY` allocation code. Not all functions use these results: check null widget handles, zero timer IDs, nonnegative text/storage byte counts and 1 from `get_local_date`. Void functions have no status. Nonzero does not universally mean failure.

## 6. Icons and content assets

Icons live in `.xz.app.icon` inside the ELF; no separate image upload is needed. The script converts `icon.pbm` with [build_icon.py](build_icon.py) to XZIC and adds a non-loadable, read-only PROGBITS section. The host reads it without executing the app. Legacy packages without icons use a generic icon; the standard script requires the PBM.

XZIC v1 is little-endian: bytes 0–3 are `XZIC`; uint16 values at offsets 4/6/8/10 are version 1, width 128, height 128 and format 1. A uint32 at offset 12 is payload length 2048. Pixels start at offset 16, row-major and MSB-first, with 1 black and 0 transparent. Total length is 2064 bytes. Listing metadata is not part of this icon format.

Place large dictionaries or content in `assets.bin`; the script adds `.xz.app.assets` without SHF_ALLOC or compression. Check the `asset_context`, `asset_size`, `read_asset` boundaries and function pointer. `read_asset(asset_context, offset, buffer, length)` returns the requested length or zero on failure. Validate `offset <= asset_size` and `length <= asset_size - offset`, read small synchronous chunks and parse the app-defined format.

The loader streams the package rather than allocating package-sized PSRAM, but executable/data segments and relocation still consume memory. The 32MiB package limit is not a RAM guarantee. Do not place bulk content in ordinary loadable constants or permanent copies. See `package_check` and `english_reader` for asset builds and on-demand reads.

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
4. Connect the device and configure the cloud or independent API base as described in the [Store guide](../../docs/app-store_en.md#usage). The platform must confirm fixed-MAC binding and board, chip, firmware and ABI compatibility. Firmware flashing and ELF installation are separate. Upgrade firmware first when new interfaces are required.
5. Refresh the Store, install, and open from the Store or home. Tap an app row for details. Multiple installs queue; tap a progress button to cancel. Leaving cancels incomplete downloads.
6. Check for updates in details and confirm installation. Unpublishing keeps local installations. Uninstall retains private data. Back up important data; do not edit installation manifests as a substitute for updates.

Packages are limited to 32MiB. Online catalogs return at most 16 apps per page, with no fixed cumulative app-count limit; devices fetch pages and reuse UI resources. Names allow 32 Chinese/48 English characters. Per-language summaries allow 256 bytes, categories 96, descriptions/release notes 1024 each, and providers 128. The API supplies up to eight language codes as an array, such as `["zh-CN","en"]`; the local manifest stores them as a comma-separated string. Byte limits use UTF-8.

## 9. Validation and troubleshooting

Validate entry results, old-host rejection/fallback, both languages, target-size layout, buttons/scroll gestures, repeated entry/exit, timer cancellation, keyboard reopen/close, missing/corrupt storage, SD export/unavailability and save compatibility after updates. Timing apps also check pause/resume, phase boundaries, clock adjustments and standby semantics. Simulations do not replace hardware input, e-ink refresh, power and full-flow tests.

- `Unsupported runtime symbol`: inspect generated imports; do not only edit the allowlist.
- Installed but not runnable: check target, ABI, capabilities and entry result. Inspect operation/stage logs from `AppStore`, `AppRuntime`, `AppServices` without exposing full URLs or tokens.
- Font warnings/missing glyphs: check installed sizes and glyph coverage rather than suppressing warnings.
- No keyboard: explicitly call `keyboard`, check parent visibility, target, existing keyboard and returned handle. The host initializes keys.
- No date: handle `get_local_date == 0` or `get_local_time` returning `XZ_APP_ERR_NOT_READY` instead of assuming a valid today.
- Save failure: distinguish first-use absence, capacity, invalid data and SD readiness. Provide useful recovery feedback without overwriting data that should be retained.
- HTTP 404: check that the selected service supports the App Store. The store URL is managed by `cloudzao_app_store_url` in `main/cloudzao_endpoints.c`.

## 10. Host implementation and licensing

Host implementations are in the [runtime page](../../main/display/screen/app_store_screen/app_store_runtime.cc), [foundation services](../../main/display/screen/app_store_screen/app_runtime_services.cc) and [storage/loader](../../main/app_store/app_store.cc). These are host maintenance entry points, not internal APIs third-party ELFs may link directly.

ELF Loader in `components/elf_loader` is sourced from Espressif esp-iot-solution commit `5fbb2597b88f21ec41e952f81292ccd6ae569761`, retaining Apache-2.0 licensing; local CMake explicitly declares lwIP. Reading content provenance/licenses are documented in the [sample guide](README.md). ELF packaging does not remove required license appendices.

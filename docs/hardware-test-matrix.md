# Multi-board hardware test cases and results

[简体中文](hardware-test-matrix_zn.md)

Cases distinguish static checks, host tests, firmware builds and hardware tests. Existing results apply only to their corresponding firmware. Source changes require validation of affected areas; pending cases are not passes, and host tests or builds cannot replace device acceptance.

Scope covers board builds, startup, display, input, SD, USB, camera cancellation, memory and standby recovery. Coverage maps to changed functional areas; no line or branch coverage instrumentation is installed. Simulated input does not establish physical input correctness, and successful logs do not establish visual correctness.


## Boundary refactor checks

| ID | Area | Procedure | Expected | 470 | 397 |
|---|---|---|---|---|---|
| BND-01 | Board reuse | Aliases, directory names, Global selection, path validation and root sdkconfig checks | Shared family defaults; isolated selection; no copied implementation for a related board | Host PASS | Host PASS |
| BND-02 | Flash admission | Actual scope code with nesting, failure, exception exit and cross-thread contention | Drain once at outer scope; no release after failed admission; consistent order | Host PASS; hardware regression pending | Host PASS; hardware regression pending |
| BND-03 | Input | Actual pause-counter concurrency/nesting; inspect logical keys and ISR boundary | Balanced references; no board indices in policy; no task-state queries from ISR | Host/source PASS; keys and OTA pending | Host/source PASS; keys and OTA pending |
| BND-04 | Vision business | Actual shared function with missing config, short writes, disconnects, HTTP errors, oversized responses and fictional secret-bearing exceptions | Separate config/capture; actionable safe errors; no credentials in logs or returned errors | Host PASS; MCP end-to-end pending | Host PASS; MCP end-to-end pending |
| BND-05 | Final images | Full builds and source inspection; no flashing | No old forwarding/miscellaneous entry; only selected hardware backends compiled | Build checked; not flashed | Build checked; not flashed |
| BND-06 | Directory/link isolation | Inspect source lists, shared implementation counts, ELF symbols and 397 resource CRC | No foreign display/touch/camera backend; one shared implementation; complete assets | Host checks PASS | Host checks PASS |
| BND-07 | Vibration configuration | Exercise four drive/output combinations, polarity, retriggering, stale callbacks, PWM, resource/output failures and concurrent start/stop | Independent drive and connection settings; attempt output-off, report errors and clean up failed timer allocation | Host simulation PASS; physical feel pending | Host simulation PASS; physical feel pending |
| BND-08 | Fresh configurations | Generate Domestic/International and direct-directory configurations from empty sdkconfig | Correct target, backend, variant and partition; root sdkconfig unchanged | Host checks PASS | Host checks PASS |
| BND-09 | Partition consolidation | Compare the shared CSV, three previous CSVs and existing board partition images using the ESP-IDF partition generator | Identical binaries; no firmware rebuild or flash | Partition validation PASS; firmware not rebuilt | Partition validation PASS; firmware not rebuilt |
| BND-10 | Domain layout and dependencies | Interpret source lists for four profiles; inspect owned headers, embedded files, backend isolation and system callback bindings | Only entry/build files at root; complete source selection; no reverse Application dependency in OTA/MQTT/MCP core | Static checks PASS; not compiled | Static checks PASS; not compiled |
| BND-11 | Audio-option removal | Inspect PCM conversion, frame declarations, Kconfig/dependencies and callers of deleted APIs | PCM/Opus capture/playback retained; no AFE/AEC/wake/model stubs; frame layout unchanged | Source checks PASS; runtime regression pending | Source checks PASS; runtime regression pending |
| BND-12 | MCP tool ownership | Inspect public registration, seven tool IDs, optional hardware/actions and initialization order | No Application dependency in tools; no product tool lists in the protocol class; disabled theme/snapshot code removed | Static checks; not compiled | Static checks; not compiled |
| BND-13 | Configuration ownership | Interpret domestic source manifests and recursively resolve owned headers, config headers and backend selection | Each translation unit uses the active board config; no duplicate sources or foreign display/camera backend | Static checks passed; not compiled | Static checks passed; not compiled |
| BND-14 | Shared input | Click/double/long press, repeat release, OTA/standby/log-upload restrictions, queue overflow, I²C sampling failure and held keys after resume | Same business rules; release/overflow stops repeat; standby power events consumed once | Source checked; runtime pending | Source checked; runtime pending |
| BND-15 | Protocol boundaries and logs | Inject short headers, invalid types/lengths, oversized frames, non-terminated JSON and malformed MCP with fictitious credentials | No out-of-bounds reads/copies or input mutation; content-free rate-limited logs | Source checked; injection pending | Source checked; injection pending |
| BND-16 | Power rollback | Simulate outstanding SD/startup/network work, failed parking/restoration, task allocation and shutdown painting | Admission timeout retains power; restoration retries remain; failed shutdown restores policy/input/refresh | Source checked; simulation/hardware pending | Source checked; simulation/hardware pending |
| BND-17 | Images and medium lifecycle | Copy/move images and release originals; revisit library/thumbnails after USB host changes; invalid expander maps and partial init | Descriptors reference owned pixels; mount generations invalidate caches; no duplicate pins or partial-resource leak | Source checked; runtime pending | Source checked; runtime pending |
| BND-18 | Application compatibility | Compare the app ABI header and load old apps on matching chips | Unchanged ABI; chip match and real loading still required | ABI header unchanged; loading pending | Not applicable: existing old apps target S31 |

Prioritize BOOT-01, FLASH-02, SD-01–02, INPUT-01–03, POWER-01–03/07, OTA-01–03, CAMERA-01–03 and NETWORK-01 for physical regression. Flash/touch concurrency, standby keys, repeated captures and leaving during upload require device checks; host simulations do not establish hardware timing correctness.

## Historical hardware baseline

| ID | Area | Procedure | Expected | 470 | 397 |
|---|---|---|---|---|---|
| BUILD-01 | Board identity | Identify the chip on each assigned port | Correct chip-to-board mapping | PASS | PASS |
| BUILD-02 | Full build | Build standard profiles and inspect source selection | Only selected hardware backend | PASS | PASS |
| BUILD-03 | Global profiles | Configure Global variants | Correct product flag and backend | PASS | PASS |
| FLASH-01 | Flash | Flash matching images with verification, preserve NVS | Verified flash and boot | PASS | PASS |
| BOOT-01 | Startup | Wait for board readiness before UI | No recursion crash, watchdog or reset loop | PASS | PASS |
| DISPLAY-01 | Geometry | Round-trip corners in four orientations; check stride and format | Reversible, in-bounds coordinates | PASS | PASS |
| NAV-01 | Navigation | Cycle home, reading, settings and network 32 times | No stale callback, hang or reset | PASS | PASS |
| DISPLAY-02 | Coalescing | Submit 64 rapid updates and wait for physical completion | BUSY clears with no new refresh errors | PASS: both boards after fix | PASS: both boards after fix |
| DISPLAY-03 | Visual quality | Inspect physical glyphs, gray levels and ghosting | Complete and appropriate display output | Not measured: no visual capture | Not measured: no visual capture |
| INPUT-01 | Quick controls | Open and close global controls | Consistent state; no frontlight on 397 | PASS: both boards after fix | PASS: both boards after fix |
| INPUT-02 | Input queue | Inject 16 pager previous/next events | Shared input dispatch remains responsive | Device injection responsive; host dispatch rules PASS | Device injection responsive; host dispatch rules PASS |
| INPUT-03 | Physical input | Physical touch, short-list pull-down and key levels | Correct recognition and gesture ownership | Not measured: no input actuator | Not measured: no input actuator |
| SD-01 | SD transactions | Write/read/verify/delete a temporary file 24 times | Matching data and cleaned test file | PASS | PASS |
| SD-02 | Media recovery | Repeat SD I/O after USB test | Mounted and usable | PASS | PASS |
| USB-01 | Virtual disk | Enable for 8 seconds and stop automatically | Controller and SD ownership restored | PASS | PASS |
| USB-02 | Role exclusion | Concurrent admission and wrong-owner release on host | Single owner; wrong release rejected | PASS: shared-header host test | PASS: shared-header host test |
| CAMERA-01 | Cancellation | Cancel before capture | No capture, output or retained resources | PASS | Host simulation PASS; fixes flashed, targeted device regression incomplete |
| CAMERA-02 | Physical camera | Preview, capture and disconnect an attached camera | Correct capture and recovery | 640×480 capture passed; physical hot-plug untested | Unconfirmed: test results after camera routing were not retrieved |
| MEMORY-01 | Memory | Record memory around loops and check integrity | No corruption; distinguish caches from leaks | PASS | Unconfirmed: result logs lost around USB switching |
| POWER-01 | Automatic standby | Release inhibition after functional tests; wait original 5-minute threshold | Automatic standby and cleanup | PASS | PASS |
| POWER-02 | Timed recovery | Inject software wake after actual light sleep | Restore CPU, peripherals and serial without manual action | PASS | Unconfirmed: test results after camera routing were not retrieved |
| POWER-03 | Post-wake regression | Check SD, quick controls and refresh after wake | Operations resume | PASS | Unconfirmed: test results after camera routing were not retrieved |
| POWER-04 | Consumption and physical wake | Current measurement and physical POWER input | Meets hardware consumption and wake requirements | Not measured: no instrument or actuator | Not measured: no instrument or actuator |
| POWER-05 | Physical power-off | Validate low-battery notice and cut-off sequence | Safe cut-off after physical completion | Not run: no automatic power-on path | Not run: no automatic power-on path |
| OTA-01 | Resource image | Check actual trailer length, CRC and partition capacity | Valid checksums within capacity | N/A: existing resource flow | PASS |
| OTA-02 | Invalid package | Simulate missing assets and repeated packaging | Reject invalid resources; idempotent output | N/A | PASS: host simulation |
| FAIL-01 | Failure injection | Bus failure, BUSY timeout and missing peripherals | Actionable errors; no false physical completion | Host simulation PASS; physical faults not measured | Host simulation PASS; physical faults not measured |
| FINAL-01 | Final release regression | Remove test hooks; rebuild, flash and boot | No test hooks; original 5-minute standby | Latest release flash verified and booted; default automatic standby verified | PASS: latest release flash verified and reset boot completed |
| FLASH-02 | Flash and bus | Commit/read/clean a temporary NVS key 32 times | Matching data; key removed; no bus deadlock | PASS | PASS |
| AUDIO-01 | Audio lifecycle | Acquire through audio task, capture Opus and release, three cycles | Valid packets and returned DMA resources; no claim of acoustic quality | PASS | PASS |
| SENSOR-01 | Gauge and RTC | Read actual battery voltage and RTC | Successful I2C reads; plausible voltage | PASS | PASS |
| SENSOR-02 | Peripheral regression | Repeat gauge and RTC reads after audio and capture | Shared bus remains usable | PASS | Unconfirmed: test results after camera routing were not retrieved |
| POWER-06 | CPU capabilities | Apply each board frequency and measure actual clock | Matching frequencies; original policy value restored | PASS | 80/160 MHz confirmed; 240 MHz result unavailable |
| POWER-07 | Repeated standby | Three standby/recovery cycles after peripheral tests | SD, I2C and UI recover; verify actual sleep in logs | PASS | Unconfirmed: test results after camera routing were not retrieved |
| LIGHT-01 | Frontlight capability | Cycle and restore 470 brightness; check 397 remains uninitialized | State restored; no absent hardware driven; no photometric measurement | PASS | Unconfirmed: test results after camera routing were not retrieved |
| USB-03 | Retained Host boundary | Request virtual disk after 397 camera Host remains installed | Busy result; no PHY switch over live Host | N/A | Unconfirmed: test results after camera routing were not retrieved |
| INPUT-04 | Gesture arbitration simulation | Empty/short/long lists, boundaries, horizontal scroll and paged drag | Actual arbitration functions meet both backend contracts | Host simulation PASS | Host simulation PASS |
| STATUS-01 | Status binding simulation | Delete an old/current status bar and repeat revocation | Revoke only the matching binding; preserve newer screen | Host simulation PASS | Host simulation PASS |
| NETWORK-01 | Network and protocols | Startup and network-screen cycles; distinguish actual service traffic | Stable lifecycle; cloud traffic needs separate end-to-end validation | Basic lifecycle PASS; end-to-end untested | Basic lifecycle PASS; end-to-end untested |
| AUDIO-02 | Speaker and Bluetooth calls | Acoustic capture, quality and actual calls | Correct sound and communication | Not measured | Not measured |
| OTA-03 | Actual OTA failures | Upgrade, network interruption and interrupted writes | Correct recovery and data integrity | No actual OTA/power-cut injection | No actual OTA/power-cut injection |
| POWER-08 | Power-off gate simulation | Preparation, absent display, failed refresh and successful pulse order | Failed preparation must not cut power | Existing render failure simulation; actual cut-off untested | Actual shutdown task host simulation PASS; actual cut-off untested |
| CAMERA-03 | Capture failure cleanup simulation | Duplicate admission, incomplete-session cleanup and standby-lease balance | Release only owned resources; balance standby inhibition | N/A: existing DVP path | Host simulation PASS; fixes flashed, targeted device regression incomplete |

## UI layout checks

| ID | Area | Procedure | Expected | 470 | 397 |
|---|---|---|---|---|---|
| UI-01 | Option layout | Actual LVGL at widths 180/288/460/680 with two test-font line heights, repeated layouts and changed long English text | No horizontal overflow; text fits button content height; stable layout | Host PASS; physical display pending | Host PASS; physical display pending |
| UI-02 | QR raster | Actual LVGL at logical heights 480/800/1216 and hidden-panel first paint; compare every pixel with encoded modules | Equal integer modules, four-module quiet zone, no overflow | Host PASS; scanning pending | Host PASS; scanning pending |
| UI-03 | Font resources | Check product resource specifications and build both profiles | Default font independent of display controller; retain saved selection | Build checked; reading display pending | Build checked; reading display pending |
| UI-04 | Standby artwork | Verify aspect calculation; inspect portrait and landscape composition | No sizing from physical panel width; aspect retained | Calculation PASS; display pending | Calculation PASS; display pending |
| UI-05 | Common control regression | Visit language, charging, frontlight, power, Bluetooth and pager options in both languages | Wrapping and fixed-height callers work; clicks and persistence unchanged | User display verification pending | User display verification pending |
| UI-06 | Home themes | Visit white, gray, border, slash and pixel themes with installed apps, long names and multi-digit values | No Hero/app overlap; complete icons; bounded labels and shadows | Digit and grid paging host PASS; complete physical themes pending | Digit and grid paging host PASS; complete physical themes pending |
| UI-07 | Reader and TOC | Option columns and portrait/landscape TOC capacity; chapter jumps and persistence | Bounded options; capacity changes preserve chapter identity | Option layout host PASS; TOC and display pending | Option layout host PASS; TOC and display pending |
| UI-08 | Pager cards | Long names, addresses, status; empty/short/long lists and modals | Content-driven height; no intrusion into actions | Build checked; physical layout and pairing pending | Build checked; physical layout and pairing pending |
| UI-09 | Welcome and photos | Actual welcome and camera layout functions with two test fonts, both languages, board sizes and orientations | No text overlap; images avoid titles and bottom actions | Host PASS; actual photos and display pending | Host PASS; actual photos and display pending |
| UI-10 | Monochrome and gestures | Actual shelf texture at 2/16 levels; shared gesture policy for empty/short/long and paged lists | Binary shelf pixels on monochrome; one policy for both backends | Host PASS; touch and display pending | Host PASS; touch and display pending |
| UI-11 | Font failure and reflow | Default font success/failure and repeated access; enlarge an initially insufficient text area | Non-null fallback and cached initialization; original text restored | Host PASS | Host PASS |

The documented software issues are assigned to product resource specifications, shared layout/text rules, a single gesture policy, theme layout or display capabilities. Existing unsupported page-wipe fallback, absent-frontlight entry handling, orientation mapping and asynchronous cancellation remain in place; there are no copied 397 business controllers. Source convergence and host checks do not establish physical acceptance. Touch, dithering contrast, ghosting, pairing and scanning remain pending as listed above.

## Issues and regression mapping

| Issue | Change scope | Required regression | Status |
|---|---|---|---|
| HW-001: clock update accesses a deleted status label after a screen without a status bar takes over; both boards crash | Revoke the matching binding when its status label is deleted | DISPLAY-02, NAV-01, INPUT-01, POWER-03 | Display/navigation regression PASS on both; 470 post-wake PASS, 397 post-wake regression incomplete |
| HW-002: absent frontlight blocks all quick controls on 397 | Require frontlight readiness only on boards with frontlight | INPUT-01, INPUT-03, POWER-03 | Display/navigation regression PASS on both; 470 post-wake PASS, 397 post-wake regression incomplete |
| HW-003: 397 camera routing removes serial and JTAG, blocking recovery | Existing retained-Host limitation; do not force PHY switching over a live Host | CAMERA-02, USB-03, POWER-02, POWER-03, POWER-07, FINAL-01 | Release reflashed after control access returned; retained-Host risk remains and related stress results are unconfirmed |
| HW-004: camera ownership reused the PC-eject-disk hint | Separate CameraHostBusy with a bilingual restart hint | USB-01, USB-03, NAV-01 | Source/build verified; release flashed, 397 ownership-scenario regression incomplete |

## Regression triggers

| Change scope | Required cases |
|---|---|
| Product resources, shared layout, text or gesture policy | UI-01–11, NAV-01, INPUT-01–04, POWER-03; physical checks on both boards, languages and orientations |
| Board manifests, defaults, partitions and assets | BUILD-01–03, FLASH-01, OTA-01–03, FINAL-01 |
| Display backends, frame formats and page lifecycle | DISPLAY-01–03, NAV-01, STATUS-01, POWER-03, POWER-07 |
| Input and quick controls | INPUT-01–04, NAV-01, POWER-03 |
| Power policy and board hardware operations | POWER-01–08, SENSOR-02, SD-02, AUDIO-01 |
| USB and capture | USB-01–03, CAMERA-01–03, SD-02, MEMORY-01, POWER-07 |
| Audio, shared buses, Flash and storage | AUDIO-01–02, FLASH-02, SENSOR-01–02, SD-01–02, POWER-07 |

Automation requires a device-side reset fallback and persisted results, or independent controllable power/UART, before camera, USB-role and standby tests. Serial and USB JTAG share a failure domain when they use the same PHY; they are not independent recovery paths. Skip tests that disconnect the only control channel when recovery is unavailable. Complete all image updates before observing automatic standby on release firmware, since USB disconnection can prevent another flash.

Keep PASS, host simulation PASS, not measured and BLOCKED distinct. Functional-area coverage is not statement, branch or physical-hardware coverage. Blocked and unmeasured cases must not count as passes.

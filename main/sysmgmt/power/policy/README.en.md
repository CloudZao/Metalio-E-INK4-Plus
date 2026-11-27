# Low-power policy (S31)

[中文](README.md) | **English**

The hardware sequences below apply to the 470. Shared policy also serves the 397; see [multi-board integration](../../../../docs/hardware-adaptation.md) for board boundaries.

This document describes software cleanup, hardware operations, and restoration when `metalio-e-ink4-plus` or `metalio-e-ink4-plus-global` enters standby, wakes from light sleep, and leaves standby. The global variant uses Wi-Fi only; both variants share the remaining standby hardware sequence. Showing the standby page starts the transition: light sleep requires completed cleanup and physical display refresh. A timer wake does not leave standby.

Standby is a software state. The MCU enters light sleep through `esp_light_sleep_start()` and continues after that call returns on wake. The touch controller's own deep-sleep mode is a separate peripheral feature; its support is described under Current limits and failure protection.

## Entry points and source ownership

- Automatic standby: `PowerPolicy` evaluates power needs, device state, and inactivity, then requests the standby page asynchronously. The UI callback checks needs again so an obsolete request cannot cancel a transfer that has just started.
- Short POWER press: the [key handler](../../../ui/common/runtime/input/boot_key_handler.cc) requests the standby page, stops transfers, and requests transfer-hotspot suspension. OTA screens block this action. USB virtual-disk ownership or an SD transition defers entry.
- Applications declare needs through `PowerPolicy::GetInstance()`, `Acquire/Release`, or `PowerNeedHold` in [power_policy.h](power_policy.h), without adding Board forwarding methods.

| Source | Responsibility |
|---|---|
| [power_policy.cc](power_policy.cc) | Mode evaluation, need references, timing, NVS, and scheduling of network, CPU, and shared-power requests |
| [power_policy_standby_lp.cc](power_policy_standby_lp.cc) | Cleanup waits, display parking, sleep slices, key interpretation, and standby-exit scheduling |
| [power_hw.cc](../../../boards/common/power/power_hw.cc) | Shared frequency holds and system callbacks |
| [eink4_plus_power_hw.cc](../../../boards/common/power/topology/eink4_plus_power_hw.cc) | SD, rails, pin holds, wake and shutdown pulses |
| [power_lifecycle.cc](../power_lifecycle.cc) | Network pause/recovery, feature suspension and restoration, low-battery shutdown and failure rollback |
| [standby_screen.cc](../../../ui/apps/standby/standby_screen.cc) | Standby overlay, variant presentation and painting lifecycle |
| [lv_adapter_epdiy.cc](../../../ui/common/drawing/adapter/epdiy/lv_adapter_epdiy.cc) | Standby-frame submission, physical-refresh completion, submission freezing, touch-sampling pause, and frontlight restoration |
| [s31_sleep/](../../../boards/common/power/s31_sleep) | SDK sleep compatibility, MPLL/PSRAM clock handoff, and retained RTC diagnostics |

The board [config.h](../../../boards/metalio-e-ink4-plus/config.h) owns pin, bus, and power-control definitions. The [board display/power implementation](../../../boards/common/display/epdiy/panel/epd_board_adapter.c) owns rail and pin-hold operations. The standby pin-configuration section below records directions, levels, and holds; board sources remain authoritative for pin definitions.

Wi-Fi pause/resume and transfer AP ownership share `infra/network/wifi_radio.*`. Idle suspension and actual standby are distinct: only standby blocks new AP requests. Restoration respects the user switch and retains failed cleanup state for retry.

A Wi-Fi setup session is registered under the registry lock before acquiring its network hold outside that lock. Power restoration queries session state, so power callbacks must never run while the registry lock is held. If task creation fails, the session is marked closed and its network hold is released, allowing a later attempt. Settings waits for the next automatic scan interval after a failed request instead of recreating the task on each UI poll.

## Bluetooth shared-power reset

The 4.7-inch board preserves shared power at startup without resetting P14. It initializes UART before configuring the audio mode in the background. The automatic test page queries the Bluetooth version once UART is available, independently of mode 1 confirmation. An unavailable UART or response timeout ends the query and displays an unknown result.

Settings → Reset BT uses `power_hw_bt_power_reset()`. It stops the cellular driver and ordinary vibration, disables the PA, notifies the audio driver of power removal, then drives P14 low. While power is off, the motor stays high for discharge and is then driven low. Power is restored after any remaining minimum off time. The board's `config.h` maintains these intervals through `POWER_DISCHARGE_MOTOR_PULSE_MS` and `POWER_BT_RESET_LOW_MS`. The discharge action is independent of the UI haptic switch and intensity; ordinary vibration PWM cannot change the discharge level during reset. The motor and PA remain off afterward. Drivers are notified after successful power restoration; IO write failures return an error.

## Entering standby: software cleanup

Main sequence:

```text
Show standby overlay, pause applications, block ordinary new SD access
→ Wait for standby content
→ Drain SD operations and handles
→ Wait for boot work, network/audio work, and hardware requests
→ Unmount and park SD
→ Park the standby frame, wait for physical refresh, turn off frontlight
→ Sleep the panel and bus, remove P5 power, preserve P14
→ Run the light-sleep hardware sequence
```

### 1. Show standby and request pauses

`StandbyPresentation::Show()` keeps the underlying page, shows the classic page or selected wallpaper, closes quick controls, and uses `BeginStandbyEnterPaint()` to defer physical submission until the standby frame can be parked. It does not turn off the frontlight early.

| Software owner | Action on standby entry |
|---|---|
| Reader | Stops orientation tracking, reading-time accounting, and font scanning; closes font-operation panels; cancels page rendering, image loading, layout, and pagination. After workers and font conversion finish, detaches book SD handles and releases the reader font. A held SD transaction prevents premature unmounting. |
| Wallpaper browser | Pauses decoding and invalidates older preview/thumbnail results. The standby wallpaper itself may finish decoding through explicitly permitted SD access. |
| Baiwen AI | Clears push-to-talk state and the listening animation, releases the foreground network hold, and stops the voice session. Worker tasks finish asynchronous capture, codec, and connection cleanup. |
| WorkBuddy | Cancels voice, HTTP, authorization checks, and reply polling; stops recording and repeated page turns; releases the foreground network hold. Cancellation does not resend accepted commands. The global variant has no application entry. |
| Cloud transfer and log upload | Entry requests transfer/sync cancellation and aborts log upload; the settings hook tears down upload state. Connection closure and worker reclamation do not wait inside UI callbacks. The POWER entry also requests AP shutdown, completed by the radio worker. |
| SD runtime logger | `SdLogWriter_OnSdUnmounting()` stops production before `sd_access::Quiesce()` blocks ordinary new access. Admitted transactions and permitted standby decoding may finish. |
| Battery test and touch | Pauses the battery-test page's polling timer. `PauseStandbyBackgroundWork()` pauses touch sampling to reduce standby I²C traffic. |
| Network, PA, and shared-power policy | Reevaluates standby policy, pauses the STA radio, and turns off PA subject to the need rules below. Network and shared-power operations run serially outside the policy lock; the standby worker waits for completion. |

The audio service is shared by multiple consumers. Audio workers stop and I²S channels/DMA resources are released only after the last consumer returns its hold. Network-application ownership remains visible until the workers exit, so a stop request is not mistaken for completed driver release. See [audio_runtime.cc](../../audio/audio_runtime.cc) and [audio_service.cc](../../../infra/audio/audio_service.cc).

`UiPageHooks::power` only issues nonblocking requests; returning does not acknowledge worker exit. Before storage closes admission, owners stop producing work and retain SD reservations for accepted saves. After admission closes, UI updates pause while owners finish cleanup. `PowerLifecyclePendingStandbyWork()` reports storage, startup or activity work that still prevents standby. Owners retain SD transactions or audio/network/hardware holds until release actually completes; failures retain cleanup responsibility. Timeout or user cancellation aborts sleep and restores the overlay flow while power remains available. Storage resumes before application and view hooks; the physical wake-frame window remains a separate condition.

### 2. Wait until resources are idle

The `standby_lp` worker first attempts the board's `TouchEnterSleep()` hook, then performs these checks:

1. `WaitStandbyPaintReady()` waits for at least one classic-page LVGL flush or completed standby-wallpaper decoding. Content readiness is not physical panel-refresh completion.
2. `DrainSdAndBootWork()` waits for `sd_access::IsIdle()`: operations, transactions, and file/directory handles must be released. SD is parked only after the startup and business checks below also complete.
3. Waits for `SystemState::IsStartupBusy()` to clear so power removal cannot race startup hardware initialization. Startup yields hardware ownership at standby checkpoints.
4. Waits for `AudioRuntime::HasWork()`, `HasForegroundNetworkWork()`, and `HasPendingHardwareWork()` to clear so capture, factory tests, pending audio releases, connection recovery, and shared-power operations cannot overlap rail removal.

POWER and wake requests remain polled during these waits. SD cleanup and standby-content preparation have no timeout that forces power removal. An admission deadline cancels sleep and requests UI recovery; live file writes and shared-rail users are never bypassed. Failed hardware restoration retains recovery responsibility and retries with limited logs.

### 3. Park the display and turn off the frontlight

`StandbyPresentation::EnterEpdSleep()` requests `ParkEpdForStandby()` through the LVGL queue. It freezes later UI submissions, submits the complete target frame, and waits for physical completion with `EpdWait()`. Only a successful refresh is followed by `Frontlight::ForceAllLow()`.

A failed refresh keeps the device awake and is retried before light sleep. See [display documentation](../../../ui/README.md) for cleaning and waveform details. Physical refresh and Flash log writes are mutually excluded. The frame must be parked before the display supply and bus are parked.

With light sleep enabled, initial parking sleeps the panel and bus, stops the cellular driver, and removes P5 power after the standby frame completes. Timer wakes retain that state. Standby entry and exit preserve P14 shared power without motor discharge or audio power-off/restoration notifications. Each slice confirms P5 shutdown, retrying failures while checking exit requests. Standby exit restores P5 and the display bus.

## Entering light sleep: hardware order

Before every attempt, `power_hw_light_sleep_once()` checks that SD admission is closed and drained, boot work is finished, and no standby-exit request exists, then holds the SD transition guard. The current `kLightSleepHw` enables all four groups: SD, TPS, display bus, and peripheral rails. TPS sleep, bus parking, and rail removal complete before each slice. The hardware sleep function reuses that state; when no earlier parking occurred, it follows the order below.

1. **Confirm SD parking.** `power_hw_sd_park()` is idempotent and does not unmount an already parked card again.
2. **Sleep the panel PMIC.** The board sequence puts TPS65185 into SLEEP, then holds the display bus low.
3. **Release shared interrupts.** `Accelerometer_ReleaseIntLine()` stops reading-orientation tracking and deinitializes or silences accelerometer interrupts so shared INT is not held low.
4. **Stop key sampling and set safe levels.** `ExpanderButtonInput_MaskForSleep()` stops side-key scanning before changing IO-expander ports. Pending POWER clicks and standby-exit requests are checked again.
5. **Arm wake sources.** Restores the POWER channel as an input, lets it settle, and reads the port to acknowledge interrupts left by the direction change. It confirms that POWER is released and shared INT is idle-high before arming MCU EXT1 low-level wake and the slice timer. A stuck INT or failed configuration aborts the attempt.
6. **Remove display peripheral power.** Forces frontlight outputs low again. In cellular mode, stops the modem and its UART/UHCI/network tasks. `sleep_cut_rails()` removes P5 camera/display power and preserves P14 shared power.
7. **Latch the bus and sleep.** Holds display-bus sleep levels, records the RTC phase, and calls `esp_light_sleep_start()` through `s31_sleep::LightSleep()`.

Each slice is at most 60 seconds and is shortened near the automatic power-off deadline. Automatic power-off occurs after the selected accumulated standby interval.

## Standby pin and level configuration

The following maps the active definitions in `config.h` to operations in `power_hw.cc` and the board drivers. Domestic and global variants share the mapping; the cellular row executes only when the active network board is cellular. Recheck this section when board definitions or hardware revisions change. These tables describe behavior; source code remains authoritative for configuration.

`GPIO n` means an ESP32-S31 pin. `Pxy` means TCA9555 Port x, bit y: **P12 = P1.2 = expander linear index 10**, not MCU GPIO12, which is EPD D4. Output 0/1 means low/high; input rows do not imply a fixed level. TCA configuration bits use 1 for input and 0 for output. Expander latches and MCU `gpio_hold_en()` are different mechanisms.

### MCU GPIO: sleep configuration and restoration

| Signal | Current GPIO mapping | Before / during light sleep | Wake and standby exit |
|---|---|---|---|
| Shared I²C | SDA = GPIO0; SCL = GPIO1 | Retains the shared I²C bus and pull-up configuration. This path does not force these pins low or add GPIO hold; TCA and other devices remain accessible. | Uses the existing bus to restore the expander and power controls; does not recreate I²C on each wake. |
| Shared INT / EXT1 | GPIO2 | MCU input, pull-up enabled and pull-down disabled. Checks idle-high before arming EXT1 ANY_LOW. TCA POWER and the accelerometer share this INT line; accelerometer interrupts are released first. | Latches POWER before disabling this EXT1 configuration, then restores normal key ports. Low-level wake does not mean touch wake. |
| SDMMC data | D0/D1/D2/D3 = GPIO20/21/22/23 | Unmounts SD and releases CNNT routing, selects GPIO input/output mode, disables GPIO pulls, drives 0, and calls `gpio_hold_en()`. | User exit releases holds and remounts/recreates SDMMC. Timer wakes leave SD parked. |
| SDMMC clock/command | CLK = GPIO24; CMD = GPIO25 | Same treatment as data pins. The CNNT SDIO path separately disables pull-ups, enables pull-downs, and clears dedicated routing. CNNT settings and GPIO pulls are distinct. | Restored with the data pins. USB Serial/JTAG console restoration follows the SD initialization call. |
| EPD data | D0–D7 = GPIO8–15 | Deinitializes the LCD bus, selects GPIO output with no pulls, and drives every pin 0. Enables GPIO hold after shared-rail removal and before the CPU enters light sleep. | Releases holds and reinitializes the LCD bus and routing. |
| EPD control | XCL = GPIO16; XLE = GPIO17; XOE = GPIO18; XSTL = GPIO4; SPV = GPIO35; CKV = GPIO40; MODE = GPIO19; BORDER = GPIO37 | Driven low and held with the EPD data group. Uses the active XSTL/MODE definitions, not the commented-out older mapping in `config.h`. | Releases holds, first sets XOE/MODE/BORDER to output 0, then recreates the LCD bus. Pins are not all restored high directly. |
| Frontlight | Cool = GPIO6; warm = GPIO7 | After the standby frame completes, sets duty/output to 0 and stops the brightness-transition timer. Keeps LEDC routing and adds no separate GPIO hold. Forces low again before rail removal. | Remains 0 after power restoration; saved brightness returns only after the underlying frame physically completes. |
| Touch INT | GPIO5 | Retains its input/pull-up and interrupt configuration while sampling is paused. It is not armed as a light-sleep wake source. | Sampling resumes on standby exit; no controller deep-sleep command or reset pulse is implied. |
| BOOT | Board configuration | Arm ordinary GPIO low-level wake when standby exit or shutdown is assigned. | Evaluate the configured short or long press after wake. |
| Audio I²S | BCLK = GPIO42; WS = GPIO43; DOUT = GPIO44; DIN = GPIO45 | The last audio consumer releases I²S channels and DMA. There is no separate all-low or GPIO-hold configuration for this group; the shared rail controls chip power. | The I²S driver recreates channels after audio-mode readiness and a service request. Rail restoration alone does not restore channels. |
| Bluetooth audio UART | TX = GPIO39; RX = GPIO38 | Preserves pin configuration and P14 power without a power-off notification. | Controlled by subsequent audio demand. |
| Cellular UART / handshake | TX = GPIO58; RX = GPIO59; MRDY = GPIO60; SRDY = GPIO3 | During standby parking, retains P14 power and stops modem/UHCI, disables SRDY interrupt/GPIO wake, restores MRDY sleep-select, and calls `gpio_reset_pin()` on all four pins. It does not drive the group low or add holds. GPIO reset defaults depend on the SDK. | The cellular recovery worker recreates UART/handshake/networking on user exit, not on timer wakes. The global variant has no active cellular board and skips this handling. |
| Analog switch | ANA_SW = GPIO36 | The current sleep sequence makes no additional change; previous routing remains. It is not an implemented standby power-removal action. | Retains routing until the relevant feature changes it. |

Implementation: [SD/CNNT](../../../boards/common/storage/sdmmc/sdmmc_storage.cc), [shared I²C](../../../boards/common/bus/i2c/shared_i2c_bus.c), [frontlight](../../../boards/common/display/frontlight.cc), [I²S release](../../../boards/common/audio/bluetooth/bt_audio_codec.cc), and [cellular pin cleanup](../../../boards/common/board/uart_eth_modem.cc).

### TCA9555: directions, latches, and power controls

Direction changes have three stages; keys do not simply remain inputs throughout:

- Normal: Port0 input mask is `0xC0`; Port1 input mask is `0xE3`.
- `ioexp_mask_for_sleep()`: writes safe output latches first, then writes both configuration registers to `0x00` (all outputs). Key sampling stops before this step.
- `ioexp_enable_key_wake_pins()`: preserves physical inputs assigned to standby exit or shutdown, settles and acknowledges direction-change interrupts; normal input directions are restored after wake.

| Expander pin | Signal / normal state | Before and during sleep | Wake restoration |
|---|---|---|---|
| P00 / P01 / P02 | TPS VCOM_CTRL / PWRUP / WAKEUP, outputs | VCOM_CTRL/PWRUP go to 0 first, followed by WAKEUP; all remain 0 during sleep. | Output directions return without driving all high. The display driver powers TPS as required for refresh. |
| P03 | Motor enable, output | No standby discharge pulse; masking preserves the software latch. | Controlled by ordinary haptics. |
| P04 | PA_EN, output | Power/audio policy requests PA-off. Masking preserves that policy latch; P14 retains shared power. | Follows audio needs and state; PA is not unconditionally enabled with P14. |
| P05 | Camera/display power, output | `sleep_cut_rails()` sets it to 0 before each slice. | `sleep_restore_rails()` sets it to 1 and waits for settling before restoring the bus. |
| P06 | Unassigned, normally input | Output 0 after masking. | Restored to input. |
| P07 | TPS PWR_GOOD, normally input | Changed to output 0 after TPS enters SLEEP. It is not read as an operating-voltage indicator while asleep. | Restored to input for subsequent display-power handling. |
| P10 / P11 | Volume key inputs | Preserve wake inputs according to bindings; keep other pins idle. | Restore inputs and the sampling baseline. |
| P12 | Shared touch/cellular/camera RST, output | Preserves its latch, normally 1 after power-up. Standby does not actively pull it low or pulse reset. | Preserves its latch. Paused touch sampling does not mean this pin removed power. |
| P13 | Power-off pulse, output | Forced 0; no power-off pulses are sent. | Remains 0. Only hardware power-off generates configured pulses. |
| P14 | Shared cellular/Bluetooth/PA power, output | Preserves the supply latch without removing power. | Preserves its state without a power cycle. |
| P15 | Unassigned, normally input | Output 0 after masking. | Restored to input. |
| P16 | RTC_INT, normally input | Output 0 after masking, not retained as an external RTC-interrupt wake input for this sleep. | Restored to input. Slice wake uses the MCU timer wake source. |
| P17 | POWER, normally input | Preserved as a wake input when its binding requires it. | Latched before input restoration. |

Current TPS timing: when the three controls are not already all low, P00/P01 are lowered, followed 50 ms later by P02, then a 120 ms wait. If already all low, only the final 120 ms wait is added. Standby switches only **P05** and preserves **P14**.

Expander physical keys wake through the shared interrupt; BOOT uses ordinary GPIO light-sleep wake according to its binding. Board configuration remains authoritative for pin assignments. Touch and RTC interrupts are not key-customization wake sources.

## Waking and leaving standby

### 1. Hardware restoration after every light-sleep return

After `esp_light_sleep_start()` returns, key sampling is restored to evaluate the wake request:

1. Latches POWER state before restoring key directions so a released short press is not lost.
2. Disables this attempt's EXT1/timer wake configuration. `ExpanderButtonInput_ResumeAfterSleep()` restores key inputs, waits for stable levels, and rebuilds sampling baselines to avoid false press/release edges.
3. Successful returns leave peripheral supplies, the display bus, and SD parked. The standby worker decides whether exit or shutdown requires restoration. Timer wakes and shared interrupts without key evidence therefore avoid repeated peripheral power cycles. Failed restoration rolls back applied stages and returns an error.

### 2. Timer wake: stay in standby

Timer wakes account for standby time and check automatic power-off. They do not remove the overlay, resume application networking, start recording, restore peripheral supplies or the display bus, or remount SD. Another sleep sequence runs with peripherals still powered down unless the power-off deadline or a key-exit request requires action.

Standby therefore consists of repeated light-sleep slices, with short awake intervals and hardware operations between them. It is neither continuous whole-device power removal nor a deep-sleep reboot.

### 3. Physical key wake: resume software

Initial bindings use a POWER short press for standby exit and a long press for shutdown. The standby task evaluates the configured action and hold duration; physical POWER events do not force fixed software actions. Each gesture is consumed once across recovery.

Key levels are latched before input directions are restored, then the configured gesture is evaluated. A shared interrupt without a valid key action leaves the device in standby. Bottom touch keys cannot exit standby. See [Keys & gestures](../../../../docs/key-bindings.md).

The standby worker finishes an exit request through the same restoration path even if it arrives before SD draining or display parking completes:

1. Restores peripheral supplies and the display bus first, retrying on failure. Board restoration enables P5 directly, waits for `POWER_RAIL_SETTLE_MS`, then restores the bus. P14 retains its state. Successful restoration updates cellular recovery state while keeping the frontlight off; it does not imply service readiness. Then calls `power_hw_sd_restore()` to attempt SD restoration, requests asynchronous cellular recovery for the active network board, clears the consumed POWER click, and posts `FinishWake()` to the priority UI queue. This caller does not gate exit on the SD restoration result: exit continues after a mount failure, while application access remains constrained by actual mount state. An application-loop network wait does not discard the exit request.
2. `StandbyPresentation::Dismiss()` unfreezes display submissions and prepares saved frontlight brightness. `sd_access::Resume()` reopens ordinary SD admission and resumes SD logging, then the standby variant, timers, and overlay are removed. The display task renders the underlying page after the exit callback finishes restoring its resources.
3. `OnStandbyOverlayDismissed()` clears requests and accumulated standby time, records user activity, and reevaluates the mode. New needs drive network restoration; returning to Home does not guarantee immediate reconnection. The saved manual Wi-Fi-off setting has priority.
4. A retained assistant or WorkBuddy page reacquires foreground networking and resumes session/authorization checks. It does not automatically record or resend accepted requests. Old worker results cannot touch invalid controls or overwrite a newer session.
5. Resets battery filtering and resamples after wake load settles. Resumes checklist/reader work, fonts/book handles, necessary library warmup, settings frontlight controls, battery-test polling, and touch sampling. Reader restoration waits for previous workers to finish.
6. Requests the underlying page's frame and sets the frontlight-restoration flag, then resumes wallpaper decoding and lets the transfer page recreate its AP when required. These software operations do not wait for physical refresh. **The refresh worker restores the frontlight after the associated frame physically completes**; `Dismiss()` does not turn it on synchronously.

## Current limits and failure protection

- **Paused sampling is not controller deep sleep.** This board's `TouchEnterSleep()` and `TouchWakeByReset()` return `ESP_ERR_NOT_SUPPORTED`. Do not describe paused touch sampling as FT6336U deep sleep or power removal.
- **AppIdle does not physically remove shared power.** `power_hw_main_rail_set()` maintains policy state; there is no independent MAIN_PWR rail to switch. Shared rails are physically switched in the light-sleep sequence.
- **A visible standby page does not prove light sleep.** Pending decoding, handles, boot work, network cleanup, or audio cleanup can delay sleep. Exit remains requestable while waiting.
- Failed wake configuration or pre-sleep checks abort the attempt. The rollback path restores hardware according to applied stages, including rail, bus, and SD operations, before key handling resumes in the standby loop.
- Failed SD restoration is not a successful mount; ordinary access still checks mount state. Leaving USB virtual-disk mode first removes the TinyUSB MSC mount, then releases raw-card/SDMMC ownership and restores the normal mount. Mount ownership must not be mixed.
- PA-off, display parking, shared-rail removal, and light sleep are distinct stages. Tasks are not all deleted, and RTC/retained memory does not imply every peripheral is off. Actual standby current requires measurement on the target hardware with its supply and wake configuration; software mode names do not establish it.

## Modes and settings

`PowerPolicy` reevaluates every second and on changes to needs, state, or configuration. `KeepNet` is reserved and is not currently selected.

| Mode | Main conditions and actions |
|---|---|
| `Full` | Phone/Bluetooth audio needs: operating CPU frequency and PA. These needs take precedence over ordinary inactivity standby. |
| `NetActive` | Hard network needs, foreground network holds, provisioning/activation, network grace, or local playback: operating CPU frequency and networking as needed. Connecting/Listening do not enable PA; Speaking/AudioTesting/LocalPlayback may do so. |
| `AppIdle` | Idle, no network need, no local playback, and expired grace: PA off, STA paused, configured idle CPU frequency. Shared-power changes here are policy state only. |
| `StandbyUi` | Inactivity deadline or standby request: overlay, cleanup, and the sleep sequence above. |

`AudioTesting` is held by factory audio tests: it keeps networking available, enables PA as needed, and blocks automatic standby without changing conversation state. Its hold and audio workers must drain before rail removal. `LocalPlayback` is held by ELF app local playback: it keeps the operating CPU frequency and PA without requiring networking. The playback session also holds `StandbyInhibit` against inactivity standby; power-key standby still interrupts playback, and the app decides whether to resume after wake. `PeripheralReset` suppresses PA. While `UiKeepNet` is held, the policy leaves PA unchanged. `ForegroundNetwork` is a soft hold: it does not refresh user inactivity, affect PA, or prevent automatic standby. Active recording, OTA/downloads, and hard states such as Connecting/Listening/Speaking/Upgrading block automatic standby. `UsbVirtualDisk` and `StandbyInhibit` only block inactivity standby; they do not independently force networking or PA. Releasing hard, USB, or inhibit needs refreshes inactivity to avoid immediate standby.

Settings → Power reads its inactivity timeout from `BOARD_IDLE_TO_STANDBY_SECONDS` in the board `config.h`, with a default of 5 minutes and no runtime selector. Automatic power-off offers 30 minutes, 1 hour, 3 hours or 24 hours of accumulated standby, with 30 minutes as the default. Idle CPU frequency, network grace and power-off settings are saved asynchronously in NVS namespace `power`. Timeout changes share the storage worker with an internal RAM stack, coalesce pending updates and retry failed writes at a limited rate.

Network grace and standby shutdown options and defaults are configured in the selected board’s `config.h`: `BOARD_NET_GRACE_OPTIONS_SECONDS`, `BOARD_NET_GRACE_SECONDS`, `BOARD_STANDBY_OFF_OPTIONS_SECONDS`, and `BOARD_STANDBY_OFF_SECONDS`. The table shows the shipped configuration. Saved NVS values remain effective when included in the configured options; otherwise the board default is used.

| Setting | NVS key | Choices | Default |
|---|---|---|---|
| Idle CPU frequency | `idle_mhz` | 160 / 240 / 320 MHz | 320 MHz |
| Inactivity to standby | Not stored | Board `BOARD_IDLE_TO_STANDBY_SECONDS`; no runtime selector | 300 seconds |
| Network grace | `net_grace_s` | 0 / 30 / 60 / 120 seconds; 0 means never disconnect for inactivity | 30 seconds |
| Accumulated standby to power-off | `standby_off_s` | 1800 / 3600 / 10800 / 86400 seconds; other values use the default | 1800 seconds |

- The default idle frequency is 320 MHz, not an automatic low-frequency setting. Only `AppIdle` uses it; other modes request `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`. Fixed-frequency control requires `CONFIG_PM_ENABLE`. This layer disables SDK automatic light sleep and lets the standby task invoke sleep explicitly. `PowerHwCpuFreqHold` can retain operating frequency during critical work.
- Ordinary Idle network grace and standby timing run in parallel from the last user activity. Provisioning/activation has separate evaluation and considers grace after its inactivity threshold. Soft network holds keep networking available without overriding automatic standby.
- Never disconnect for inactivity does not prevent standby from stopping networking. Soft holds and wake recovery do not override manual Wi-Fi-off. STA pause stops scanning, reconnection timers, and radio activity while retaining netif/event context.
- Automatic power-off accounts for awake cleanup within the current standby session and completed timer-sleep slices. It resets on exit and is not persisted across reboot; it is not simply measured CPU sleep time.

## Hardware power-off path

The power-off task uses a 16 KiB PSRAM stack for its synchronous storage, LVGL redraw and display cleanup calls. It must not erase or write Flash/NVS directly; persistence writes run on the storage worker's internal-RAM stack.

A key assigned to Power off or the accumulated standby deadline requests hardware power-off. The policy aborts log upload and attempts the touch sleep hook. The power-off task cancels and waits for font conversion, turns PA off, restores peripheral supplies and the display bus, attempts SD restoration for wallpaper loading, waits for the shutdown frame's physical refresh, then turns off the frontlight and sends the board power-off pulses. An unavailable SD uses the default shutdown image; failed physical refreshes are retried within the attempt budget. Failed preparation retains power and restores policy, input and refresh. The power circuit removes power: this is not `esp_deep_sleep_start()` and must not be confused with standby cleanup order. See [display documentation](../../../ui/README.md) for wallpaper loading and fallback behavior.

A standby long-press shutdown does not wake touch first. After requesting shutdown, the LP session ends without waiting for POWER release. Holding POWER to the board's hardware forced-off limit can still cut power directly; the software refresh wait cannot prevent it.

## S31 SDK sleep compatibility and diagnostics

[s31_sleep.cc](../../../boards/common/power/s31_sleep/s31_sleep.cc) restores the DREG/PVT prerequisites and analog initialization before MPLL calibration on light-sleep wake. Low-level wrappers apply only within the current SDK sleep scope. Cache-off or stalled-peer-CPU operations use IRAM and low-level registers without logging, allocation, or cross-core locks; scope flags are cleared on return.

[s31_sleep_sdk.cmake](../../../boards/common/power/s31_sleep/s31_sleep_sdk.cmake) inspects the SDK's S31 PMU source and, when needed, generates a compatibility copy inside the build directory. Before sleep it hands PSRAM clocks to XTAL and handles MSPI clocks according to CPU-retention conditions; wake restores MPLL/MSPI before switching PSRAM clocks back. A recognized handoff implementation skips this patch. If a patch is needed but source anchors no longer match, configuration fails for manual review. The installed SDK is not rewritten, and clean builds regenerate the copy. SDK upgrades require reviewing both the handoff patch and MPLL wrappers; skipping copy generation does not prove all workarounds are unnecessary.

The source cites Espressif commit `9726078b55430ef8d2ce4a460ebf21d889bb5c12` for analog initialization. The RTC watchdog remains enabled. CPU retention and PSRAM halfsleep follow the current SDK configuration; protection is not disabled to work around wake failures.

Diagnostics use the existing `PowerPolicy` / `PowerHw` logs:

- `wait_paint` waits for content; `SD drain pending` means SD is not drained; `epd_park` waits for display parking; `pre_sleep` / `post_sleep` bracket the sleep call. These logs are not proof of physical rail removal or low current.
- Retained RTC snapshots record cycle, phase, applied hardware bits, and registers. See [s31_sleep.h](../../../boards/common/power/s31_sleep/s31_sleep.h) for phase definitions. `9` marks return from hardware restoration during error rollback; phase 6 means key sampling is restored while peripherals remain parked; `501/502` bracket the actual MPLL calibration call. Interpret them with reset reasons and error codes, not as standalone success flags.
- Previous snapshots are replayed 10, 30, and 60 seconds after startup so they remain readable after USB re-enumeration. Sleep diagnostics do not write to SD while asleep.
- Setting `kStandbyLpUseLightSleep = false` in `power_policy_priv.h` replaces sleep with polled delays for diagnosis only. Its power consumption and wake behavior cannot validate hardware light sleep.

Applications should hold `PowerNeedHold` for actual activity lifetime and release it before task exit. UI callbacks request cancellation/restoration without waiting for network, audio, or SD tasks. New background work must support standby cancellation, resource return, and demand-driven recovery; hiding its page is insufficient if it still owns files or shared power.

# Bluetooth chip audio protocol

[中文](ble_chip-audio-protocol.md)

## 1. Overview

ble_chip uses UART AT commands for mode control and I2S for host audio. Modes cover local audio, Bluetooth transmit, Bluetooth receive, low power, Bluetooth voice/HID and USB voice/HID. Availability depends on the chip variant, module firmware and feature configuration; ble_chip_G, ble_chip_H and dongle firmware are not assumed interchangeable.

The 470 interface currently exposes modes 1, 2, 3 and 5. External Bluetooth audio and the on-chip BLE page turner are independent connections. The sections below describe mode commands, responses, timing and limitations.

## 2. Current integration

- [Board configuration](../main/boards/metalio-e-ink4-plus/config.h): pins and sample rates.
- [SimpleUart](../main/boards/common/audio/bluetooth/simple_uart.hpp): raw UART, page callbacks and observers.
- [BluetoothAudio](../main/boards/common/audio/bluetooth/bluetooth_audio.cc): power and mode initialization.
- [AT handling](../main/business/bluetooth/bluetooth_control.cc): modes, names, version, SCO and HID.
- [Screens](../main/ui/apps/settings/bluetooth/bluetooth_screen.cc): discovery, connection, reset and modes.
- [I2S codec](../main/boards/common/audio/bluetooth/bt_audio_codec.cc): audio transport.
- [Power policy](../main/sysmgmt/power/policy/README.en.md): shared supply and reset.

The host uses UART1 at 115200 baud, 8N1, without flow control. Host pin assignments are maintained in board configuration. GPIO7/8 in the extension protocol refer to the **ble_chip side**, not ESP host pins.

AT commands terminate with CR; CRLF is also accepted. Current ordinary commands use CRLF and HID commands use CR. Escapes below represent actual control bytes. HID replies use CRLF. The business controller splits on LF and trims whitespace, while the boot observer accepts CR or LF. UART read chunks are not message boundaries.

Audio travels over I2S, separately from AT control. The host is an I2S slave clocked by ble_chip, configured for 16 kHz and stereo 32-bit slots. Capture shifts 32-bit values right by 12 and clips to 16 bits; playback feeds both slots from mono samples. Valid bits, slot sources and per-mode microphone routing must match the module firmware.

## 3. Operating modes

### Mode 1: local audio

```text
AT+RX=2\r\n
wait 700 ms
AT+MODE=1\r\n
reply: SET MODE 1
```

This mode provides amplifier playback and delivers microphone audio to the host. This is the boot default. After power settles, boot initialization waits up to 1000 ms for the mode acknowledgement and retries up to three times after the first failure. The settings mode-switch path does not implement the same acknowledgement timeout/retry policy.

### Mode 2: Bluetooth transmit

```text
AT+TX=1\r\n
wait 700 ms
AT+MODE=2\r\n
reply: SET MODE 2
```

This mode plays through nearby Bluetooth speakers or headsets. Stored devices may reconnect with a `RECONNECT DEVICE NAME` report. Headset or onboard microphone capture can feed the host; route-selection commands and slot mapping remain unspecified. Scan, connection and music/call switching are integrated; voice conversation requires a headset with microphone and suitable call support.

### Mode 3: Bluetooth receive

```text
AT+RX=1\r\n
wait 700 ms
AT+MODE=3\r\n
reply: SET MODE 3
```

Integrated for receiving audio from phones or other sources. Lyrics and cover-art messages, encoding, fragmentation and transport remain unspecified. No dedicated lyrics/cover-art handling was found in the current implementation.

### Mode 4: low power

Entry command: `AT+MODE=4\r`, reply: `SET MODE 4`. After entry, communication IO on the external MCU must be high impedance or low. Wake requires holding **ble_chip GPIO20** low for 700 ms. Post-wake mode, acknowledgement and full timing are unspecified.

Mode 4 and its wake sequence are not integrated. The current reset action cycles shared power; it is distinct from this wake procedure. ble_chip GPIO20 must not be confused with ESP GPIO20.

### Mode 5: Bluetooth voice and HID

The project sends `AT+MODE=5\r\n`, recognizes `SET MODE 5` and provides preset HID tests. This mode supports Classic Bluetooth microphone audio and Classic/BLE HID keys; pairing, security and audio formats remain incompletely specified. Availability depends on module firmware and host connection state.

### Mode 6: USB voice and HID

This mode delivers microphone audio and HID keys to a computer over USB. Mode-entry commands, replies, USB descriptors, audio formats and wiring requirements remain unspecified. `AT+MODE=6` cannot be assumed valid solely from the mode number. No mode 6 entry or dedicated handling exists in the current implementation.

## 4. Discovery, connection and SCO

```text
AT+INQUIRING\r\n
  INQUIRING START
  AT+BT:363154329632ble_chip_CY
  INQ COMPLETE

A discovery line contains `AT+BT:`, twelve hexadecimal address digits, then the name without a separator. The current parser follows this format with a 64-byte name buffer including the terminator. Scan cancellation, explicit disconnect, pairing removal, complete disconnect events and connection timeout values are not specified.

Successful connection defaults to A2DP music. Use the following call/music switching sequences:

```text
call:
AT+PP=1\r\n
wait 200 ms
AT+BTSCO=1\r\n
  SETUP SCO

The exact meaning of `AT+PP=1` is undocumented and should not be equated with parameterless `AT+PP`. Current SCO tasks use fixed delays rather than waiting for the SCO reports before continuing.

## 5. Other AT commands

- **Version**: send `AT+GVER=1\r\n`; accept `AT+GVER=V...`, 2–63 bytes, restricted to letters, digits and `.-_+`. Parameterless `AT+GVER\r` compatibility/reply format is unconfirmed.
- **Name**: `AT+GLBN=1\r` returns `AT+LOCAL_NAME=<name>`. `AT+SLBN=<name>\r` restarts after about 700 ms. Reject the command-shaped echo, restore mode and query again. Host setters allow 1–63 bytes without control characters; module limits are unspecified. The UI is read-only.
- **Media**: `AT+PP\r` toggles playback, with `AT+PREV\r`, `AT+NEXT\r` and `AT+VOLDOWN\r`. Exact volume-up spelling is unconfirmed; do not assume `AT+VOLUP` is valid. `MPAUSE` conditions and dedicated handling are unspecified. Host software volume is independent.
- **Sample rate**: mode 1 lists `AT+SAMPLE=N\r` for 8000, 16000, 44100, 48000, 88200 or 96000. Replies, default, persistence and restart requirements are unspecified. It is not integrated; coordinate the whole I2S/audio path before changing the configured 16 kHz.
- **UART baud**: `AT+SUARTBAUD` syntax/replies are unspecified. The host initializes at 115200 without dynamic switching.

## 6. HID shortcuts

Syntax allows multiple modifiers and at most one ordinary key, or a modifier/key alone, without spaces. Uppercase letter names do not imply Shift. Numeric HID keycodes are unsupported.

- Modifiers: `LCTRL/RCTRL`, `LSHIFT/RSHIFT`, `LALT/RALT`, `LGUI/RGUI`, `LWIN/RWIN`; `CTRL/SHIFT/ALT/GUI/WIN` alias the left keys.
- Keys: case-insensitive `A`–`Z`, `0`–`9`, `F1`–`F12`, `ENTER ESC BACKSPACE TAB SPACE`.
- Symbols: `MINUS EQUAL LBRACKET RBRACKET BACKSLASH SEMICOLON QUOTE GRAVE COMMA DOT SLASH`. Use `SHIFT+EQUAL` for plus; output depends on the host layout.
- System/navigation: `CAPSLOCK PRTSC SCROLLLOCK PAUSE MENU INSERT HOME PGUP DELETE DEL END PGDN RIGHT LEFT DOWN UP`.

```text
AT+SHORTCUT=CTRL+C\r
AT+SHORTCUT=CTRL+SHIFT+ESC\r
AT+SHORTCUT=ALT,DOWN\r
AT+SHORTCUT=ALT,UP\r
AT+SHORTCUT=RELEASE\r
```

A click presses and releases automatically. Hold and release must be paired; `RELEASE` clears all keys. The hold sequence above uses Alt; support for other held combinations remains unspecified. The invalid examples above must not be sent as valid shortcuts.

Replies terminate with CRLF. `OK` means valid parameters and an asynchronous request submitted with an HID connection present, not host execution. `ERR` indicates invalid parameters/format; `DISCONNECTED` indicates disabled HID or no connected HID host.

The mode 5 test page uses presets, including Alt hold/release and release-all. Release handling covers touch release/cancellation, page exit and mode changes. There is no arbitrary shortcut editor or request-sequence correlation for HID replies.

## 7. UART GPIO extension V3.0

### 7.1 Firmware and coexistence

This protocol applies to ble_chip_dongle firmware with `GPIO_EXT_ENABLE` enabled. Default address is `01`, with 32 ble_chip GPIOs. GPIO0/1 and GPIO7/8 are reserved for UART0/1; additional reservations are configurable. Enablement on the fitted 470 module is unconfirmed.

AT and binary traffic share ble_chip UART1. Binary payload bytes are opaque; AT text runs from a complete prefix through CR, including UTF-8 bytes. Send binary requests after the AT line terminates. Fragmentation and alternating message types are supported. Payloads over eight bytes are discarded by declared length before a length error is returned. Incomplete frames keep waiting for the declared length. Stale-frame timeout and resynchronization are unspecified; retry alone is not guaranteed to recover.

### 7.2 Frames and status

```text
Request:
A5 ADDR SEQ CMD LEN PAYLOAD[LEN] CRC_H CRC_L
Response:
A5 ADDR SEQ (CMD | 80) STATUS LEN PAYLOAD[LEN] CRC_H CRC_L
Event:
A5 ADDR 00 C0 00 02 PIN MODE CRC_H CRC_L
```

CRC-16/CCITT-FALSE uses initial `FFFF`, polynomial `1021`, no reflection and final XOR `0000`, covering ADDR through PAYLOAD, including response STATUS. CRC is high byte first. The event layout includes STATUS `00`. Events are unsolicited, not replies to the last request. Other addresses receive no reply.

Status codes: success, CRC error (reply command `FF`), unknown command, invalid length, invalid pin and invalid parameter, respectively. Invalid parameters include reserved UART pins and invalid VAL/MODE.

### 7.3 Six commands

- `00 PING`: no request or success payload.
- `01 WRITE_PIN`: request `PIN,VAL`, `VAL` 0/1; selects output, disables the pin interrupt, drives the level and returns `PIN,VAL`.
- `02 READ_PIN`: request `PIN`, response `VAL` 0/1; reads the actual pin state without changing direction.
- `03 CONFIG_INT`: request/response `PIN,MODE`; selects input with internal pull-up; mode 0 disabled, 1 rising, 2 falling. Both-edge mode is unsupported.
- `04 GET_INFO`: no request payload; response `VER,GPIO_COUNT,FLAGS`, V3 `03,20,01`, with bit 0 advertising events.
- `05 RESET`: no request/success payload. After successful reply, restores inputs and disables interrupts; it does not reboot or restore audio mode.

Events carry PIN/MODE and clear pending state after transmission. Falling-edge mode remains falling-edge; a pin already low must first rise before a falling transition can occur. Initialization establishes software defaults without changing existing pinmux. Actual changes occur on WRITE_PIN, CONFIG_INT or RESET. Debouncing, edge counting and lossless events are not guaranteed; check board wiring before using any non-UART pin.

Events are sent by a task; the ISR only marks pending state and signals it. Full-duplex UART allows events during host transmission. RESET waits for up to 129 UART byte times plus tick rounding after successful reply submission before restoring defaults; failed reply transmission cancels the reset.

### 7.4 Frame examples

All values below are hexadecimal bytes using address `01` and **ble_chip GPIO18**. Confirm the pin is safe to operate. Do not append CR/LF to binary frames.

```text
PING TX:          A5 01 01 00 00 C5 44
PING RX:          A5 01 01 80 00 00 F6 B3
GET_INFO TX:      A5 01 02 04 00 50 D0
GET_INFO RX:      A5 01 02 84 00 03 03 20 01 10 C4
WRITE high TX:    A5 01 03 01 02 12 01 C8 86
WRITE high RX:    A5 01 03 81 00 02 12 01 14 9E
WRITE low TX:     A5 01 03 01 02 12 00 D8 A7
WRITE low RX:     A5 01 03 81 00 02 12 00 04 BF
READ TX:          A5 01 05 02 01 12 68 3A
READ high RX:     A5 01 05 82 00 01 01 7B A7
READ low RX:      A5 01 05 82 00 01 00 6B 86
FALLING TX:       A5 01 04 03 02 12 02 72 59
FALLING RX:       A5 01 04 83 00 02 12 02 A8 3F
FALLING EVENT:    A5 01 00 C0 00 02 12 02 51 24
RISING TX:        A5 01 04 03 02 12 01 42 3A
RISING EVENT:     A5 01 00 C0 00 02 12 01 61 47
INT disable TX:   A5 01 06 03 02 12 00 16 98
INT disable RX:   A5 01 06 83 00 02 12 00 03 3D
```

SimpleUart transports raw bytes, but the current upper layer has no GPIO framing, CRC, sequence matching or event dispatch. Integration requires shared UART demultiplexing plus event, timeout and incomplete-frame handling before binary data reaches the line-oriented page parser.

## 8. Persistent custom key-value storage

Requires **ble_chip Bluetooth firmware 1.08 or later**. Numeric index X is the key and user data Y is the value. This is module storage, separate from host NVS and eFuse. Earlier Bluetooth firmware does not support these user-data commands.

```text
AT+WDATA=X,Y\r\n
  AT+WDATA=OK
AT+RDATA=X\r\n
  AT+RDATA=X,Y
AT+FLUSHDATA=1\r\n
  AT+FLUSHDATA=OK
```

X ranges from 0 to 999; Y is at most 200 bytes. Confirm FLUSHDATA before power removal; WDATA acknowledgement alone does not establish persistence. Dedicated repair, deletion, factory reset, empty-record behavior, full error responses, Flash atomicity and endurance are unspecified.

Example write, persistent save and read using index 123; confirm the index is unused before assigning it:

```text
AT+WDATA=123,factory_value\r\n
  AT+WDATA=OK
AT+FLUSHDATA=1\r\n
  AT+FLUSHDATA=OK
AT+RDATA=123\r\n
  AT+RDATA=123,factory_value
```

To update a key, repeat WDATA and FLUSHDATA, then verify with RDATA. Reading does not flush pending writes. Power-cycle and read again to verify persistence.

The 470 firmware reserves slot 470 for panel calibration, using write, Flash flush and readback verification. A persistent observer handles responses; transactions, mode commands and shared-power reset share the UART mutex. See the [VCOM factory guide](../tools/vcom/README.md) for storage and tool details.

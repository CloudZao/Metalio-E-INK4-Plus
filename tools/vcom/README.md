# VCOM factory tool

[中文](README_zn.md) | **English**

A USB scanner supplies keyboard input to the computer. The page sends calibration requests through the device's native USB Serial/JTAG port. Firmware writes the BH1098 record, flushes it to Flash, verifies readback and queues the negative millivolt value for the display worker. Operation results also appear in the device’s top notification. After the device confirms local save, HTML uploads with POST using the computer network. It reports “VCOM saved and synced” only after server confirmation.

## Usage

Requires **BH1098 Bluetooth firmware 1.08 or later**; earlier versions do not support persistent calibration storage and retrieval.

From the repository root, run the following command and open the [local tool](http://localhost:8765) in desktop Chrome or Edge:

```sh
python -m http.server 8765 --bind 127.0.0.1 --directory tools/vcom
```

An existing HTTPS static server can also serve these files. No frontend dependencies are required. Flash firmware containing the factory service and close serial monitors first. Select “Connect device” and choose its native USB Serial/JTAG port. A UART adapter, the BH UART and the virtual USB disk are different interfaces.

Connection automatically reads the BH record. “Restart device”, on the right of the serial-log header, pulses DTR/RTS for a hardware reset and is disabled during active operations. No firmware command is required. The port stays open for startup logs; reconnect if USB re-enumeration disconnects it. Sending the pulse does not establish successful boot and does not replace a BH power-cycle persistence test. Configure the scanner for standard alphanumeric keyboard input with an Enter suffix and focus “Scanner input”. The field captures physical key codes to avoid IME composition and also accepts pasted ASCII text. A separator is optional, for example `E0470A14-AF_TEST-1.38`. A complete scan such as `E0470A14-AF_TEST -1.38` is converted to `-1380mv` and submitted automatically after validation. “Automatically write and verify” is enabled by default; turn it off to populate the fields without submitting, then use the manual button. A new scan clears the previous identifier and voltage so invalid input cannot reuse stale parameters. This is a format example only; use the actual label on the installed panel. Alternatively, edit the panel identifier and negative millivolt value and select “Write and verify”. Only one operation runs at a time. Results appear as nonblocking notifications that disappear after five seconds, require no confirmation and do not take scanner focus.

The device test menu has no VCOM calibration entry. Reading and writing use the HTML tool. Settings → Screen displays the last confirmed BH value as “Black depth”, for example `Black depth: -1300mv`, without preset levels.

## Serial log

The serial log shows USB serial TX and RX text verbatim, including device logs, protocol responses and fragmented input. Display can be paused or cleared. Only the latest 128KB remains in the page; nothing is saved or uploaded automatically. BH UART traffic is visible only if the host firmware forwards it.

## Storage and interface

BH slot `470` is reserved by this project. Its record is `V1|negative millivolts|panel identifier`, for example `V1|-1380|E0470A14-AF_TEST`. Other software must not use this slot. Identifiers allow up to 79 ASCII letters, digits, hyphens and underscores.

USB requests and responses use the `@VCOM ` prefix, line-delimited JSON and CRLF. `id` is an integer from 1 to 2147483647. The tool accepts only responses matching its active operation.

```text
@VCOM {"id":1,"cmd":"panel.get"}
@VCOM {"id":2,"cmd":"panel.set","panel_id":"E0470A14-AF_TEST","vcom_mv":-1380}
```

Writes execute `AT+WDATA=470,...`, `AT+FLUSHDATA=1`, then `AT+RDATA=470`. Example response:

```json
{"id":2,"ok":true,"code":"OK","stage":"done","vcom_mv":-1380,"applied_mv":-1380,"panel_id":"E0470A14-AF_TEST","local_saved":true,"board_model":"metalio-e-ink4-plus","device_mac":"00:11:22:33:44:55"}
```

`vcom_mv` is the last confirmed BH value. `applied_mv` is the display worker's configuration, not a measured voltage. Applying it is asynchronous after the current waveform, so the two may briefly differ. Device identity uses the host Wi-Fi STA MAC. This interface is intended for controlled local factory connections.

Failures include `code` and `stage`, distinguishing `ready`, `write`, `flush`, `readback`, `apply`. Examples include `BH_TIMEOUT`, `BH_FLUSH_REJECTED` and `BH_VERIFY_MISMATCH`. Local failures do not start a cloud request. After a timeout or failed write, scan again or write manually to retry, or read the device to check. Failed device writes are retried up to three additional times, one second apart, using the original parameters. Disconnecting cancels pending retries. Cloud failures do not rewrite the device.

## Cloud interface

Manage the cloud URL in the server address field; it is empty by default. Remember address is enabled by default to save and restore the address in this browser; disabling it removes the saved address. Browser storage restrictions may prevent saving. Query cloud displays the cloud VCOM for the connected device separately without replacing local calibration or writing the device.

The device only writes, saves and verifies local calibration. HTML starts an HTTPS POST over the computer network after `ok=true` and `local_saved=true`. It sends `Content-Type: application/json` and fields `board_model`, `device_mac`, `vcom_mv`, without additional credentials. The server must allow the page origin, GET, POST and Content-Type through CORS preflight. Browsers cannot distinguish network failures from CORS rejection; both report connection failure.

Success requires HTTP 200, JSON `code=0` and matching model, case-insensitive MAC and VCOM in `data`. Requests time out after 15 seconds. Device write failures and cloud failures have separate notifications. “Retry sync” only uploads again, without rewriting the device. Firmware and the serial protocol no longer perform cloud sync.

## Limits


- Use only the panel label's calibration value. Both the page and firmware require a magnitude of 1.0–3.0V (-3000 to -1000mv, inclusive), in 10mv steps. Out-of-range values report “Abnormal value” and are neither written nor applied. Automatic calibration is not provided.
- An unavailable or invalid boot record retains the board's default driver value and displays “Unknown”. Runtime failures retain the last confirmed driver configuration while black depth displays “Unknown”. Defaults or historical values are never represented as the current read result. Legacy black-depth presets are no longer loaded.
- Readback follows write and Flash acknowledgements. Power-cycle and read again to verify actual persistence. The BH protocol does not define transaction IDs, all error responses or Flash atomicity. A timeout can mean an unknown result rather than a failed save.
- The applied value describes software configuration. TPS register writes and physical voltage still require hardware verification. If another feature changes the USB role, or the port is removed or occupied, restore the native serial interface and reconnect.
- A cloud failure retains the locally saved driver calibration. “Retry sync” only uploads again and does not rewrite BH Flash. Sync state is kept in RAM; there is no persistent pending queue or automatic retry. After reboot, read the local record before manually syncing.

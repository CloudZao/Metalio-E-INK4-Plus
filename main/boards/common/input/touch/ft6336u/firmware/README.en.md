# FT6336U V04 upgrade

[简体中文](README.md)

The embedded `ft6336u_v04_app.bin` comes from the supplier's
`LT0470GF0430M_V1.1_FT6336U_684x1216_FT6336U_V04_20261106_app.bin` for this board's FT6336U panel.
The file contains 47,176 bytes; the header specifies a 47,096-byte application.
Target version: `0x04`; application XOR: `0xFA`; file CRC32: `0xAFF8E72B`.
SHA256: `60a660a4f80bf7aa8000a2c597f83620ea9b815bec1db31be2c8002cce1ee089`.
Do not flash `all.bin` or pass the touch image to esptool.

Set `BOARD_TOUCH_UPGRADE_ON_BOOT` in `main/boards/metalio-e-ink4-plus/config.h`
to `1` (the shipped default), then build and flash the ESP32 application.
Set it to `0` to disable version checks and upgrades and exclude the touch image.
The setting must be a single-line literal `0` or `1`. At startup, before touch sampling, LVGL and background
tasks, the updater exclusively uses I2C1 to update any version other than `0x04` to V04.
Valid V04 applications are skipped. Invalid application information, including `0xFF`, triggers bootloader identification.
Keep power connected throughout the update. Do not use this image for other panels.

The updater checks image integrity, chip IDs, bootloader IDs, packet status,
application XOR and the version after restart. The `touch_upgrade` log reports
`updated and verified V04` on success or `already current; skipped` when no update
is needed. Application IDs and version are polled every 200 ms for up to one second.
Invalid application information triggers a 0x07 reset and Boot ID identification;
a valid application enters upgrade mode through the 0xBC AA/55 sequence.
Boot ID is checked up to 30 times, and erasure requires bootloader ID 79/1C.
A failed attempt requests bootloader exit and retries once. The second attempt
performs a complete write regardless of the version reported by a partially written application.
Unrecognized bootloader IDs never permit erasure. Touch may remain unavailable
after both attempts fail; check power, reset and communication, or use supplier recovery tools.
Application fallback and the two-attempt limit follow the supplier Qualcomm V3.4
driver functions `fts_get_ic_information()`, `fts_fwupg_enter_into_boot()` and
`fts_fwupg_upgrade()`. The flashing protocol follows the supplier's FT6336U I2C V4.1 driver; Linux driver code
was not copied directly.

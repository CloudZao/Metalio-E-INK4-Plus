# Documentation index

[中文](README_zn.md) | **English**

See the [project README](../README.md) for features/getting started and the [tool index](../tools/README.md) for commands/tools. Topics below own their full content; this index provides navigation. Existing Chinese-only topics are labelled.

## Usage and features

- [Device usage](device-guide.md)
- [SD layout and data protection](sd-storage.md)
- [App Store](app-store_en.md)
- [Keys and gestures](key-bindings.md)
- [Wi-Fi configuration (Chinese)](wifi-config.md)
- [Library search and dictionary](book-search.md)
- [System fonts and extensions](ui-font-extension.md)
- [BLE page turners](ble-pager.md)
- [WorkBuddy](../main/business/workbuddy/README.en.md)

## Development and interfaces

- [Boards, module boundaries and integration](hardware-adaptation.md)
- [ELF apps, loader and development](../tools/elf_app/README.md)
- [UI scheduling and reader work](ui-scheduling.md)
- [Display and refresh](../main/ui/README.md)
- [Flash partitions](../partitions/README.md)
- [Local file transfer (Chinese)](ap-file-transfer.md)
- [Bluetooth audio protocol](ble_chip-audio-protocol.en.md)
- [Battery and charging (Chinese)](battery-soc-and-charge.md)
- [Power policy](../main/sysmgmt/power/policy/README.en.md)
- [Board power and audio startup](metalio-e-ink4-plus.en.md)
- [WorkBuddy audio and task lifecycle](../main/business/workbuddy/README.en.md#audio-tasks-and-standby)
- [A2UI rendering](../main/ui/common/drawing/a2ui/README.md)
- [JPEG implementation](../main/infra/image/jpeg/README.md)
- [Dithered gray backgrounds (Chinese)](epd-bayer-dither-gray.md)

## Diagnostics and test material

- [SPV timing diagnostics](epd-spv-investigation.en.md)
- [Audio self-test diagnostics (Chinese)](audio-self-test-diagnostics.md)
- [Hardware test matrix and historical records](hardware-test-matrix.md)
- [Host regression tests](../tools/tests/README.md)
- [Dependency checks](../tools/checks/README.md)
- [VCOM manufacturing](../tools/vcom/README.md)
- [Touch firmware](../main/boards/common/input/touch/ft6336u/firmware/README.en.md)

## Design proposals

- [SD resource upgrade proposal (unimplemented, Chinese)](sd-font-resources-upgrade.md)

## Maintenance

The root README owns product-level entry points; topics own functional domains; tools/modules own usage and contracts; individual ELF apps own their operations, behavior and examples. Maintain one complete source per subject and link summaries elsewhere. Update languages, inbound links, relative paths and anchors together. Proposals and historical tests are not current capabilities or verification guarantees.

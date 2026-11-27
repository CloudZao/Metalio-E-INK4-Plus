# Device usage

[中文](device-guide_zn.md) | **English**

This page covers on-device operations. Topic documents own scheduling, protocols and hardware parameters; see the [documentation index](README.md).

## Home and navigation

Home pages show built-in functions, the Store and installed apps. Entries vary by board and Domestic/International configuration. Short HOME follows page-specific back handling; long HOME usually returns home. See [keys and gestures](key-bindings.md) for bindings.

Swipe down in an area without actual vertical overflow to open quick controls; long lists retain scrolling priority. Controls include Wi-Fi, frontlight, brightness and warmth. Swipe up, tap outside or use system back to close. OTA and standby disable the panel.

## Reading

Put EPUB, TXT and `.ebook` files in the product books directory, select a book and open it. Reading provides progress, contents, bookmarks, fonts and layout. Text direction is independent of screen rotation. Layout changes prepare new pages; loading does not prevent leaving or standby.

Settings cards cover direction, information bars, touch regions, background, layout, antialiasing, spacing, underlines and page turning. Volume keys default to page turns in body text; overlays follow their page rules and custom bindings take priority. Enable shake turning in its settings and connect BLE page turners in Settings.

See [library search](book-search.md), [system fonts and extensions](ui-font-extension.md), [UI and reader tasks](ui-scheduling.md), and [desktop conversion](../tools/ebook/README.md).

## AI, checklist and WorkBuddy

Baiwen accepts voice and displays paginated replies; cloud checklists can be viewed on the device. Network access, device binding and service configuration are prerequisites for their cloud features. Camera tools require successful hardware detection and a configured vision service; restart detection after reconnecting hardware.

WorkBuddy requires service support and the computer assistant online. Domestic variants expose it; International home screens omit it. See [WorkBuddy](../main/business/workbuddy/README.en.md) for binding, operations and limits.

## Wallpaper and transfer

Wallpaper manages standby/shutdown images, with full-screen or transparent choices. See [SD storage](sd-storage.md) for resource locations and deletion effects.

Transfer includes cloud push, AP, same-network Wi-Fi and USB. Cloud push requires a backend; follow on-device connection instructions for local services. See [file transfer](ap-file-transfer.md) for local operation and limits. Stop device file activity before USB export, safely eject on the computer, then disable the virtual disk. Do not edit files concurrently from both sides.

## Settings and connectivity

Settings group networking, Bluetooth, page turners, themes, fonts, frontlight, display, vibration, input, conversation, power, language, tests and device information; available items vary by board.

Manually disabling Wi-Fi persists the choice and stops scanning, connections and wireless transfer. Business features do not force it back on. Configurations supporting 4G can change network mode with a restart; International variants are Wi-Fi only. See [Wi-Fi configuration](wifi-config.md) for SD configuration and startup synchronization.

External Bluetooth audio is independent from on-chip BLE page turners. See the [audio protocol](ble_chip-audio-protocol.en.md) for modes and compatibility and [BLE page turners](ble-pager.md) for pairing, learning and reconnection.

Preview SD TTF/OTF system fonts before applying them. Frontlight supports brightness/warmth controls. Refresh options vary by board; see [display and refresh](../main/ui/README.md).

## Power, tests and maintenance

Power settings manage inactivity standby and shutdown. Active features may defer inactivity standby; the system still handles manual power-key standby. See [power policy](../main/sysmgmt/power/policy/README.en.md) and [battery/charging](battery-soc-and-charge.md).

Tests are for hardware checks and manufacturing; normal use does not require calibration changes. VCOM uses its dedicated [manufacturing tool](../tools/vcom/README.md). Avoid credentials and user content in saved logs; use the documentation index for diagnostics.

## International variants

`470-global` and `397-global` share their corresponding hardware and default to English when no language preference is saved; saved preferences take priority. They use Wi-Fi only, omit 4G/SIM controls and hide WorkBuddy from home. AI services requiring binding show instructions in their pages; binding does not imply a purchased plan. Firmware upgrades and binding are separate operations.

# Calendar

[中文](README_zn.md) | **English**

## Features and usage

Browses the Gregorian calendar for 1600–2399, with month/year navigation, date selection, today marking and return to today.

## Example

February 2024 includes day 29. Startup and Today read the host date; when unavailable, browsing starts at January 2000 without a today marker.

## Source and call flow

`ReadToday` checks the optional date field and range; `Leap`, `Days` and `Weekday` calculate the calendar. `Refresh` updates 42 cells and selection. Selection is not written to private storage.

## Compatibility and limits

Requires at least 600×1000. No lunar calendar, holidays, reminders or background calendar sync; a missing host date is not a valid today.

[calendar.c](calendar.c) · [Shared interfaces and lifecycle](../README.md#5-capability-interfaces)

## Build and installation

From the repository root, activate the matching ESP toolchain and set `$compiler` to the full RISC-V compiler path:

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/calendar -Target esp32s31 -OutputDirectory ./tools/elf_app/calendar/build/470
```

Output is `build/470/calendar.elf` relative to this app. See [shared instructions](../README.md#8-publishing-and-device-operations) for the environment, installation manifest and publication requirements.

# Unit Converter

[中文](README_zn.md) | **English**

## Features and usage

Converts length, mass, temperature and area, with source/target selection, unit swap, signed decimal input and 12 significant digits.

## Example

Select temperature, °C to °F, and enter `100` to obtain `212`. Values below absolute zero are rejected. Check source and target after changing category.

## Source and call flow

`unit_converter.c` owns category, units and input. `Convert` maps through a standard unit and validates absolute zero for temperatures. `Refresh` updates selections and results; decimal parsing/formatting uses `common/decimal.h`.

## Compatibility and limits

Uses `AppInit` and requires at least 600×1000; the current 397 layout is unsupported. State lasts for the current run only.

[unit_converter.c](unit_converter.c) · [Shared interfaces and lifecycle](../README.md#5-capability-interfaces)

## Build and installation

From the repository root, activate the matching ESP toolchain and set `$compiler` to the full RISC-V compiler path:

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/unit_converter -Target esp32s31 -OutputDirectory ./tools/elf_app/unit_converter/build/470
```

Output is `build/470/unit_converter.elf` relative to this app. See [shared instructions](../README.md#8-publishing-and-device-operations) for the environment, installation manifest and publication requirements.

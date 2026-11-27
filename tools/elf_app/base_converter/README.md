# Base Converter

[中文](README_zn.md) | **English**

## Features and usage

Displays one unsigned 32-bit value in binary, octal, decimal and hexadecimal. Binary is byte-grouped; invalid digits are disabled for the selected input base.

## Example

Enter decimal `255` to see hexadecimal `FF` and octal `377`. Changing input base retains the numeric value. Overflow reports an error and keeps the previous value.

## Source and call flow

`Click` handles base selection, reset, backspace and digit append, checking overflow with `(UINT32_MAX-key)/base`. `Refresh` updates all four representations and key states.

## Compatibility and limits

Range is 0–4294967295, without signed/two's-complement interpretation, fractions or arbitrary precision. Requires at least 600×1000; state is not persisted.

[base_converter.c](base_converter.c) · [Shared interfaces and lifecycle](../README.md#5-capability-interfaces)

## Build and installation

From the repository root, activate the matching ESP toolchain and set `$compiler` to the full RISC-V compiler path:

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/base_converter -Target esp32s31 -OutputDirectory ./tools/elf_app/base_converter/build/470
```

Output is `build/470/base_converter.elf` relative to this app. See [shared instructions](../README.md#8-publishing-and-device-operations) for the environment, installation manifest and publication requirements.

# Package Check

[中文](README_zn.md) | **English**

## Features and usage

Diagnoses large-package installation and on-demand SD resources rather than serving as a reader. The ELF embeds 16 MiB of deterministic data, sampled at runtime.

## Example

Install, open, and select Check resource samples. Success shows 17 / 17 blocks. Installation SHA256 covers the full package; runtime sampling does not replace it.

## Source and call flow

`build_content.py` generates `assets.bin`. `Check` uses a 4096-byte buffer to read 17 evenly spaced blocks across the resource and verifies the deterministic byte pattern without a whole-resource RAM copy.

## Compatibility and limits

Requires at least 600×1000 and, in this implementation, the full current interface structure plus asset reading. Unavailable SD or read failures fail the check.

[package_check.c](package_check.c) · [Shared interfaces and lifecycle](../README.md#5-capability-interfaces)

## Build and installation

From the repository root, activate the matching ESP toolchain and set `$compiler` to the full RISC-V compiler path:

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/package_check -Target esp32s31 -OutputDirectory ./tools/elf_app/package_check/build/470
```

Output is `build/470/package_check.elf` relative to this app. See [shared instructions](../README.md#8-publishing-and-device-operations) for the environment, installation manifest and publication requirements.

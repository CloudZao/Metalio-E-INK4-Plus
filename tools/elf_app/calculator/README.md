# Calculator

[中文](README_zn.md) | **English**

## Features and usage

Supports decimals, four arithmetic operations, percentages, sign changes, backspace, chained operations and repeated equals, with 12 significant digits. Evaluation follows input order without operator precedence.

## Example

Enter `2 + 3 × 4 =` to obtain `20`. Enter a new number after division-by-zero or range errors. AC resets the state; Back edits the current input.

## Source and call flow

`calculator.c` owns entry, UI and clicks; `calculator_engine.h` owns numeric input and arithmetic state. `Click` invokes `CalcInput`, then `Show` updates the result, expression and feedback. `ApplyStyle` uses optional host styling.

## Compatibility and limits

Requires at least 600×900 content space; the current 397 content area is too small. Results are not persisted.

[calculator.c](calculator.c) · [Shared interfaces and lifecycle](../README.md#5-capability-interfaces)

## Build and installation

From the repository root, activate the matching ESP toolchain and set `$compiler` to the full RISC-V compiler path:

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/calculator -Target esp32s31 -OutputDirectory ./tools/elf_app/calculator/build/470
```

Output is `build/470/calculator.elf` relative to this app. See [shared instructions](../README.md#8-publishing-and-device-operations) for the environment, installation manifest and publication requirements.

## Using this app as a template

Read [calculator.c](calculator.c) and [calculator_engine.h](calculator_engine.h) first. The C file handles entry, ABI checks, widget layout, click callbacks and display updates; the private header handles input and calculation state without calling host widgets. `Click` passes keys to `CalcInput`, then `Show` updates the result, expression and hint. `ApplyStyle` uses optional host styling when available.

Build the original calculator from the repository root:

```powershell
$compiler = '<full path to riscv32-esp-elf-gcc.exe>'
./tools/elf_app/calculator/build.ps1 -Compiler $compiler
```

The output is `tools/elf_app/calculator/calculator.elf`. To start another app, copy the directory and keep the directory name and main source name consistent:

```powershell
Copy-Item -LiteralPath ./tools/elf_app/calculator -Destination ./tools/elf_app/my_app -Recurse
Rename-Item -LiteralPath ./tools/elf_app/my_app/calculator.c -NewName my_app.c
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/my_app
```

Keep the `app_main` argument, ABI, interface availability and content-area size checks. Adapt layout, callbacks and business logic to the new app; do not link LVGL or ESP-IDF directly. Replace the calculator-specific `calculator_engine.h` as needed, create your own `icon.pbm` and upload under a new app ID. Passing entry, resource and import checks does not verify device interaction; follow the [shared validation checklist](../README.md#9-validation-and-troubleshooting) before publishing.

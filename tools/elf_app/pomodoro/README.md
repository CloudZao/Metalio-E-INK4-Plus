# Pomodoro

[中文](README_zn.md) | **English**

## Features and usage

Offline focus, short-break and long-break timing, defaulting to 25 / 5 / 15 minutes with a long break after four focus sessions. Includes start, pause, resume and reset confirmation.

## Example

Choose a stage and start. Pause before changing stages. Confirm reset by clicking again within the confirmation window. Exit saves and pauses; resume manually after reopening. Reminders are foreground screen notices only.

## Source and call flow

`pomodoro.c` owns UI, the one-second managed callback, private storage and keep-awake. `pomodoro_engine.h` owns timing state. Monotonic time advances the timer; snapshots save each minute and at key actions, and exit pauses/saves.

## Compatibility and limits

Requires at least 600×900 plus time, timer, storage, widgets, exit, notice and keep-awake capabilities. Running prevents inactivity standby without holding networking; the power key can still enter standby. Power loss may lose progress after the last snapshot. No background alarm or wake capability.

[pomodoro.c](pomodoro.c) · [Shared interfaces and lifecycle](../README.md#5-capability-interfaces)

## Build and installation

From the repository root, activate the matching ESP toolchain and set `$compiler` to the full RISC-V compiler path:

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/pomodoro -Target esp32s31 -OutputDirectory ./tools/elf_app/pomodoro/build/470
```

Output is `build/470/pomodoro.elf` relative to this app. See [shared instructions](../README.md#8-publishing-and-device-operations) for the environment, installation manifest and publication requirements.

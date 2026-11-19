# ELF app tools and examples

[中文](README_zn.md) | **English**

See the [developer guide](DEVELOPMENT.md) for coding rules, ABI and lifecycle, resource formats and the Calculator walkthrough. This directory contains build tools, shared helpers and sample apps. The firmware runtime and authoritative interfaces remain in `main/app_store/`.

Apps use ABI 1 from `main/app_store/app_api.h` and create widgets inside the host content area. Each sample declares its required capabilities through entry checks; apps using extended interfaces require matching firmware. Apps must not retain host references or background tasks after exit.

## Capability overview

The host manages basic widgets, monochrome drawing, time and foreground timing, timers, private data, three-axis acceleration, timed vibration, in-app Back and white-wipe transitions. Button haptics follow system settings.

Audio supports embedded ELF resources and SD files containing Ogg/Opus or PCM WAV, with looping, pause, resume, stop and progress queries, using system volume. Recording exposes one slot with start, pause, resume and stop/save. It produces 16kHz mono 16-bit PCM WAV for direct playback and replaces the previous file only after saving succeeds. Recording and playback are mutually exclusive; multiple-recording management is not exposed.

The host cleans resources on exit and resumes foreground business after the wake frame physically succeeds. Extensions append to ABI 1; apps must check field boundaries, capability bits and function pointers. Parameters and limits are maintained in the [developer guide](DEVELOPMENT.md#5-capability-interfaces).

## Sample apps

- `calculator`: Standard calculator with decimal arithmetic, contextual percentages, sign change, backspace, chained operations and repeat equals. Displays 12 significant digits and evaluates left to right without operator precedence.
- `unit_converter`: Length, mass, temperature and area conversion with direct source/target selection, decimals, sign change, unit swapping and 12 significant digits. Temperatures below absolute zero are rejected.
- `calendar`: Gregorian calendar for 1600–2399 with month/year navigation, date selection, today marking and return to today. Reads the device date on launch and Today. Without a ready clock, starts manual browsing in January 2000 without marking today.
- `base_converter`: Simultaneous unsigned 32-bit binary, octal, decimal and hexadecimal conversion, with byte-grouped binary, unavailable digits disabled and overflow preserving the previous value.
- `english_reader`: Chapter reading, 206 word cards and quizzes; reading position lasts for the current session. The original text is Lewis Carroll’s Alice’s Adventures in Wonderland from [Project Gutenberg ebook 11](https://www.gutenberg.org/ebooks/11). `book.txt` retains the source and the reader includes the full license as an appendix.
- `package_check`: Includes 16 MiB of deterministic resources for large-download and on-demand SD checks. Runtime checks sample 17 blocks across the file; installation SHA256 covers the full package.

- `pomodoro`: A clock-first offline Pomodoro. One screen contains focus, short/long break selection, start, pause, resume and reset confirmation. Defaults are 25 / 5 / 15 minutes with a long break every four focus sessions. No task input, statistics or settings pages. Updates retain saved durations and progress. Exit saves and pauses; reopening requires manual resume.
- `recorder`: a single-file recording/playback example with pause, resume, save, duration and progress; successful new takes replace the previous file using app-private data and temporary paths. See the [app guide](recorder/README.md).
- `gravity_maze`: portrait tilt maze with three levels, Random mode, time limits, five-star scoring and Chinese/English UI. Demonstrates embedded looping music and system volume. Requires drawing, foreground timing, acceleration and audio capabilities; see the [app guide](gravity_maze/README.md).

Build with the ESP32-S31 RISC-V toolchain, with IDF Python on PATH:

```powershell
./tools/elf_app/build_app.ps1 -Compiler <riscv32-esp-elf-gcc.exe> -AppDirectory ./tools/elf_app/package_check
```

The script builds the ELF, embeds its icon and runs `build_content.py` when present. Imports are limited to the compiler runtime entries in `main/app_store/app_runtime_math.def`. The updated calculator requires firmware providing this runtime table and the optional widget styling interface. The updated Calendar, Base Converter and Unit Converter require button-text updates and disabling support. `package_check/assets.bin` and `english_reader/content.h` are generated assets. Upload and publish the ELF in the local service to install it on the device; app content stays outside firmware.

See the [App Store development guide](DEVELOPMENT.md) for capability checks, hosted timers, private data, input controls and exit cleanup. New apps must check optional fields and capabilities; existing apps remain on ABI 1.

Pomodoro requires the foundation interfaces and `XZ_APP_CAP_KEEP_AWAKE`; older firmware refuses to launch it. Update the companion firmware before use. Running inhibits automatic idle standby without keeping networking active; pause or exit releases the hold. The system still handles manual power-key standby. Snapshots are saved each minute and on key actions; power loss can lose progress since the last snapshot. Alerts are visual and foreground-only, with no background alarm.

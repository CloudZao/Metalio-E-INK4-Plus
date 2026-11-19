# Recorder

[中文](README_zn.md) | **English**

A single-recording ELF example for host-managed recording and WAV playback. Start, pause, resume, stop and save a recording. The screen shows captured seconds, the saved path and total duration. Saved audio supports play, pause, resume and stop, with a read-only progress bar. Playback uses system volume.

A new recording replaces the previous one only after saving succeeds. Preparation failures, empty takes and write failures preserve the previous file. There is no recording list, rename or delete UI. The format is fixed to 16kHz mono 16-bit PCM WAV, without recording gain, codec or sample-rate controls.

The saved file is `settings/apps/<installedName>--<appId>/recording.wav`, relative to `metalio/e-ink/`. The pending file uses `temp/app_store/<installedName>--<appId>/recording.wav.part`. Settings, usage records and audio share the existing app-private data layout; there is no new top-level directory. Updates preserve the recording; removing app data or uninstalling removes it with private data. Do not modify files through USB or another tool during recording.

Recording and playback are mutually exclusive. Record again first stops playback and waits for hardware release. Recording and recording pause inhibit automatic idle standby. Power-button standby, exit or leaving the foreground stops and saves; waking does not start a new recording. System audio has priority; preemption fails the current take while preserving the previous file. Playback follows host standby pause and display-ready resume rules. Recovery of an interrupted temporary recording after power loss is not guaranteed.

## Build and installation

Configure the toolchain using the [developer guide](../DEVELOPMENT.md), then run:

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/recorder
```

The output is `recorder.elf`. For manual installation, use the delivery filename `录音机.elf` with its matching `manifest.json` under `metalio/e-ink/apps/录音机/`. Update the manifest size and SHA256 after rebuilding, or publish through App Store. Upgrade to firmware supporting `XZ_APP_CAP_RECORDING` and WAV playback first. Missing requirements return `XZ_APP_ERR_UNSUPPORTED` at entry.

See the [recording developer guide](../DEVELOPMENT.md#recording-xz_app_cap_recording) for parameters, results and lifecycle rules.

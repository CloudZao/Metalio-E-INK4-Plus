# Imaging

[中文](README_zn.md) | **English**

An example app for ELF image display, image processing, camera capture and full refresh. The central region shows camera preview or an embedded image, with display modes and processing controls below. The interface follows the host's Chinese or English language setting.

The app provides preview and display adjustments without saving photos or recordings or managing an album. Software exposure and other adjustments process the displayed image without changing camera sensor settings.

## Usage

- Image source: The app starts with the camera selected. Tap JPEG, PNG, A2I1 or A2L8 to display an embedded image. Resources are packaged in the ELF and need no separate SD copy.
- Display mode: Choose 16 gray, 8 gray, Dither or B/W. The 397 defaults to dithering; selecting an unsupported grayscale mode returns an error and preserves the previous mode.
- Parameters: Use Previous/Next to select contrast, brightness, software exposure, shadows, highlights, Gamma, detail, rotation, center crop, horizontal mirror, vertical mirror or inversion, then use −/+ to adjust. Negative detail values soften; positive values sharpen. The interface does not expose binary threshold adjustment.
- Reset: Restore neutral processing, the full crop region and the board's default display mode. Pause/Resume controls camera preview; for embedded images, the same button reloads the current image. Refresh requests a full refresh of the current screen.

Contrast, brightness, shadows, highlights and detail default to 0%, with 25% UI steps. Software exposure defaults to 0 EV with 0.25 EV steps; Gamma defaults to 1.00 with 0.25 steps. Rotation uses 90-degree steps. Center crop retains 20–100% of source width and height in 10% steps. For mirrors and inversion, − disables and + enables. These are example UI steps; host precision and ranges are documented in [images and display effects](../README.md#images-and-display-effects).

Preview waits at least 800 ms after delivery before capturing again. Actual display speed also depends on camera capture and panel refresh. Pausing preserves the displayed frame for further adjustment. Standby, navigation and exit cancel capture; waking resumes it on demand once the page is interactive. After a capture failure, select the camera again to retry.

## Image assets

[build_content.py](build_content.py) resizes existing repository artwork while preserving aspect ratio and encodes four resources: JPEG from `docs/images/product.png`, PNG from `docs/images/product_en.png`, and both A2I1 and A2L8 from `tools/a2i1/bg_shutdown2.jpg`. These paths are relative to the repository root.

A2I1 is already binary; grayscale modes cannot restore lost tones. The script generates `assets.bin`, `image_assets.h` and `icon.pbm`. Generated files are excluded from version control.

## Build and installation

Python, Pillow and a matching ESP toolchain are required. Configure the environment using the [developer guide](../README.md#2-quick-start), then run from the repository root, replacing compiler placeholders with actual paths:

```powershell
# 470: ESP32-S31 / RISC-V
./tools/elf_app/build_app.ps1 -Compiler '<full path to riscv32-esp-elf-gcc.exe>' -AppDirectory ./tools/elf_app/imaging -Target esp32s31 -OutputDirectory ./tools/elf_app/imaging/build/470

# 397: ESP32-S3 / Xtensa
./tools/elf_app/build_app.ps1 -Compiler '<full path to xtensa-esp32s3-elf-gcc.exe>' -AppDirectory ./tools/elf_app/imaging -Target esp32s3 -OutputDirectory ./tools/elf_app/imaging/build/397
```

Outputs are `build/470/imaging.elf` and `build/397/imaging.elf`. The script generates assets and embeds the icon; it does not generate an installation manifest. For manual installation, place `imaging.elf` and its matching `manifest.json` under `metalio/e-ink/apps/imaging/` on SD. Use `installedName: imaging` with `target: esp32s31` for the 470 or `target: esp32s3` for the 397. Update size and SHA256 after rebuilding, or publish through App Store.

## Compatibility and limits

Requires ABI 1 with `XZ_APP_CAP_IMAGE`, `XZ_APP_CAP_IMAGE_TONE`, `XZ_APP_CAP_CAMERA` and `XZ_APP_CAP_FULL_REFRESH`, plus a host content area of at least 480×600. Missing capabilities or interfaces return `XZ_APP_ERR_UNSUPPORTED` at entry. An implemented interface does not imply a connected camera; embedded images remain usable without one.

The 470 supports grayscale display. The 397 has native monochrome output without 16/8-level modes, and ELF packages are not interchangeable across target architectures. USB camera use on the 397 is mutually exclusive with USB mass storage. The 8-gray mode changes image quantization without guaranteeing faster refresh. Full refresh is asynchronous and does not imply a white-flash animation or video frame rate.

Full parameters, format limits and lifecycle rules are maintained in [images and display effects](../README.md#images-and-display-effects) and [camera preview and full refresh](../README.md#camera-preview-and-full-refresh).



## Source and call flow

`imaging.c` owns UI, source selection, effect parameters and status polling. `build_content.py` generates test images and the icon; `image_assets.h` stores resource byte ranges only.

1. `app_main` checks ABI, field boundaries, capabilities and dimensions, creates image/control widgets, registers the managed timer and chooses the initial source.
2. `SelectSource` selects camera or embedded images. Images use `image_load_asset` with offset/length; success means accepted, and `Tick` polls completion and updates feedback.
3. `SelectAdjustment` chooses a parameter, `AdjustValue` updates app settings, and `ApplyEffects` invokes `image_set_effects`. Failures show a result code. Shared interfaces own parameter limits and processing order.
4. `Pause` pauses/resumes preview or reloads an embedded image. `Refresh` requests asynchronous full refresh, not physical completion.
5. The host handles standby/exit cleanup for camera, images, widgets and timers. The app does not access hardware directly or retain references after exit.

## Interface example

After capability checks and creation of valid `api` and `picture` handles, update effects as follows. This fragment is not app initialization:

```c
xz_app_image_effects_t effects = {0};
effects.struct_size = sizeof(effects);
int result = api->image_get_effects(api->host_context, picture, &effects);
if (result == XZ_APP_OK) {
    effects.rotation = 90;
    effects.brightness = 25;
    result = api->image_set_effects(api->host_context, picture, &effects);
}
```

Select JPEG, reset effects, raise brightness and rotate 90 degrees to inspect tone and orientation. Then select A2I1 to see that a binary source cannot regain grayscale. Use embedded images when the camera is unavailable and select camera again after recovery. See [shared image interfaces](../README.md#images-and-display-effects) for parameters, asynchronous results and resource limits.

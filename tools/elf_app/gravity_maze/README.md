# Gravity Maze

[中文](README_zn.md) | **English**

`gravity_maze.c` implements the standalone ELF app; `maze_engine.h` maintains maps, stars and collisions. Firmware must support drawing, foreground timing, acceleration sampling, in-app Back, white-wipe transitions, vibration and audio. The app refuses to start on hosts missing required capabilities.

## Interface

- Portrait only, designed on a 684×1216 logical canvas with a 54-pixel simulated system status bar. The ELF adapts to the actual host content area.
- Fixed monochrome appearance. Level selection, gameplay, pause, retry confirmation, completion and timeout states are interactive. A “Settings” button in the top-right opens language selection and a music toggle. Music is enabled by default and its preference is retained in app-private storage. Both settings dialogs have a top-left Back control. “Tilt to move” sits immediately to the title’s right on the same baseline. Selecting 简体中文 or English returns to Settings; there is no Cancel button. The game screen has no language control; app-private storage retains the ELF language choice. Pause, retry and results use centered dialogs with centered titles and result text. Dialog height follows text rows and button count, with widths suited to each content type; the maze remains visible and background controls are inactive. Selecting a level opens it directly. A compact top bar contains the level name, timer, Retry and Pause; the maze fills the main area without move counts, legends or instructions. Both languages use the same layout. Chinese text uses built-in sizes 25 and 30. The hint is aligned along the title’s bottom, immediately to its right; level chevrons are geometric lines. Short Back returns from a level to level selection, from Pause or Retry dialogs to gameplay, and from level selection to normal host navigation.
- All three levels and a separate Random mode are immediately selectable: First steps, The passages and Into the maze. Their grids are 7×11, 9×15 and 9×15, with limits of 60, 75 and 90 seconds.
- Fixed seeds in the code generate the maps. Shortest routes contain 50, 56 and 76 moves. Random mode uses a 9×15 grid with a 90-second limit and generates a new map on entry or confirmed retry. The home screen does not preview actual maps.

## Maze generation constraints

Both fixed and random modes first generate a connected maze excluding the goal, then attach the goal as a leaf with exactly one entrance. Every non-goal cell must be reachable from the start without crossing the goal. Branches or regions accessible only through the terminal goal are forbidden. Stars follow the shallow-branch placement rule and cannot lie beyond the goal.

## Gameplay and timing

Each maze has five stars worth 20 points each, for a maximum of 100 points. Entering a star cell collects it once; revisiting it cannot add points. Retrying resets the current collection. Stars prefer side branches one cell from the solution route, spread along it. If there are too few shallow branches, remaining stars go on the route. Collecting all five adds at most ten moves to the direct solution. Every star is reachable before entering the goal, but collecting all stars is optional. Only the original score area expands into two rows: high score, last score, best clear time and last clear time. Random mode has separate records. Scores update on completion or timeout; clear times update only on success. Returning mid-run does not save a result. The game header shows the current score, and completion and timeout dialogs show the run’s score.

- Buttons automatically use system click haptics, respecting the system vibration switch and click-feedback strength. Completion or timeout requests one 0.5-second vibration. Pause, retry confirmation and language selection do not trigger end feedback. The system vibration switch is respected. Automatic standby is deferred through the pulse; manual standby or exit may end it early.
- The black ball starts in the upper-left cell. Reach the ring in the lower-right cell to finish. Walls block movement without time penalties or move counters.
- On device, entering or leaving a level first requests a white wipe; foreground timing and sampling resume once the target frame physically completes. Pausing, confirmation overlays and hiding the page stop the countdown; resuming preserves remaining time. Timing uses a monotonic clock rather than refresh counts.
- Retrying requires confirmation, then resets the position and timer. Any level can be selected; pause and result dialogs offer Main menu, and completion offers the next level or another attempt.
- Default movement allows one cell every 220ms, without inertia, diagonal movement or crossing walls. Releasing directional input stops movement.
- The ELF saves language, scores and clear times, retaining older high-score records in app-private storage. Reopening returns to level selection without restoring an unfinished game.

## Device controls

The ELF uses the SC7A20H three-axis accelerometer, not gyroscope angular velocity. Starting or continuing establishes a holding reference from the first valid sample. Hold the device close to horizontal, wait for the reference, then tilt toward the destination. Positive X moves down; positive Y moves left. A first-order low-pass filter processes changes from the reference. The dominant axis must reach 150mg and exceed the other axis by at least 40mg; movement is limited to one cell per 220ms. Upright holding limits the available change in some directions.

Gameplay inhibits automatic idle standby while allowing power-button standby. The host suspends app sampling off-screen or in standby. Foreground timing excludes standby, then resumes the remaining time. Two seconds without a valid sample show a notice and pause the game. Exit cancels timers and releases sampling and keep-awake resources. Tilt thresholds and e-ink control feel require device validation; browser behavior does not establish hardware refresh performance.

## Background music

`music/` contains five original loops: “Courtyard at Dawn” for the main menu, “Little Paper Steps” for Level 1, “Footsteps in the Gallery” for Level 2, “Starlit Passage” for Level 3, and “Curious Crossroads” for Random mode. They use mono Ogg/Opus with 16kHz output. During the build, `build_content.py` packs them in that order into `assets.bin` and generates byte ranges in `music_assets.h`; the shared build script embeds the resources in the ELF. Playback requests only the selected region rather than loading the music package into RAM.

When music is enabled, opening the app loops the menu track. Entering a level, restarting, advancing to the next level or returning from a level to the menu restarts the corresponding track. Opening Settings, entering language selection, changing language and returning from these screens preserve the current playback position. Pause and retry confirmation suspend music; Continue resumes it. Completion, timeout and exit stop it. Back controls in Settings and Language use the same rounded corners and border style as the other buttons.

The music toggle affects only this app and does not change system volume. Music uses the system volume: mute, system settings and volume keys apply directly, without a separate music volume. Audio failure shows a notice while gameplay remains available. The shared interface and SD playback are documented in the [audio developer guide](../README.md#audio-playback-xz_app_cap_audio).

## Build and installation

Configure the toolchain as described in the [developer guide](../README.md), then run:

```powershell
./tools/elf_app/build_app.ps1 -Compiler $compiler -AppDirectory ./tools/elf_app/gravity_maze
```

The build script produces `gravity_maze.elf` with a 128×128 monochrome icon. For manual SD installation, the delivered filename is `重力迷宫.elf`; place it with `manifest.json` under `metalio/e-ink/apps/重力迷宫/`. The manifest `installedName` must be `重力迷宫`, and its size and SHA256 must match the copied ELF. Rebuilds require an updated manifest. The existing App Store upload and installation flow is also available. Updating host firmware does not replace the app on SD automatically.


### Gravity Maze: combined capabilities and compatibility failures

[Gravity Maze](README.md) checks the ABI, the boundary of `audio_get_status`, required capability bits and function pointers in `app_main` before querying audio formats, sources, sample rate and channels. Missing requirements return `XZ_APP_ERR_UNSUPPORTED` before callbacks are registered or the UI is created. Optional features may degrade gracefully, but apps must never read an absent tail field.

In the sample, `DrawMaze` combines drawing commands, `Tick` reads acceleration and uses `foreground_ms` for game timing, and `Back` handles in-app navigation. Screen changes request a white wipe; completion feedback calls `vibrate(..., 500)`. `MusicStart` loops five Ogg/Opus regions using offsets generated with the resources by `build_content.py`. Pause/resume retains the audio session, and returning from Settings preserves playback position. Private storage saves language, music preferences, scores and completion times. The host manages exit and standby lifecycles for resources, sampling and audio. Refer to each capability section above for parameters and limits.


The current entry requires a portrait content area at least 500 pixels wide and no taller than 1600. The current 397 area is too narrow; an Xtensa build does not provide layout compatibility.

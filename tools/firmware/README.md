# Firmware build and release tools

[中文](README_zn.md) | **English**

This directory owns firmware builds, merging, resource conversion and release tools. Independent app builds are covered by [ELF development](../elf_app/README.md). Run commands from the repository root after activating ESP-IDF 6.1 Python/toolchains.

## Multi-board builds

```sh
python tools/firmware/build_board.py 470
python tools/firmware/build_board.py 397
python tools/firmware/build_board.py 470-global
python tools/firmware/build_board.py 397-global
```

`build_board.py` chooses board defaults and invokes IDF, writing to `build/<profile>/`. Append `menuconfig` or `reconfigure`; use `--build-dir` to override the directory. Root discovery follows the script location; examples consistently start at the repository root. See [multi-board documentation](../../docs/hardware-adaptation.md#build-and-flash) for configuration, concurrency restrictions and flashing.

## Merging and packaging

```sh
bash tools/firmware/merge_firmware.sh --help
```

`merge_firmware.sh` targets the existing 470 root `build/`. It runs a build and merges from `flash_args`, adding `font_data` when necessary. Outputs include versioned firmware, a root firmware copy and a local archive. It does not upload and is not a general merger for isolated `build_board.py` directories. Check target and build paths; do not use it to package 397 or another model.

`release.py` merges/zips the root build or builds board variants from `config.json`. Batch mode rebuilds the root directory and modifies `sdkconfig`, unlike isolated profiles. Inspect arguments and workspace changes first; do not run concurrently. `--help` and `--list-boards` inspect configuration rather than publishing it.

## Resources and public releases

- `generate_fatfs_cp936.py`: invoked by `main/CMakeLists.txt` to create compact CP936 tables from the current SDK in the build directory. It does not modify the SDK or SD data. Inputs/outputs are command arguments; only Python's standard library is needed.
- `blank_cloudzao_endpoints.py`: clears configuration in a public release copy by modifying the endpoint source in place. It is not a normal build step. Run only for an explicitly requested public release on its intended copy and inspect the diff. It does not publish.
- `publish_gitee_release.sh`: creates/updates a remote Release and uploads firmware, an external write operation. Supply `GITEE_TOKEN`, repository, tag, body file and asset through the environment. Never store credentials in documentation or Git; execute only when publication is explicitly requested.

A build is separate from firmware publication and Store publication. See [development topics](../../docs/README.md) for source, resource and patch requirements.

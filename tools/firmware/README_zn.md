# 固件构建与发布工具

**中文** | [English](README.md)

本目录管理固件构建、合并、资源转换和发布。应用 ELF 的独立构建见[应用开发](../elf_app/README_zn.md)。所有命令从仓库根目录运行，先激活 ESP-IDF 6.1 的 Python 与工具链环境。

## 多板型构建

```sh
python tools/firmware/build_board.py 470
python tools/firmware/build_board.py 397
python tools/firmware/build_board.py 470-global
python tools/firmware/build_board.py 397-global
```

`build_board.py` 选择板型默认配置并调用 IDF，产物位于 `build/<配置>/`。可追加 `menuconfig` 或 `reconfigure`，也可用 `--build-dir` 指定目录。根目录定位基于脚本位置，不依赖调用目录；示例统一使用仓库根目录。配置、并行限制和烧录说明由[多板型文档](../../docs/hardware-adaptation_zn.md#构建与烧录)维护。

## 合并与打包

```sh
bash tools/firmware/merge_firmware.sh --help
```

`merge_firmware.sh` 面向现有 470 根构建 `build/`，会执行构建并按 `flash_args` 合并，必要时补入 `font_data`。输出带版本固件、根目录固件副本及本地归档；不上传，不是 `build_board.py` 独立目录的通用合并入口。先核对其目标和实际构建目录，不用它打包 397 或其他型号。

`release.py` 可按当前根构建合并并生成 ZIP，或读取板型 `config.json` 批量构建。批量路径会重建根构建并修改 `sdkconfig`，与隔离配置入口不同；使用前检查脚本参数和工作区，不能并行运行。`--help` 与 `--list-boards` 用于查看参数和可用配置，不代表已发布。

## 资源与公开发布

- `generate_fatfs_cp936.py`：由 `main/CMakeLists.txt` 调用，从当前 SDK 生成构建目录中的紧凑 CP936 映射，不修改 SDK 或 SD 数据。输入与输出通过命令参数提供，依赖 Python 标准库。
- `blank_cloudzao_endpoints.py`：公开发布副本的配置清理工具，会原地修改端点源文件，不用于日常编译。仅在明确执行公开发布时对待发布副本运行，并审查差异；不会自动发布。
- `publish_gitee_release.sh`：创建或更新远端 Release 并上传指定固件，属于有外部写入的发布操作。需要通过环境提供 `GITEE_TOKEN`、仓库、标签、发布正文文件和固件路径；不把凭据写入文档或版本库。只有明确要求发布时才执行。

普通构建不等于固件发布，也不等于应用商店上架。源码、资源及第三方补丁的使用条件见[开发文档](../../docs/README_zn.md)。

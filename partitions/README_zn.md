# Flash 分区布局

[English](README.md)

当前两块 16 MB 硬件共用 [16m.csv](16m.csv)。分区名称、类型、偏移和容量以该文件为唯一权威源，国内版与国际版使用相同布局。

公共 `sdkconfig.defaults` 统一选择该表，芯片和板级默认配置不再重复覆盖。已有配置的 `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` 应指向 `partitions/16m.csv`；`CONFIG_PARTITION_TABLE_FILENAME` 由 ESP-IDF 在配置时派生。

该表提供 NVS、OTA 元数据、PHY 初始化、预留模型区、双 OTA 应用槽、图片资源、字体数据和崩溃转储分区。字体包写入 `font_data`，图片资源使用 `resources`。

仅维护当前硬件支持的布局。新增布局时按 Flash 容量和用途命名，不使用板型名称或版本目录分类。

文件改名不会迁移设备数据。修改分区名称、偏移或容量时，需要单独评估已安装设备、存量数据及 OTA 兼容性。字体包和应用镜像不得超过 CSV 中定义的对应分区容量。

# Flash partition layout

[中文](README_zn.md)

The current 16 MB boards share [16m.csv](16m.csv). Partition names, types, offsets and sizes are defined only in that file. Product variants use the same layout.

The common `sdkconfig.defaults` selects this table. Target and board defaults do not override it. Existing configurations must set `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` to `partitions/16m.csv`; ESP-IDF derives `CONFIG_PARTITION_TABLE_FILENAME` during configuration.

The table provides NVS, OTA metadata, PHY initialization, a reserved model area, two OTA application slots, image resources, font data and crash dumps. Font packs are written to `font_data`; image resources use `resources`.

Only layouts supported by the current hardware are maintained. Additional layouts should use flash capacity and purpose in their filenames rather than board names or version directories.

Changing a filename does not migrate device data. Changes to partition names, offsets or sizes require a separate assessment of installed devices, stored data and OTA compatibility. Keep font packs and application images within the capacities specified in the CSV.

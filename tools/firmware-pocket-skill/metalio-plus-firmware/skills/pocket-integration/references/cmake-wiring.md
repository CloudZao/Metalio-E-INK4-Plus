# CMake 接线

推荐工程生成器 `skills/firmware-build/scripts/create_plus_project.py` 复制本模块核心及桥接到独立工程。手工接入时把 `assets/pocket-standard/` 复制到main/pocket，编译pocket_core.cc、pocket_handoff.cc、pocket_image.cc与plus_pocket_bridge.cc；元数据采用生成的声明或pocket_metadata.cc，二者只选一份。

生成pocket_build.h，定义应用ID/版本、显示名/描述、存储方式/schema、退出枚举及准确hint、独立TF需求。ESP工程project_name/version必须与内嵌app_id/version一致。出口枚举为UI=1、AI_DOUBLE=2、AI_TRIPLE=3、AI_HOLD=4、CUSTOM=5；模板默认UI，但业务可以正确实现其他标准出口。

保留.rodata_custom_desc及pocket_compact_metadata链接符号，实际产物身份头必须在app起点+0x120，不以源码声明代替偏移验证。`assets/pocket_binding.cmake` 的plus_attach_pocket可辅助已有组件追加核心和生成声明，不会代写业务钩子或HAL。

使用Plus固定工具链/分区生成同一双模式BIN，先完整镜像验证，再分开做独立和集成验收。Wi-Fi为可选组件，选用时接 `assets/wifi-standard/`，不把共享读取或候选推送当强制能力。

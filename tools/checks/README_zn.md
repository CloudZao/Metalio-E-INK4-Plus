# 依赖边界检查

**中文** | [English](README.md)

`check_dependencies.py` 检查项目包含关系，`dependency_boundaries.json` 维护现有例外及其所有者。完整规则与覆盖限制由[模块边界](../../docs/hardware-adaptation_zn.md#模块边界)维护，不在这里复制。

需要 Python 3.9+，在仓库根目录运行：

```sh
python tools/checks/check_dependencies.py --root . --compile-commands build/470/compile_commands.json
```

固件 CMake 自动调用检查并传入当前编译数据库。手工省略数据库时只自动查找已存在的 `build/470`、`build/397`；没有数据库的检查不能视为完整包含解析。`--show-unresolved` 列出未覆盖的外部或工具链头。脚本只读检查并通过退出码报告结果，不替代编译、链接或硬件验证。正式主机测试见 [tests](../tests/README_zn.md)。

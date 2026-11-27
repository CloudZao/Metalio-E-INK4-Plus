# Dependency boundary checks

[中文](README_zn.md) | **English**

`check_dependencies.py` checks includes; `dependency_boundaries.json` owns existing exceptions and their owners. See [module boundaries](../../docs/hardware-adaptation.md#module-boundaries) for the full rules and coverage limits.

Requires Python 3.9+. Run from the repository root:

```sh
python tools/checks/check_dependencies.py --root . --compile-commands build/470/compile_commands.json
```

Firmware CMake invokes it with the active compilation database. Without an explicit database, it only discovers existing `build/470` and `build/397` databases; a database-free run is not full include resolution. `--show-unresolved` lists uncovered external/toolchain headers. This read-only check reports through its exit code and does not replace compilation, linking or device validation. See [tests](../tests/README.md) for maintained host tests.

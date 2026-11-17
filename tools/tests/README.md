# Host-side regression tests

[中文](README_zn.md) | **English**

`test_lifecycle_fixes.py` extracts actual lifecycle functions from the current firmware sources and injects API success, failure and timeout results. It checks temporary Wi-Fi scan initialization cleanup, ownership and repeated teardown, plus display feeder timeout shutdown, callback removal and safe resource reuse.

Run from the repository root:

```powershell
python tools/tests/test_lifecycle_fixes.py
```

The script requires Windows MSVC Build Tools and the Windows SDK. Set `VC` and `UM` to the installed directories. Generated test sources and executables use a temporary directory that is cleaned on success or failure.

These tests do not connect to hardware and do not replace Wi-Fi traffic, DMA/interrupt timing or e-paper display verification.

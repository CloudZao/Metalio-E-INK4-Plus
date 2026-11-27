# Tool index

[中文](README_zn.md) | **English**

Tools are grouped by function. This index provides navigation; each directory owns usage, dependencies and outputs.

- [Firmware builds and releases](firmware/README.md)
- [Firmware resource generation](assets/README.md)
- [Static dependency checks](checks/README.md)
- [Host regression tests](tests/README.md)
- [ELF apps and development](elf_app/README.md)
- [E-book conversion and preview](ebook/README.md)
- [Single-size .ef reading fonts](epdfont/README.md)
- [Multi-size .fontpack packages](fontpack/README.md)
- [A2I1 image conversion](a2i1/README.md)
- [VCOM manufacturing tools](vcom/README.md)


## Other maintenance resources

- `ime/zh_CN/build_dictionary.py`: builds the Chinese input dictionary; see [library search](../docs/book-search.md) for sources and device installation.
- `ttf-fonts/`: original font files and licenses used by font tools, not generated output storage.
- `git/git-commit.sh`: Git commit helper, used only when the user explicitly requests commits.
- [Firmware skill package](firmware-pocket-skill/metalio-plus-firmware/SKILL.md): integration-environment guidance, separate from product/tool documentation.

Build trees, dependency caches and installation packages are not script sources. See [ELF development](elf_app/README.md#sources-build-outputs-and-installation-test-packages) for sources, generated files, model isolation and the undecided delivery location.

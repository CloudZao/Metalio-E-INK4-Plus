# 工具索引

**中文** | [English](README.md)

工具按功能域管理。以下入口负责导航，使用方法、依赖和产物由对应目录维护。

- [固件构建与发布](firmware/README_zn.md)
- [固件资源生成](assets/README_zn.md)
- [静态依赖检查](checks/README_zn.md)
- [主机回归测试](tests/README_zn.md)
- [ELF 应用与开发](elf_app/README_zn.md)
- [电子书转换与预览](ebook/README_zn.md)
- [单字号 .ef 阅读字体](epdfont/README.md)
- [多字号 .fontpack 字库](fontpack/README.md)
- [A2I1 图片转换](a2i1/README.md)
- [VCOM 产测](vcom/README_zn.md)


## 其他维护资源

- `ime/zh_CN/build_dictionary.py`：构建中文输入词库，来源与设备安装见[书架搜索](../docs/book-search_zn.md)。
- `ttf-fonts/`：字体原文件和授权，由字体工具引用；不作为生成产物目录。
- `git/git-commit.sh`：按脚本参数执行 Git 提交的辅助入口，只有用户明确要求提交时使用。
- [固件技能包](firmware-pocket-skill/metalio-plus-firmware/SKILL.md)：面向集成环境的操作指导，不能替代产品使用与工具说明。

构建目录、依赖缓存和安装包不是脚本源码。ELF 的源码、生成产物、型号隔离和待定交付位置见[应用开发](elf_app/README_zn.md#源码构建产物与安装测试包)。

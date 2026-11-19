"""Pack the five bundled music tracks and generate their byte ranges."""
from pathlib import Path

root = Path(__file__).resolve().parent
tracks = ("menu", "level-1", "level-2", "level-3", "random")
offsets = []
lengths = []
with (root / "assets.bin").open("wb") as output:
    for name in tracks:
        data = (root / "music" / (name + ".ogg")).read_bytes()
        if not data.startswith(b"OggS"):
            raise ValueError("Invalid bundled music: " + name)
        offsets.append(output.tell())
        lengths.append(len(data))
        output.write(data)
header = "#pragma once\n#include <stdint.h>\n\n"
header += "// 构建生成，顺序为主菜单、第一关、第二关、第三关、随机模式。\n"
header += "static const uint32_t music_offsets[5] = {" + ", ".join(map(str, offsets)) + "};\n"
header += "static const uint32_t music_lengths[5] = {" + ", ".join(map(str, lengths)) + "};\n"
header += "#define MUSIC_ASSETS_SIZE " + str(sum(lengths)) + "u\n"
(root / "music_assets.h").write_text(header, encoding="utf-8")

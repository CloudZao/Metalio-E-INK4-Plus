"""压缩中英文文件传输页面，标识图片由设备共享提供，仅使用 Python 标准库。"""

import json
import re
import zlib
from pathlib import Path
import sys


def pack(source: Path, output: Path, translations: Path | None = None) -> None:
    page = source.read_text(encoding="utf-8")
    if translations is not None:
        strings = json.loads(translations.read_text(encoding="utf-8"))
        pattern = "|".join(re.escape(key) for key in sorted(strings, key=len, reverse=True))
        page = re.sub(pattern, lambda match: strings[match.group()], page)
        page = page.replace('lang="zh-CN"', 'lang="en-US"', 1)
        visible_source = re.sub(r"(?m)^\s*//.*$", "", page)
        if re.search(r"[\u3400-\u9fff]", visible_source):
            raise ValueError("Untranslated Chinese text in English file manager page")
    page = page.replace('src="cloudzao-logo.png"', 'src="/cloudzao-logo.png"', 1)
    packed = zlib.compress(page.encode("utf-8"), 9, wbits=31)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(packed)


if __name__ == "__main__":
    pack(
        Path(sys.argv[1]),
        Path(sys.argv[2]),
        Path(sys.argv[3]) if len(sys.argv) > 3 else None,
    )

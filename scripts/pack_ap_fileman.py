"""压缩中英文文件传输页面并内嵌标识图片，仅使用 Python 标准库。"""

import base64
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
    logo = base64.b64encode((source.parent / "cloudzao-logo.png").read_bytes()).decode("ascii")
    before, after = page.split('src="cloudzao-logo.png"', 1)
    image = 'src="data:image/png;base64,' + logo + '"'
    # 图片与页面分别结束 DEFLATE 块，避免大段 base64 干扰页面的压缩编码表。
    compressor = zlib.compressobj(9, zlib.DEFLATED, 31)
    packed = compressor.compress(before.encode("utf-8")) + compressor.flush(zlib.Z_BLOCK)
    packed += compressor.compress(image.encode("ascii")) + compressor.flush(zlib.Z_BLOCK)
    packed += compressor.compress(after.encode("utf-8")) + compressor.flush()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(packed)


if __name__ == "__main__":
    pack(
        Path(sys.argv[1]),
        Path(sys.argv[2]),
        Path(sys.argv[3]) if len(sys.argv) > 3 else None,
    )

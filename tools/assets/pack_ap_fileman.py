"""将文件传输页面打包为公共样式、布局和脚本，以及中英文语言入口。"""

import json
import re
import sys
import zlib
from pathlib import Path


def pack(source: Path, output: Path, translations: Path) -> None:
    page = source.read_text(encoding="utf-8")
    strings = json.loads(translations.read_text(encoding="utf-8"))
    keys = sorted(strings, key=len, reverse=True)
    pattern = re.compile("|".join(re.escape(key) for key in keys))
    indexes = {key: index for index, key in enumerate(keys)}

    def localized_literal(literal: str) -> str:
        quote = literal[0]
        content = literal[1:-1]
        parts = []
        start = 0
        for match in pattern.finditer(content):
            parts.append(quote + content[start:match.start()] + quote)
            parts.append(f"FILEMAN_TEXT[{indexes[match.group()]}]")
            start = match.end()
        if not parts:
            return literal
        parts.append(quote + content[start:] + quote)
        return "(" + "+".join(parts) + ")"

    style = re.search(r"<style>(.*?)</style>", page, re.S).group(1)
    body, script = re.search(r"<body>(.*?)<script>(.*?)</script>", page, re.S).groups()
    body = body.replace('src="cloudzao-logo.png"', 'src="/cloudzao-logo.png"', 1)
    title = re.search(r"<title>(.*?)</title>", page, re.S).group(1)
    # 当前页面只使用单双引号字符串；新增模板字符串时须同步扩展打包规则。
    if "`" in script:
        raise ValueError("Template literals are not supported by the file manager packer")
    tokens = re.compile(
        r'//[^\n]*|/\*.*?\*/'
        r'|/(?:\[(?:\\.|[^\]\\])*\]|\\.)[^\n]*?/[gimuy]*'
        r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        re.S,
    )

    def localize_token(match: re.Match) -> str:
        token = match.group()
        if token[0] in "\"'":
            return localized_literal(token)
        if token.startswith(("//", "/*")):
            return "\n" if "\n" in token else " "
        return token

    script = tokens.sub(localize_token, script)
    common = (
        "document.title=" + localized_literal(json.dumps(title, ensure_ascii=False)) + ";\n"
        "document.head.insertAdjacentHTML('beforeend',"
        + json.dumps("<style>" + style + "</style>", ensure_ascii=False) + ");\n"
        "document.body.insertAdjacentHTML('afterbegin',"
        + localized_literal(json.dumps(body, ensure_ascii=False)) + ");\n" + script
    )
    if re.search(r"[\u3400-\u9fff]", common):
        raise ValueError("Untranslated Chinese text in shared file manager resources")
    output.mkdir(parents=True, exist_ok=True)

    def write(name: str, content: str) -> None:
        (output / name).write_bytes(zlib.compress(content.encode("utf-8"), 9, wbits=31))

    write("ap_fileman.js.gz", common)
    for language, values, name in (
        ("zh-CN", keys, "ap_fileman.html.gz"),
        ("en-US", [strings[key] for key in keys], "ap_fileman_en.html.gz"),
    ):
        dictionary = json.dumps(values, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")
        write(name, '<!DOCTYPE html><html lang="' + language + '"><head>'
              '<meta charset="utf-8"><meta name="viewport" '
              'content="width=device-width,initial-scale=1,viewport-fit=cover">'
              '</head><body><script>const FILEMAN_TEXT=' + dictionary + ';</script>'
              '<script src="/fileman.js"></script></body></html>')


if __name__ == "__main__":
    pack(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))

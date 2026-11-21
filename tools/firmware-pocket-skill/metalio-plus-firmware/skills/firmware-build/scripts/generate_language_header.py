#!/usr/bin/env python3
"""Restore the missing declarations from the fixed official Plus dictionary.

This is authored development-Skill preparation, separate from the raw mirror. The official
lang_home.cc remains the only dictionary and implementation. No language
tables, translations, sound data, or substitute implementations are emitted.
"""

import argparse
import hashlib
import json
import re
from pathlib import Path


SOURCE_COMMIT = "ad8edf4150a688737c0d7c00cae4413079b262ef"
SOURCE_RELATIVE_PATH = "main/assets/lang_home.cc"
SOURCE_SHA256 = "cfd7125cab6c6d580b56925205cfc2337e811c91772f89ecb55a6def442b6c8b"
EXPECTED_SYMBOL_COUNT = 742
HEADER_RELATIVE_PATH = "main/assets/lang_config.h"
STRING_LITERAL = r'"(?:[^"\\\r\n]|\\.)*"'
ENTRY = re.compile(
    r"\s*X\(\s*([A-Z][A-Z0-9_]*)\s*,\s*"
    + STRING_LITERAL
    + r"\s*,\s*"
    + STRING_LITERAL
    + r"\s*\)\s*"
)


def dictionary_symbols(source_text):
    """Consume every macro entry, rejecting unknown syntax and duplicate keys."""
    lines = source_text.splitlines()
    starts = [
        index
        for index, line in enumerate(lines)
        if re.fullmatch(r"#define\s+LANG_STRING_LIST\(X\)\s*\\\s*", line)
    ]
    if len(starts) != 1:
        raise ValueError("Expected exactly one LANG_STRING_LIST(X) macro")
    chunks = []
    for line in lines[starts[0] + 1 :]:
        stripped = line.rstrip()
        continued = stripped.endswith("\\")
        chunks.append(stripped[:-1] if continued else stripped)
        if not continued:
            break
    else:
        raise ValueError("Unterminated LANG_STRING_LIST(X) macro")

    body = "\n".join(chunks)
    position = 0
    symbols = []
    while position < len(body):
        match = ENTRY.match(body, position)
        if match is None:
            raise ValueError(
                "Unsupported dictionary macro syntax at offset "
                + str(position)
                + ": "
                + repr(body[position : position + 100])
            )
        symbols.append(match.group(1))
        position = match.end()
    if len(symbols) != EXPECTED_SYMBOL_COUNT or len(set(symbols)) != len(symbols):
        raise ValueError("Expected exactly 742 distinct official dictionary symbols")

    definitions = (
        r"const char\* CODE = LANG_DEFAULT_CODE;",
        r"const char\* Current\(\)\s*\{",
        r"bool SetLanguage\(const char\* code, bool persist\)\s*\{",
        r"void InitFromNvs\(\)\s*\{",
    )
    for definition in definitions:
        if len(re.findall(definition, source_text)) != 1:
            raise ValueError("Official language API definition mismatch: " + definition)
    return symbols


def header_bytes(symbols, language):
    guard = language.lower().replace("-", "_")
    declarations = "\n".join("extern const char* " + name + ";" for name in symbols)
    return (
        "// Generated declarations from the fixed official lang_home.cc dictionary.\n"
        "// Authored development Skill preparation; do not edit this generated file.\n"
        f"// Source commit: {SOURCE_COMMIT}\n"
        f"// Source SHA256: {SOURCE_SHA256}\n"
        "#pragma once\n\n"
        f"#ifndef {guard}\n#define {guard}\n#endif\n\n"
        "#ifndef LANG_DEFAULT_CODE\n"
        f'#define LANG_DEFAULT_CODE "{language}"\n'
        "#endif\n\n"
        "namespace Lang {\n"
        "extern const char* CODE;\n"
        "bool SetLanguage(const char* code, bool persist = true);\n"
        "const char* Current();\n"
        "void InitFromNvs();\n\n"
        "namespace Strings {\n"
        + declarations
        + "\n}  // namespace Strings\n"
        "}  // namespace Lang\n"
    ).encode("utf-8")


def generate(project, language):
    project = project.resolve(strict=True)
    source = project / SOURCE_RELATIVE_PATH
    raw_source = source.read_bytes()
    source_digest = hashlib.sha256(raw_source).hexdigest()
    if source_digest != SOURCE_SHA256:
        raise ValueError(
            "Official language source SHA256 mismatch; refusing another revision: "
            + source_digest
        )
    symbols = dictionary_symbols(raw_source.decode("utf-8"))
    result = header_bytes(symbols, language)
    output = project / HEADER_RELATIVE_PATH
    status = "created"
    try:
        with output.open("xb") as stream:
            stream.write(result)
    except FileExistsError:
        if output.read_bytes() != result:
            raise ValueError("Existing language header differs; refusing to overwrite: " + str(output))
        status = "unchanged"
    if output.read_bytes() != result:
        raise RuntimeError("Generated header readback mismatch")
    return {
        "status": status,
        "source_commit": SOURCE_COMMIT,
        "source_path": SOURCE_RELATIVE_PATH,
        "source_sha256": source_digest,
        "output": str(output),
        "output_sha256": hashlib.sha256(result).hexdigest(),
        "symbol_count": len(symbols),
        "ordered_symbol_names_sha256": hashlib.sha256(
            ("\n".join(symbols) + "\n").encode("ascii")
        ).hexdigest(),
        "default_language": language,
        "default_language_policy": "Development Skill preparation default; upstream sdkconfig has no language selection",
        "dictionary_or_implementation_modified": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True, help="Fresh Plus project directory")
    parser.add_argument("--language", choices=("zh-CN", "en-US"), default="zh-CN")
    args = parser.parse_args()
    try:
        report = generate(args.project, args.language)
    except (OSError, UnicodeError, ValueError, RuntimeError) as error:
        parser.exit(1, "Language preparation failed: " + str(error) + "\n")
    print(json.dumps(report, ensure_ascii=True, indent=2))


if __name__ == "__main__":
    main()

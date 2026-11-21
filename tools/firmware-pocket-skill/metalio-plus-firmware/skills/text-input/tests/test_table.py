"""Verify shipped data and actual table-tool outputs; needs only Python stdlib."""
from pathlib import Path
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
import tempfile

MODULE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("table_tool", MODULE / "scripts/table_tool.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
rows = tool.load_table(MODULE / "assets/pinyin-candidates.tsv")
report = json.loads((MODULE / "generation/generation-report.json").read_text("utf-8"))
weights_text = (MODULE / "generation/frequency-weights.tsv").read_bytes()
assert hashlib.sha256(weights_text).hexdigest() == report["weights_sha256"]
weights = {c: (None if n == "missing" else int(n)) for c, n in
           (line.split("\t") for line in weights_text.decode().splitlines()[1:])}
coverage = set((MODULE / "generation/compat-characters.txt").read_text("utf-8").strip())
assert coverage == set(weights) == set("".join(chars for _, chars in rows))
assert len(coverage) == 4262 and sum(n is None for n in weights.values()) == 11
for key, chars in rows:
    expected = sorted(chars, key=lambda c: (0, -weights[c], ord(c)) if weights[c] is not None else (1, 0, ord(c)))
    assert list(chars) == expected, key
for key, chars in report["samples"].items():
    assert dict(rows)[key] == chars
for c, readings in {"重": ("chong", "zhong"), "行": ("hang", "xing"), "长": ("chang", "zhang"), "乐": ("le", "yue")}.items():
    assert all(c in dict(rows)[s] for s in readings)
assert not set("以已厕食") & set(dict(rows)["si"])
with tempfile.TemporaryDirectory(prefix="plus-ime-tests-") as temp:
    work = Path(temp)
    header = work / "pinyin_table.h"
    result = subprocess.run([sys.executable, str(MODULE / "scripts/table_tool.py"), "--header", str(header),
                             "--notices-dir", str(work / "notices")], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    matches = re.findall(r'\{"([a-z]+)", "([^\"]+)"\}', header.read_text("ascii"))
    decoded = [(key, bytes(int(b, 16) for b in re.findall(r"\\x([0-9a-f]{2})", text)).decode("utf-8")) for key, text in matches]
    assert decoded == rows
    assert (work / "notices/CC-BY-SA-4.0.txt").read_bytes() == (MODULE / "licenses/CC-BY-SA-4.0.txt").read_bytes()
    assert (work / "notices/pinyin-candidates.tsv").read_bytes() == (MODULE / "assets/pinyin-candidates.tsv").read_bytes()
    (work / "notices/help-notice.txt").write_text("unrelated existing file", encoding="utf-8")
    result = subprocess.run([sys.executable, str(MODULE / "scripts/table_tool.py"), "--notices-dir", str(work / "notices")], capture_output=True)
    assert result.returncode != 0 and (work / "notices/help-notice.txt").read_text("utf-8") == "unrelated existing file"
    corrupted = work / "changed.tsv"
    corrupted.write_bytes((MODULE / "assets/pinyin-candidates.tsv").read_bytes().replace(b"ai\t", b"az\t", 1))
    try:
        tool.load_table(corrupted)
    except ValueError:
        pass
    else:
        raise AssertionError("Unknown changed table accepted")
print(json.dumps({"status": "PASS", "rows": len(rows), "characters": len(coverage), "candidate_occurrences": 4415,
                  "missing_weights_preserved": 11, "complete_header_equivalence": True,
                  "notice_export_and_conflict": True, "changed_input_rejected": True}))

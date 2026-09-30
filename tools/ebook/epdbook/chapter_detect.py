"""TXT / 启发式章节识别。

与固件 `main/reader/txt_chapter.cc` 对齐并扩展（参考网文 TXT→EPUB 常见规则）：
- 第N章/回/节/集/话/篇
- 序章/楔子/番外/前言/后记/尾声/终章…
- Chapter / Part / Volume
- 第N卷/部、卷N
- 纯数字短章名（如「1清和宫上」，对齐固件）
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Iterable, List, Optional, Pattern, Sequence, Tuple

# 中文/阿拉伯数字（与固件 IsCnNumeral 一致）
_CN_NUM = r"[0-9一二三四五六七八九十百千万两零〇]"
_NUM = rf"{_CN_NUM}+"

# 章量词（不含「卷/部」——卷部单独匹配，避免「第一部小说」误伤过宽）
_CH_UNIT = r"[章节回讲篇话集]"

# 与固件 + 网文工具常见规则。整行 ^$，限制副标题长度。
_DEFAULT_PATTERNS: List[str] = [
    # 【第一章 标题】/ 第一章 / 第 12 回 xxx
    rf"^[【\(\（\[『「]*第\s*{_NUM}\s*{_CH_UNIT}[：:\s\-—–]?[^\n]{{0,40}}[】\)\）\]』」]*$",
    # 序章 / 楔子 / 番外 / 后记…（关键词后须结束或分隔/闭括号，避免「前言文字」误伤）
    r"^[【\(\（\[『「]*(?:序章|序言|楔子|引子|引言|前言|绪论|后记|跋|尾声|终章|番外篇|番外|外传|附录|导读|内容简介|序)"
    r"(?=$|[：:\s\-—–]|[】\)\）\]』」])[】\)\）\]』」]*(?:[：:\s\-—–]?[^\n]{0,36})?$",
    # Chapter 1 / CHAPTER XII / Part 2
    r"^(?:Chapter|CHAPTER|chapter|Part|PART|part|Section|SECTION)\s+[0-9IVXLCDM]{1,8}"
    r"(?:[：:.\-—–\s].{0,40})?$",
    # 第N卷 / 第N部 / 卷一 / 卷 3
    rf"^[【\(\（\[『「]*第\s*{_NUM}\s*[卷部][：:\s\-—–]?[^\n]{{0,40}}[】\)\）\]』」]*$",
    rf"^[【\(\（\[『「]*卷\s*{_NUM}[：:\s\-—–]?[^\n]{{0,40}}[】\)\）\]』」]*$",
    # Volume / Book / Act
    r"^(?:Volume|VOLUME|Book|BOOK|Act|ACT)\s+[0-9IVXLCDM]{1,8}(?:[：:.\-—–\s].{0,40})?$",
    # 固件：纯数字短章名「11」或「11清和宫上」（排除 1. / 1、）
    r"^[0-9]{1,4}(?![.．、．])\S{0,40}$",
]

_BLANK_SPLIT = re.compile(r"\n\s*\n\s*\n+")  # 连续 ≥2 空行弱切分


@dataclass
class ChapterSlice:
    title: str
    start_line: int  # inclusive
    # 正文从下一行开始；title 行本身可作为标题


def compile_patterns(custom: Optional[Sequence[str]] = None) -> List[Pattern[str]]:
    src = list(custom) if custom else list(_DEFAULT_PATTERNS)
    return [re.compile(p, re.UNICODE) for p in src]


def normalize_title_line(line: str) -> str:
    """去首尾空白与常见装饰符，便于匹配与展示。"""
    s = (line or "").strip()
    # 去掉行首 markdown / 装饰
    s = re.sub(r"^[#*>\s　\t]+", "", s)
    s = s.strip()
    # 去掉成对包裹
    for a, b in (("【", "】"), ("「", "」"), ("『", "』"), ("《", "》"), ("[", "]"), ("(", ")"), ("（", "）")):
        if s.startswith(a) and s.endswith(b) and len(s) > 2:
            s = s[len(a) : -len(b)].strip()
    return s


def is_txt_chapter_title_line(
    line: str,
    patterns: Optional[Sequence[Pattern[str]]] = None,
    *,
    max_len: int = 60,
) -> bool:
    """判断一行是否像章节标题（对齐固件 IsTxtChapterTitleLine，规则更全）。"""
    s = normalize_title_line(line)
    if len(s) < 1 or len(s) > max_len:
        return False
    # 过长对话/叙述：含句号逗号且很长时倾向否（数字短章名除外）
    pats = list(patterns) if patterns is not None else compile_patterns()
    return any(p.match(s) for p in pats)


def _is_title_line(line: str, patterns: Sequence[Pattern[str]], max_len: int = 60) -> bool:
    return is_txt_chapter_title_line(line, patterns, max_len=max_len)


def _filter_dense_hits(
    hits: List[int],
    lines: Sequence[str],
    *,
    min_chapter_chars: int,
) -> List[int]:
    """丢掉紧挨着的伪标题（中间几乎无正文）。正常短章（隔几行）保留。"""
    if not hits:
        return []
    filtered = [hits[0]]
    for h in hits[1:]:
        prev = filtered[-1]
        mid = "".join(lines[prev + 1 : h])
        gap = h - prev
        # 仅当标题几乎连在一起且中间无正文时跳过（目录里连写两行标题）
        if gap <= 2 and not mid.strip():
            continue
        filtered.append(h)
    return filtered


def _drop_front_toc(
    hits: List[int],
    lines: Sequence[str],
    *,
    min_run: int = 5,
    max_body: int = 50,
) -> List[int]:
    """丢弃文首「假目录」：连续多条命中且中间几乎无正文。"""
    if len(hits) < min_run:
        return hits
    run_end = 0
    for i in range(1, len(hits)):
        prev, cur = hits[i - 1], hits[i]
        body = "".join(lines[prev + 1 : cur]).strip()
        if len(body) < max_body and cur - prev < 6:
            run_end = i
        else:
            break
    # run_end 是最后一条仍属假目录的下标
    if run_end + 1 >= min_run:
        return hits[run_end + 1 :]
    return hits


def split_lines_to_chapters(
    lines: Sequence[str],
    *,
    patterns: Optional[Sequence[Pattern[str]]] = None,
    custom_regex: Optional[Sequence[str]] = None,
    min_chapter_chars: int = 80,
    skip_front_toc: bool = True,
) -> List[Tuple[str, List[str]]]:
    """
    返回 [(title, body_lines), ...]。
    若几乎无标题命中，整书作为一章「全文」。
    """
    pats = list(patterns) if patterns else compile_patterns(custom_regex)
    hits: List[int] = []
    for i, line in enumerate(lines):
        # 允许少量缩进（网文拷贝常带空格）；缩进过多视为正文
        raw = line or ""
        indent = 0
        for ch in raw:
            if ch in " \t\u3000":
                indent += 1
            else:
                break
        if indent > 4:
            continue
        if _is_title_line(line, pats):
            hits.append(i)

    if skip_front_toc:
        hits = _drop_front_toc(hits, lines)

    if len(hits) < 2:
        # 尝试空白行弱切分
        text = "\n".join(lines)
        parts = _BLANK_SPLIT.split(text)
        if len(parts) >= 3:
            chapters: List[Tuple[str, List[str]]] = []
            for part in parts:
                body = part.strip("\n").split("\n")
                if not any(x.strip() for x in body):
                    continue
                chapters.append((f"第{len(chapters)+1}部分", body))
            if len(chapters) >= 2:
                return chapters
        return [("全文", list(lines))]

    filtered = _filter_dense_hits(hits, lines, min_chapter_chars=min_chapter_chars)
    if len(filtered) < 2:
        return [("全文", list(lines))]

    chapters: List[Tuple[str, List[str]]] = []
    if filtered[0] > 0:
        preface = lines[: filtered[0]]
        if any(x.strip() for x in preface):
            chapters.append(("前言", preface))

    for i, start in enumerate(filtered):
        end = filtered[i + 1] if i + 1 < len(filtered) else len(lines)
        title = normalize_title_line(lines[start]) or f"第{i+1}章"
        body = lines[start + 1 : end]
        chapters.append((title, body))
    return chapters


_PARA_LEADING_WS = " \t\u3000\u00a0"


def _leading_ws_len(line: str) -> int:
    n = 0
    for ch in line:
        if ch in _PARA_LEADING_WS:
            n += 1
        else:
            break
    return n


def _strip_para_leading_ws(line: str) -> str:
    return (line or "").replace("\r\n", "\n").replace("\r", "\n").lstrip(_PARA_LEADING_WS)


def lines_to_paragraphs(lines: Sequence[str]) -> List[str]:
    """空行分段；段首空白在转换时剥离（阅读端统一 pad 两字）。

    - **空行**：结束当前段；缓冲内多行用 ``\\n`` 连接（段内软换行）
    - **连续非空行且该行源文有前导空白**（网文 ``\\u3000``/空格）：各自独立成段
    - **连续非空行且无源文前导空白**：合并为一段，行间 ``\\n`` 软换行
    - 不再把 ``U+3000`` 保留进正文（避免与阅读端 pad 叠加）
    """
    paras: List[str] = []
    buf: List[str] = []
    for line in lines:
        raw = (line or "").replace("\r\n", "\n").replace("\r", "\n")
        if not raw.strip():
            if buf:
                paras.append("\n".join(buf))
                buf = []
            continue
        had_leading = _leading_ws_len(raw) > 0
        cleaned = raw.lstrip(_PARA_LEADING_WS)
        if buf:
            if had_leading:
                for prev in buf:
                    paras.append(prev)
                buf = [cleaned]
            else:
                buf.append(cleaned)
        else:
            buf = [cleaned]
    if buf:
        if len(buf) == 1:
            paras.append(buf[0])
        else:
            paras.append("\n".join(buf))
    return paras


def pdf_font_size_chapter_breaks(
    spans: Iterable[Tuple[float, str]],
    *,
    size_ratio: float = 1.35,
    min_size: float = 0.0,
) -> List[int]:
    """
    PDF 启发式：根据 span 字号突变标出疑似标题在 flat 段落序列中的下标。
    spans: (font_size, text) 按阅读顺序。
    返回疑似章节标题的索引列表。
    """
    items = [(float(sz), (t or "").strip()) for sz, t in spans if (t or "").strip()]
    if not items:
        return []
    body_sizes = sorted(sz for sz, _ in items)
    median = body_sizes[len(body_sizes) // 2]
    threshold = max(min_size, median * size_ratio)
    hits: List[int] = []
    for i, (sz, text) in enumerate(items):
        if sz >= threshold and len(text) <= 60:
            hits.append(i)
    return hits

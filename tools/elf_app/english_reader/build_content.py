"""Build offline reading pages from the bundled original text and vocabulary."""
from pathlib import Path
import hashlib
import json
import re
import textwrap
import unicodedata

root = Path(__file__).resolve().parent
original = (root / 'book.txt').read_text(encoding='utf-8-sig')
text = original.replace('\r\n', '\n').replace('’', "'").replace('‘', "'").replace('“', '"').replace('”', '"').replace('—', ' -- ').replace('–', '-').replace('…', '...').replace('£', 'GBP ')
text = unicodedata.normalize('NFKD', text).encode('ascii', 'ignore').decode()
matches = list(re.finditer(r'^CHAPTER ([IVX]+)\.\s*$', text, re.M))
assert len(matches) == 12
end = text.index('*** END OF THE PROJECT GUTENBERG EBOOK')
chapters = [('Source and edition', text[:matches[0].start()])]
for i, match in enumerate(matches):
    stop = matches[i+1].start() if i+1 < len(matches) else end
    body = text[match.end():stop].strip()
    title = body.split('\n', 1)[0].strip()
    chapters.append((title, body))
chapters.append(('Project Gutenberg License', text[end:]))
pages = []
index = []
for title, body in chapters:
    lines = []
    for paragraph in re.split(r'\n\s*\n', body.strip()):
        plain = ' '.join(paragraph.split())
        if plain:
            lines.extend(textwrap.wrap(plain, width=42, break_long_words=True, break_on_hyphens=False))
            lines.append('')
    start = len(pages)
    for at in range(0, len(lines), 12):
        pages.append('\n'.join(lines[at:at+12]).rstrip())
    index.append((title, start, len(pages)-start))
words = []
seen = set()
for line in (root/'words.tsv').read_text(encoding='utf-8').splitlines():
    en, zh = line.split('\t')
    if en not in seen:
        seen.add(en)
        words.append((en, zh))
assert len(words) > 150
quote = lambda s: json.dumps(s, ensure_ascii=False)
out = ['typedef struct {\n    const char* title; // 章节名称\n    unsigned first; // 首个阅读页索引\n    unsigned count; // 章节页数\n} Chapter;',
       'typedef struct {\n    const char* en; // 英文词条\n    const char* zh; // 中文释义\n} Word;',
       'static const char* const pages[] = {']
out.extend(quote(page)+',' for page in pages)
out.extend(['};', 'static const Chapter chapters[] = {'])
out.extend('{'+quote(title)+f',{first},{count}'+'},' for title,first,count in index)
out.extend(['};','static const Word words[] = {'])
out.extend('{'+quote(en)+','+quote(zh)+'},' for en,zh in words)
out.extend(['};',f'#define PAGE_COUNT {len(pages)}u',f'#define CHAPTER_COUNT {len(index)}u',f'#define WORD_COUNT {len(words)}u'])
(root/'content.h').write_text('\n'.join(out)+'\n',encoding='utf-8')
print(f'Book: {len(pages)} pages, {len(index)} sections, {len(words)} words; source SHA256 {hashlib.sha256((root/"book.txt").read_bytes()).hexdigest()}')

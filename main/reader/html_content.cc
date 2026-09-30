#include "html_content.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace reader {
namespace {

void AppendText(std::string& buf, char c) {
    if (c == '\r') {
        return;
    }
    if (c == '\n' || c == '\t') {
        c = ' ';
    }
    if (c == ' ' && (buf.empty() || buf.back() == ' ' || buf.back() == '\n')) {
        return;
    }
    buf.push_back(c);
}

void FlushParagraph(std::string& buf, std::vector<ContentBlock>& out) {
    while (!buf.empty() && (buf.back() == ' ' || buf.back() == '\n')) {
        buf.pop_back();
    }
    size_t start = 0;
    while (start < buf.size() && (buf[start] == ' ' || buf[start] == '\n')) {
        ++start;
    }
    if (start >= buf.size()) {
        buf.clear();
        return;
    }
    ContentBlock b;
    b.kind = ContentKind::kText;
    b.text = buf.substr(start);
    out.push_back(std::move(b));
    buf.clear();
}

std::string ToLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool StartsWithIgnoreCase(const char* p, const char* lit) {
    while (*lit) {
        if (std::tolower(static_cast<unsigned char>(*p)) !=
            std::tolower(static_cast<unsigned char>(*lit))) {
            return false;
        }
        ++p;
        ++lit;
    }
    return true;
}

std::string ReadAttrValue(const char* tag_start, const char* tag_end, const char* attr) {
    const size_t alen = std::strlen(attr);
    for (const char* q = tag_start; q + alen + 2 < tag_end; ++q) {
        if (!StartsWithIgnoreCase(q, attr)) {
            continue;
        }
        if (q > tag_start && (std::isalnum(static_cast<unsigned char>(q[-1])) || q[-1] == '_' ||
                              q[-1] == '-')) {
            continue;
        }
        q += alen;
        while (q < tag_end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) {
            ++q;
        }
        if (q >= tag_end || *q != '=') {
            continue;
        }
        ++q;
        while (q < tag_end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) {
            ++q;
        }
        if (q >= tag_end) {
            return {};
        }
        char quote = 0;
        if (*q == '"' || *q == '\'') {
            quote = *q++;
        }
        const char* p = q;
        std::string val;
        while (p < tag_end) {
            if (quote) {
                if (*p == quote) {
                    break;
                }
            } else if (*p == ' ' || *p == '\t' || *p == '>' || *p == '/') {
                break;
            }
            val.push_back(*p++);
        }
        return val;
    }
    return {};
}

bool IsBlockEndTag(const std::string& name) {
    return name == "p" || name == "div" || name == "br" || name == "li" || name == "tr" ||
           name == "h1" || name == "h2" || name == "h3" || name == "h4" || name == "h5" ||
           name == "h6" || name == "section" || name == "article" || name == "blockquote";
}

}  // namespace

std::string DecodeHtmlEntities(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '&') {
            out.push_back(in[i]);
            continue;
        }
        const size_t semi = in.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 16) {
            out.push_back(in[i]);
            continue;
        }
        const std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent == "amp") {
            out.push_back('&');
        } else if (ent == "lt") {
            out.push_back('<');
        } else if (ent == "gt") {
            out.push_back('>');
        } else if (ent == "quot") {
            out.push_back('"');
        } else if (ent == "apos") {
            out.push_back('\'');
        } else if (ent == "nbsp") {
            out.push_back(' ');
        } else if (!ent.empty() && ent[0] == '#') {
            int code = 0;
            if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
                code = static_cast<int>(std::strtol(ent.c_str() + 2, nullptr, 16));
            } else {
                code = static_cast<int>(std::strtol(ent.c_str() + 1, nullptr, 10));
            }
            if (code > 0 && code < 128) {
                out.push_back(static_cast<char>(code));
            } else if (code >= 128 && code <= 0x7FF) {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else if (code > 0x7FF && code <= 0xFFFF) {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else {
                out.push_back('?');
            }
        } else {
            out.push_back('&');
            out.append(ent);
            out.push_back(';');
            i = semi;
            continue;
        }
        i = semi;
    }
    return out;
}

std::string NormalizeZipPath(const std::string& base_dir, const std::string& href) {
    std::string raw = href;
    const size_t hash = raw.find('#');
    if (hash != std::string::npos) {
        raw = raw.substr(0, hash);
    }
    if (raw.empty()) {
        return {};
    }
    // file:// 或绝对 http 忽略
    if (raw.find("://") != std::string::npos) {
        return {};
    }
    while (!raw.empty() && raw[0] == '/') {
        raw.erase(raw.begin());
    }

    std::string path;
    if (raw.find('/') == 0) {
        path = raw;
    } else {
        path = base_dir;
        if (!path.empty() && path.back() != '/') {
            path.push_back('/');
        }
        path += raw;
    }

    // 折叠 ./ 与 ../
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) {
            j = path.size();
        }
        const std::string seg = path.substr(i, j - i);
        i = j + 1;
        if (seg.empty() || seg == ".") {
            continue;
        }
        if (seg == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
            continue;
        }
        parts.push_back(seg);
    }
    std::string out;
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k) {
            out.push_back('/');
        }
        out += parts[k];
    }
    return out;
}

void HtmlToBlocks(const std::string& html, const std::string& base_dir, std::vector<ContentBlock>& out) {
    out.clear();
    std::string text;
    bool in_script = false;
    bool in_style = false;
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            const size_t end = html.find('>', i + 1);
            if (end == std::string::npos) {
                break;
            }
            const char* tag_start = html.data() + i + 1;
            const char* tag_end = html.data() + end;
            bool closing = false;
            if (*tag_start == '/') {
                closing = true;
                ++tag_start;
            }
            while (tag_start < tag_end &&
                   (*tag_start == ' ' || *tag_start == '\t' || *tag_start == '\n')) {
                ++tag_start;
            }
            std::string name;
            const char* p = tag_start;
            while (p < tag_end && std::isalnum(static_cast<unsigned char>(*p))) {
                name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
                ++p;
            }

            if (name == "script") {
                in_script = !closing;
            } else if (name == "style") {
                in_style = !closing;
            } else if (!in_script && !in_style) {
                if (!closing && (name == "img" || name == "image")) {
                    FlushParagraph(text, out);
                    std::string src = ReadAttrValue(tag_start, tag_end, "src");
                    if (src.empty()) {
                        src = ReadAttrValue(tag_start, tag_end, "xlink:href");
                    }
                    src = DecodeHtmlEntities(src);
                    const std::string path = NormalizeZipPath(base_dir, src);
                    if (!path.empty()) {
                        ContentBlock b;
                        b.kind = ContentKind::kImage;
                        b.image_href = path;
                        out.push_back(std::move(b));
                    }
                } else if (name == "br") {
                    text.push_back('\n');
                } else if (closing && IsBlockEndTag(name)) {
                    text.push_back('\n');
                    FlushParagraph(text, out);
                } else if (!closing && IsBlockEndTag(name) && name != "br") {
                    if (!text.empty()) {
                        text.push_back('\n');
                    }
                }
            }
            i = end + 1;
            continue;
        }

        if (!in_script && !in_style) {
            if (html[i] == '&') {
                const size_t semi = html.find(';', i + 1);
                if (semi != std::string::npos && semi - i < 16) {
                    const std::string decoded = DecodeHtmlEntities(html.substr(i, semi - i + 1));
                    for (char c : decoded) {
                        AppendText(text, c);
                    }
                    i = semi + 1;
                    continue;
                }
            }
            AppendText(text, html[i]);
        }
        ++i;
    }
    FlushParagraph(text, out);
}

}  // namespace reader

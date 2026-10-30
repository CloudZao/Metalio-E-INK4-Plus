#include "html_content.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace reader {
namespace {

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

bool HtmlBlockParser::Emit(bool more) {
    if (!more) while (!text_.empty() && (text_.back() == ' ' || text_.back() == '\n')) text_.pop_back();
    if (text_.empty() && !continuation_) return true;
    ContentBlock block;
    block.text.swap(text_); block.continuation = continuation_; block.continues = more;
    if (!sink_(std::move(block))) return false;
    continuation_ = more;
    if (!more) last_space_ = true;
    return true;
}

bool HtmlBlockParser::Append(char c) {
    // UTF-8 最多跨分片边界补齐三个字节，异常续字节不能无限扩展缓冲。
    if (text_.size() >= 8195 && (static_cast<uint8_t>(c) & 0xC0) == 0x80) return false;
    if (text_.size() >= 8192 && (static_cast<uint8_t>(c) & 0xC0) != 0x80 && !Emit(true)) return false;
    text_.push_back(c);
    last_space_ = c == ' ' || c == '\n';
    return true;
}

bool HtmlBlockParser::Text(char c) {
    if (c == '\r') return true;
    if (c == '\n' || c == '\t') c = ' ';
    if (c == ' ' && last_space_) return true;
    return Append(c);
}

bool HtmlBlockParser::Tag() {
    const char* begin = tag_.data();
    const char* end = begin + tag_.size();
    bool closing = begin < end && *begin == '/';
    if (closing) ++begin;
    while (begin < end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    const char* p = begin;
    while (p < end && (std::isalnum(static_cast<unsigned char>(*p)) || *p == ':' || *p == '-' || *p == '_')) ++p;
    std::string name = ToLower(std::string(begin, p));
    const size_t colon = name.find(':');
    if (colon != std::string::npos) name.erase(0, colon + 1);
    if (name == "head") in_head_ = !closing;
    else if (name == "script") in_script_ = !closing;
    else if (name == "style") in_style_ = !closing;
    else if (!in_head_ && !in_script_ && !in_style_) {
        if (!closing && (name == "img" || name == "image")) {
            if (!Emit(false)) return false;
            std::string src = ReadAttrValue(begin, end, "src");
            if (src.empty()) src = ReadAttrValue(begin, end, "xlink:href");
            if (src.empty()) src = ReadAttrValue(begin, end, "href");
            ContentBlock image; image.kind = ContentKind::kImage;
            image.image_href = NormalizeZipPath(base_, DecodeHtmlEntities(src));
            if (!image.image_href.empty() && !sink_(std::move(image))) return false;
        } else if (name == "br") {
            if (!Append('\n')) return false;
        } else if (closing && IsBlockEndTag(name)) {
            if (!Emit(false)) return false;
        } else if (!closing && IsBlockEndTag(name) && name != "br" && !text_.empty()) {
            if (!Append('\n')) return false;
        }
    }
    return true;
}

bool HtmlBlockParser::Feed(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        const char c = static_cast<char>(data[i]);
        if (in_tag_) {
            if (comment_) {
                tag_.push_back(c);
                if (tag_.size() > 3) tag_.erase(0, tag_.size() - 3);
                if (tag_ == "-->") { comment_ = in_tag_ = false; tag_.clear(); }
                continue;
            }
            if (c == '>' && !quote_) {
                if (!Tag()) return false;
                tag_.clear(); in_tag_ = false;
            } else {
                if (tag_.size() >= 8192) return false;
                tag_.push_back(c);
                if (tag_ == "!--") { comment_ = true; tag_.clear(); }
                else if (quote_ && c == quote_) quote_ = 0;
                else if (!quote_ && (c == '\'' || c == '"')) quote_ = c;
            }
            continue;
        }
        if (!entity_.empty()) {
            if (c == ';') {
                entity_.push_back(c);
                for (char out : DecodeHtmlEntities(entity_)) if (!Text(out)) return false;
                entity_.clear(); continue;
            }
            if (entity_.size() < 16 && c != '<' && c != '&' && !std::isspace(static_cast<unsigned char>(c))) {
                entity_.push_back(c); continue;
            }
            for (char out : entity_) if (!Text(out)) return false;
            entity_.clear();
        }
        if (c == '<') { in_tag_ = true; continue; }
        if (in_head_ || in_script_ || in_style_) continue;
        if (c == '&') entity_ = "&";
        else if (!Text(c)) return false;
    }
    return true;
}

bool HtmlBlockParser::Finish() {
    if (in_tag_) return false;
    for (char c : entity_) if (!Text(c)) return false;
    entity_.clear();
    return Emit(false);
}

} // namespace reader

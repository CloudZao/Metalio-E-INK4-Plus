#include "epub_document.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <esp_log.h>
#ifndef EBOOK_EMULATOR
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include "html_content.h"
#include "image_stream.h"

namespace reader {
namespace {

constexpr const char* TAG = "EpubDoc";

std::string BytesToString(const std::vector<uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string TitleFromPathFallback(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) {
        base = base.substr(0, dot);
    }
    return base;
}

std::string XmlAttr(const std::string& tag, const char* attr) {
    const size_t alen = std::strlen(attr);
    for (size_t i = 0; i + alen < tag.size(); ++i) {
        bool match = true;
        for (size_t k = 0; k < alen; ++k) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(tag[i + k])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(attr[k])));
            if (a != b) {
                match = false;
                break;
            }
        }
        if (!match) {
            continue;
        }
        if (i > 0) {
            const char prev = tag[i - 1];
            if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_' || prev == '-' ||
                prev == ':') {
                continue;
            }
        }
        size_t j = i + alen;
        while (j < tag.size() && std::isspace(static_cast<unsigned char>(tag[j]))) {
            ++j;
        }
        if (j >= tag.size() || tag[j] != '=') {
            continue;
        }
        ++j;
        while (j < tag.size() && std::isspace(static_cast<unsigned char>(tag[j]))) {
            ++j;
        }
        if (j >= tag.size()) {
            return {};
        }
        char quote = 0;
        if (tag[j] == '"' || tag[j] == '\'') {
            quote = tag[j++];
        }
        std::string val;
        while (j < tag.size()) {
            if (quote) {
                if (tag[j] == quote) {
                    break;
                }
            } else if (tag[j] == ' ' || tag[j] == '\t' || tag[j] == '>' || tag[j] == '/') {
                break;
            }
            val.push_back(tag[j++]);
        }
        return DecodeHtmlEntities(val);
    }
    return {};
}

std::string LocalName(const std::string& tag_name) {
    const size_t colon = tag_name.find(':');
    return colon == std::string::npos ? tag_name : tag_name.substr(colon + 1);
}

bool TagNameIs(const std::string& open_tag, const char* local) {
    std::string name;
    size_t i = 0;
    if (!open_tag.empty() && open_tag[0] == '<') {
        i = 1;
    }
    while (i < open_tag.size() &&
           (std::isalnum(static_cast<unsigned char>(open_tag[i])) || open_tag[i] == ':' ||
            open_tag[i] == '_' || open_tag[i] == '-')) {
        name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(open_tag[i]))));
        ++i;
    }
    return LocalName(name) == local;
}

std::string TextBetween(const std::string& xml, size_t open_end, const char* close_local) {
    const std::string close1 = std::string("</") + close_local + ">";
    const std::string close2 = std::string("</dc:") + close_local + ">";
    size_t close = xml.find(close1, open_end);
    if (close == std::string::npos) {
        close = xml.find(close2, open_end);
    }
    if (close == std::string::npos) {
        const std::string needle = "</";
        size_t p = open_end;
        while ((p = xml.find(needle, p)) != std::string::npos) {
            size_t e = xml.find('>', p);
            if (e == std::string::npos) {
                break;
            }
            std::string t = xml.substr(p + 2, e - (p + 2));
            while (!t.empty() && t.back() == ' ') {
                t.pop_back();
            }
            const size_t colon = t.find(':');
            const std::string local = colon == std::string::npos ? t : t.substr(colon + 1);
            if (local == close_local) {
                close = p;
                break;
            }
            p = e + 1;
        }
    }
    if (close == std::string::npos || close < open_end) {
        return {};
    }
    return DecodeHtmlEntities(xml.substr(open_end, close - open_end));
}

void TrimInPlace(std::string& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\r' || s.front() == '\t')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t')) {
        s.pop_back();
    }
}

}  // namespace

std::string EpubDocument::DirOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }
    return path.substr(0, slash + 1);
}

void EpubDocument::Close() {
    zip_.Close();
    path_.clear();
    content_base_.clear();
    title_.clear();
    author_.clear();
    language_.clear();
    cover_href_.clear();
    spine_.clear();
    toc_.clear();
    manifest_.clear();
    manifest_props_.clear();
}

bool EpubDocument::Open(const char* path) {
    error_ = BookReadError::kInvalidData;
    Close();
    if (path == nullptr) return false;
    if (!zip_.Open(path)) {
        error_ = zip_.Error();
        return false;
    }
    path_ = path;
    if (!ParseContainer()) {
        if (zip_.Error() != BookReadError::kNone) error_ = zip_.Error();
        Close();
        return false;
    }
    return true;
}

bool EpubDocument::ParseContainer() {
    std::vector<uint8_t> bytes;
    if (!zip_.ReadEntry("META-INF/container.xml", bytes, 64 * 1024)) {
        ESP_LOGE(TAG, "missing container.xml");
        return false;
    }
    const std::string xml = BytesToString(bytes);
    size_t pos = 0;
    std::string opf;
    while ((pos = xml.find("rootfile", pos)) != std::string::npos) {
        const size_t tag_end = xml.find('>', pos);
        if (tag_end == std::string::npos) {
            break;
        }
        const std::string tag = xml.substr(pos, tag_end - pos);
        const std::string media = XmlAttr(tag, "media-type");
        const std::string full = XmlAttr(tag, "full-path");
        if (!full.empty() &&
            (media.empty() || media.find("oebps-package") != std::string::npos ||
             media.find("opf") != std::string::npos)) {
            opf = full;
            break;
        }
        pos = tag_end + 1;
    }
    if (opf.empty()) {
        ESP_LOGE(TAG, "rootfile not found");
        return false;
    }
    return ParseOpf(NormalizeZipPath({}, opf));
}

bool EpubDocument::ParseOpf(const std::string& opf_path) {
    std::vector<uint8_t> bytes;
    if (!zip_.ReadEntry(opf_path.c_str(), bytes, 512 * 1024)) {
        ESP_LOGE(TAG, "cannot read opf error=%d", static_cast<int>(zip_.Error()));
        return false;
    }
    content_base_ = DirOf(opf_path);
    const std::string xml = BytesToString(bytes);

    // 一次扫描：兼容 <item>/<opf:item>、<itemref>/<opf:itemref>、<meta>/<opf:meta>
    // （so-novel / EPUB4J 等会给 OPF 元素加 opf: 前缀；旧逻辑只找字面量 <item 会得到 spine=0）
    std::string cover_id, nav_href, ncx_href, toc_id;
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        const size_t tag_end = xml.find('>', pos);
        if (tag_end == std::string::npos) {
            break;
        }
        const std::string tag = xml.substr(pos, tag_end - pos);
        const size_t last = tag.find_last_not_of(" \t\r\n");
        const bool self_closing = last != std::string::npos && tag[last] == '/';
        if (TagNameIs(tag, "item")) {
            const std::string id = XmlAttr(tag, "id");
            const std::string href = XmlAttr(tag, "href");
            const std::string props = XmlAttr(tag, "properties");
            if (XmlAttr(tag, "media-type") == "application/x-dtbncx+xml") ncx_href = NormalizeZipPath(content_base_, href);
            std::string tokens = props;
            for (char& c : tokens) if (std::isspace(static_cast<unsigned char>(c))) c = ' ';
            if ((" " + tokens + " ").find(" nav ") != std::string::npos) nav_href = NormalizeZipPath(content_base_, href);
            if (!id.empty() && !href.empty()) {
                manifest_[id] = NormalizeZipPath(content_base_, href);
                if (!props.empty()) {
                    manifest_props_[id] = props;
                    if (props.find("cover-image") != std::string::npos) {
                        cover_href_ = manifest_[id];
                    }
                }
            }
        } else if (TagNameIs(tag, "spine")) {
            toc_id = XmlAttr(tag, "toc");
        } else if (TagNameIs(tag, "itemref")) {
            const std::string idref = XmlAttr(tag, "idref");
            auto it = manifest_.find(idref);
            if (it != manifest_.end()) {
                EpubSpineItem item;
                item.id = idref;
                item.href = it->second;
                spine_.push_back(std::move(item));
            }
        } else if (TagNameIs(tag, "meta")) {
            const std::string name = XmlAttr(tag, "name");
            const std::string content = XmlAttr(tag, "content");
            if (name == "cover" && !content.empty()) {
                cover_id = content;
            }
        } else if (TagNameIs(tag, "title") && title_.empty() && !self_closing) {
            title_ = TextBetween(xml, tag_end + 1, "title");
            TrimInPlace(title_);
        } else if (TagNameIs(tag, "language") && language_.empty() && !self_closing) {
            language_ = TextBetween(xml, tag_end + 1, "language");
            TrimInPlace(language_);
        } else if ((TagNameIs(tag, "creator") || TagNameIs(tag, "author")) && author_.empty() &&
                   !self_closing) {
            author_ = TextBetween(xml, tag_end + 1, TagNameIs(tag, "creator") ? "creator" : "author");
            TrimInPlace(author_);
        }
        pos = tag_end + 1;
    }

    if (cover_href_.empty() && !cover_id.empty()) {
        auto it = manifest_.find(cover_id);
        if (it != manifest_.end()) {
            cover_href_ = it->second;
        }
    }

    if (title_.empty()) {
        title_ = TitleFromPathFallback(path_);
    }

    const auto ncx = manifest_.find(toc_id);
    if (ncx != manifest_.end()) ncx_href = ncx->second;
    if (nav_href.empty() || !ParseNavigation(nav_href, false)) {
        if (!ncx_href.empty()) ParseNavigation(ncx_href, true);
    }
    ESP_LOGI(TAG, "opf ok spine=%d toc=%u", SpineCount(), static_cast<unsigned>(toc_.size()));
    return !spine_.empty();
}

bool EpubDocument::ParseNavigation(const std::string& href, bool ncx) {
    struct Node {
        std::string title; // 目录标签文本
        std::string link; // 相对导航文档的链接
        uint16_t depth = 0; // 原生嵌套层级
    };
    if (!zip_.OpenEntryStream(href.c_str())) {
        ESP_LOGW(TAG, "toc open failed format=%s error=%d", ncx ? "ncx" : "nav", static_cast<int>(zip_.Error()));
        return false;
    }
    std::vector<Node> nodes;
    std::vector<size_t> stack;
    std::string tag, type_attribute = "epub:type";
    bool in_tag = false, invalid = false;
    char quote = 0;
    int nav_depth = 0, list_depth = 0, span_depth = 0;
    size_t capture = SIZE_MAX, total = 0;
    const auto add = [&](const std::string& link, int depth) -> size_t {
        if (nodes.size() >= 2048 || depth > 64) { invalid = true; return SIZE_MAX; }
        nodes.push_back({{}, link, static_cast<uint16_t>(std::max(0, depth))}); return nodes.size() - 1;
    };
    uint8_t bytes[1024];
    size_t count = 0;
    while (!invalid && (count = zip_.EntryStreamRead(bytes, sizeof(bytes))) != 0) {
        total += count;
        if (total > 512 * 1024) { invalid = true; break; }
        for (size_t i = 0; i < count && !invalid; ++i) {
            const char c = static_cast<char>(bytes[i]);
            if (!in_tag) {
                if (c == '<') { tag = "<"; in_tag = true; quote = 0; }
                else if (capture != SIZE_MAX) {
                    if (nodes[capture].title.size() >= 1024) { invalid = true; break; }
                    nodes[capture].title.push_back(c);
                }
                continue;
            }
            tag.push_back(c);
            if (tag.size() > 4096) { invalid = true; break; }
            if (tag.compare(0, 4, "<!--") == 0) {
                if (tag.size() >= 7 && tag.compare(tag.size() - 3, 3, "-->") == 0) in_tag = false;
                continue;
            }
            if (tag.compare(0, 9, "<![CDATA[") == 0) {
                if (tag.size() >= 12 && tag.compare(tag.size() - 3, 3, "]]>") == 0) {
                    if (capture != SIZE_MAX) {
                        for (char literal : tag.substr(9, tag.size() - 12)) {
                            if (literal == '&') nodes[capture].title += "&amp;";
                            else nodes[capture].title += literal;
                        }
                        if (nodes[capture].title.size() > 1024) invalid = true;
                    }
                    in_tag = false;
                }
                continue;
            }
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '\'' || c == '"') { quote = c; continue; }
            if (c != '>') continue;
            in_tag = false;
            const bool closing = tag.size() > 1 && tag[1] == '/';
            const std::string open = closing ? "<" + tag.substr(2) : tag;
            size_t ns = 0;
            while ((ns = tag.find("xmlns:", ns)) != std::string::npos) {
                size_t end = ns + 6;
                while (end < tag.size() && (std::isalnum(static_cast<unsigned char>(tag[end])) || tag[end] == '_' || tag[end] == '-')) ++end;
                const std::string key = tag.substr(ns, end - ns);
                if (XmlAttr(tag, key.c_str()) == "http://www.idpf.org/2007/ops") type_attribute = key.substr(6) + ":type";
                ns = end;
            }
            if (ncx) {
                if (TagNameIs(open, "navpoint")) {
                    capture = SIZE_MAX;
                    if (closing) { if (stack.empty()) invalid = true; else stack.pop_back(); }
                    else { const size_t index = add({}, stack.size()); if (index != SIZE_MAX) stack.push_back(index); }
                } else if (TagNameIs(open, "text") && !stack.empty()) capture = closing ? SIZE_MAX : stack.back();
                else if (!closing && TagNameIs(open, "content") && !stack.empty()) nodes[stack.back()].link = XmlAttr(tag, "src");
            } else if (TagNameIs(open, "nav")) {
                if (closing) { if (nav_depth > 0) --nav_depth; capture = SIZE_MAX; }
                else {
                    std::string type = XmlAttr(tag, type_attribute.c_str());
                    for (char& t : type) if (std::isspace(static_cast<unsigned char>(t))) t = ' ';
                    if (nav_depth > 0 || (" " + type + " ").find(" toc ") != std::string::npos || XmlAttr(tag, "role") == "doc-toc") ++nav_depth;
                }
            } else if (nav_depth > 0) {
                if (TagNameIs(open, "ol")) list_depth += closing ? -1 : 1;
                else if (TagNameIs(open, "a")) {
                    if (closing) { capture = SIZE_MAX; span_depth = 0; }
                    else { capture = add(XmlAttr(tag, "href"), list_depth - 1); span_depth = 0; }
                } else if (TagNameIs(open, "span")) {
                    if (!closing && capture == SIZE_MAX) { capture = add({}, list_depth - 1); span_depth = 1; }
                    else if (!closing && span_depth > 0) ++span_depth;
                    else if (closing && span_depth > 0 && --span_depth == 0) capture = SIZE_MAX;
                }
            }
        }
        #ifndef EBOOK_EMULATOR
        vTaskDelay(1);
        #endif
    }
    const bool failed = invalid || in_tag || quote || zip_.EntryStreamError() || !stack.empty() || nav_depth != 0;
    zip_.CloseEntryStream();
    if (failed) {
        ESP_LOGW(TAG, "toc parse failed format=%s bytes=%u; trying fallback", ncx ? "ncx" : "nav", static_cast<unsigned>(total));
        return false;
    }
    std::vector<EpubTocEntry> parsed;
    for (size_t i = 0; i < nodes.size(); ++i) {
        Node& node = nodes[i];
        if (node.link.empty()) {
            for (size_t j = i + 1; j < nodes.size() && nodes[j].depth > node.depth; ++j) {
                if (!nodes[j].link.empty()) { node.link = nodes[j].link; break; }
            }
        }
        std::string title;
        for (char c : DecodeHtmlEntities(node.title)) {
            if (std::isspace(static_cast<unsigned char>(c))) { if (!title.empty() && title.back() != ' ') title += ' '; }
            else title += c;
        }
        TrimInPlace(title);
        if (title.empty() || node.link.empty() || node.link.find(':') != std::string::npos) continue;
        const size_t hash = node.link.find('#');
        const std::string path = hash == 0 ? href : NormalizeZipPath(DirOf(href), node.link);
        const auto item = std::find_if(spine_.begin(), spine_.end(), [&](const EpubSpineItem& s) { return s.href == path; });
        if (item == spine_.end()) continue;
        EpubTocEntry entry;
        entry.title = std::move(title); entry.depth = node.depth; entry.spine_index = item - spine_.begin();
        if (hash != std::string::npos) {
            entry.fragment = DecodeUrlComponent(node.link.substr(hash + 1));
            if (entry.fragment.size() > 1024 || (hash + 1 < node.link.size() && entry.fragment.empty())) continue;
        }
        parsed.push_back(std::move(entry));
    }
    if (parsed.empty()) {
        ESP_LOGW(TAG, "toc has no local targets format=%s", ncx ? "ncx" : "nav");
        return false;
    }
    toc_.swap(parsed);
    return true;
}

const EpubSpineItem* EpubDocument::SpineItem(int index) const {
    if (index < 0 || index >= SpineCount()) {
        return nullptr;
    }
    return &spine_[static_cast<size_t>(index)];
}

bool EpubDocument::LoadItemBytes(const char* href, std::vector<uint8_t>& out, size_t max_bytes) {
    out.clear();
    if (href == nullptr || href[0] == '\0') {
        return false;
    }
    return zip_.ReadEntry(href, out, max_bytes);
}

bool EpubDocument::GetItemUncompressedSize(const char* href, size_t* size) {
    if (href == nullptr || href[0] == '\0' || size == nullptr) {
        return false;
    }
    return zip_.GetUncompressedSize(href, size);
}

bool EpubDocument::LoadCoverBytes(std::vector<uint8_t>& out, size_t max_bytes) {
    out.clear();
    if (cover_href_.empty()) {
        return false;
    }
    return LoadItemBytes(cover_href_.c_str(), out, max_bytes);
}

bool EpubDocument::DecodeItemImageToL8(const char* href, int max_w, int max_h, RasterImage& out,
                                       const std::atomic<bool>* abort) {
    out.Reset();
    if (href == nullptr || href[0] == '\0') {
        return false;
    }
    return DecodeZipEntryImageToL8(zip_, href, max_w, max_h, out, abort);
}

bool EpubDocument::DecodeCoverImageToL8(int max_w, int max_h, RasterImage& out,
                                        const std::atomic<bool>* abort) {
    out.Reset();
    if (cover_href_.empty()) {
        return false;
    }
    return DecodeItemImageToL8(cover_href_.c_str(), max_w, max_h, out, abort);
}

bool EpubDocument::StreamChapterBlocks(int spine_index, const ContentBlockSink& sink,
                                      const HtmlAnchorSink& anchors, const std::atomic<bool>* cancelled) {
    error_ = BookReadError::kInvalidData;
    const auto* item = SpineItem(spine_index);
    if (!item) return false;
    if (!zip_.OpenEntryStream(item->href.c_str())) { error_ = zip_.Error(); return false; }
    struct StreamGuard { ZipReader& zip; ~StreamGuard() { zip.CloseEntryStream(); } } guard{zip_};
    HtmlBlockParser parser(DirOf(item->href), sink, anchors);
    uint8_t bytes[2048];
    uint64_t total = 0;
    while (!zip_.EntryStreamEof() && !zip_.EntryStreamError()) {
        if (cancelled && cancelled->load()) { error_ = BookReadError::kCancelled; return false; }
        const size_t got = zip_.EntryStreamRead(bytes, sizeof(bytes));
        total += got;
#ifndef EBOOK_EMULATOR
        if ((total & 16383) == 0) vTaskDelay(1);
#endif
        if (total > zip_.EntryStreamUncompSize() || !parser.Feed(bytes, got)) return false;
        if (got == 0 && !zip_.EntryStreamEof()) return false;
    }
    if (zip_.EntryStreamError()) { error_ = zip_.Error(); return false; }
    if (total != zip_.EntryStreamUncompSize() || !parser.Finish()) return false;
    error_ = BookReadError::kNone;
    return true;
}

bool EpubDocument::DetectPageImages() {
    if (spine_.empty()) return false;
    size_t markup_bytes = 0;
    for (int index = 0; index < SpineCount(); ++index) {
        size_t size = 0;
        // 一页一图的标记文档应很小；大型或复杂文档保留普通图文排版。
        if (!GetItemUncompressedSize(spine_[index].href.c_str(), &size) || size > 128 * 1024 ||
            size > 4 * 1024 * 1024 - markup_bytes) return false;
        markup_bytes += size;
        int images = 0;
        const bool ok = StreamChapterBlocks(index, [&](ContentBlock&& block) {
            if (block.kind == ContentKind::kImage) return ++images <= 1;
            for (unsigned char c : block.text) if (!std::isspace(c)) return false;
            return true;
        });
        if (!ok || images != 1) return false;
    }
    return true;
}

}  // namespace reader

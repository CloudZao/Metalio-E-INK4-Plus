#include "epub_document.h"

#include <cctype>
#include <cstring>

#include <esp_log.h>

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
        while (j < tag.size() && (tag[j] == ' ' || tag[j] == '\t')) {
            ++j;
        }
        if (j >= tag.size() || tag[j] != '=') {
            continue;
        }
        ++j;
        while (j < tag.size() && (tag[j] == ' ' || tag[j] == '\t')) {
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
    cover_href_.clear();
    spine_.clear();
    manifest_.clear();
    manifest_props_.clear();
}

bool EpubDocument::Open(const char* path) {
    Close();
    if (path == nullptr || !zip_.Open(path)) {
        return false;
    }
    path_ = path;
    if (!ParseContainer()) {
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
    return ParseOpf(opf);
}

bool EpubDocument::ParseOpf(const std::string& opf_path) {
    std::vector<uint8_t> bytes;
    if (!zip_.ReadEntry(opf_path.c_str(), bytes, 512 * 1024)) {
        ESP_LOGE(TAG, "cannot read opf %s", opf_path.c_str());
        return false;
    }
    content_base_ = DirOf(opf_path);
    const std::string xml = BytesToString(bytes);

    size_t pos = 0;
    std::string cover_id;
    while ((pos = xml.find("<item", pos)) != std::string::npos) {
        const size_t tag_end = xml.find('>', pos);
        if (tag_end == std::string::npos) {
            break;
        }
        if (xml.compare(pos, 8, "<itemref") == 0) {
            pos = tag_end + 1;
            continue;
        }
        const std::string tag = xml.substr(pos, tag_end - pos);
        const std::string id = XmlAttr(tag, "id");
        const std::string href = XmlAttr(tag, "href");
        const std::string props = XmlAttr(tag, "properties");
        if (!id.empty() && !href.empty()) {
            manifest_[id] = NormalizeZipPath(content_base_, href);
            if (!props.empty()) {
                manifest_props_[id] = props;
                if (props.find("cover-image") != std::string::npos) {
                    cover_href_ = manifest_[id];
                }
            }
        }
        pos = tag_end + 1;
    }

    pos = 0;
    while ((pos = xml.find("<meta", pos)) != std::string::npos) {
        const size_t tag_end = xml.find('>', pos);
        if (tag_end == std::string::npos) {
            break;
        }
        const std::string tag = xml.substr(pos, tag_end - pos);
        const std::string name = XmlAttr(tag, "name");
        const std::string content = XmlAttr(tag, "content");
        if (name == "cover" && !content.empty()) {
            cover_id = content;
        }
        pos = tag_end + 1;
    }
    if (cover_href_.empty() && !cover_id.empty()) {
        auto it = manifest_.find(cover_id);
        if (it != manifest_.end()) {
            cover_href_ = it->second;
        }
    }

    pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        const size_t tag_end = xml.find('>', pos);
        if (tag_end == std::string::npos) {
            break;
        }
        const std::string open = xml.substr(pos, tag_end - pos);
        if (TagNameIs(open, "title") && title_.empty() && open.find('/') == std::string::npos) {
            title_ = TextBetween(xml, tag_end + 1, "title");
            TrimInPlace(title_);
        } else if ((TagNameIs(open, "creator") || TagNameIs(open, "author")) && author_.empty() &&
                   open.find('/') == std::string::npos) {
            author_ = TextBetween(xml, tag_end + 1, TagNameIs(open, "creator") ? "creator" : "author");
            TrimInPlace(author_);
        }
        pos = tag_end + 1;
    }

    pos = 0;
    while ((pos = xml.find("<itemref", pos)) != std::string::npos) {
        const size_t tag_end = xml.find('>', pos);
        if (tag_end == std::string::npos) {
            break;
        }
        const std::string tag = xml.substr(pos, tag_end - pos);
        const std::string idref = XmlAttr(tag, "idref");
        auto it = manifest_.find(idref);
        if (it != manifest_.end()) {
            EpubSpineItem item;
            item.id = idref;
            item.href = it->second;
            spine_.push_back(std::move(item));
        }
        pos = tag_end + 1;
    }

    if (title_.empty()) {
        title_ = TitleFromPathFallback(path_);
    }

    ESP_LOGI(TAG, "opf ok title='%s' spine=%d cover='%s'", title_.c_str(), SpineCount(),
             cover_href_.c_str());
    return !spine_.empty();
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

bool EpubDocument::LoadChapterBlocks(int spine_index, std::vector<ContentBlock>& out, size_t max_html_bytes) {
    out.clear();
    const EpubSpineItem* item = SpineItem(spine_index);
    if (item == nullptr) {
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!zip_.ReadEntry(item->href.c_str(), bytes, max_html_bytes)) {
        ESP_LOGW(TAG, "chapter read fail %s", item->href.c_str());
        return false;
    }
    // 确保以 '\0' 结尾的安全字符串
    std::string html = BytesToString(bytes);
    const std::string base = DirOf(item->href);
    HtmlToBlocks(html, base, out);
    return !out.empty();
}

}  // namespace reader

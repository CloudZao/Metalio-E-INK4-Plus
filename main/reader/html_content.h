#pragma once

#include <functional>
#include <string>
#include <vector>

#include "reader_types.h"

namespace reader {

using HtmlAnchorSink = std::function<void(const std::string&, uint32_t, uint32_t)>; // 锚点名称、正文块序号及块内字节偏移

/** @brief 有界 HTML 流解析器，跨读取块保留标签、实体及段落状态。 */
class HtmlBlockParser {
public:
    /** @brief 创建流式解析器，可选观察锚点对应的正文块位置。 */
    HtmlBlockParser(const std::string& base, const ContentBlockSink& sink, const HtmlAnchorSink& anchors = {})
        : base_(base), anchors_(anchors), sink_(sink) {}
    /** @brief 追加 HTML 数据块并同步输出正文，失败或消费者停止时返回 false。 */
    bool Feed(const uint8_t* data, size_t size);
    /** @brief 结束输入并输出剩余正文，返回处理是否成功。 */
    bool Finish();
private:
    bool Emit(bool more);
    bool Append(char c);
    bool Text(char c);
    bool Tag();
    void Anchor(const char* begin, const char* end);
    std::string base_; // 图片相对路径基准
    HtmlAnchorSink anchors_; // 可选锚点观察者，不改变正文分块
    uint32_t source_block_ = 0, source_offset_ = 0; // 与输出正文块一致的位置
    ContentBlockSink sink_; // 同步内容消费者
    std::string text_, tag_, entity_; // 有界段落片段、标签及实体缓冲
    bool in_tag_ = false, in_head_ = false, in_script_ = false, in_style_ = false, comment_ = false; // 解析状态
    bool in_heading_ = false; // 独立 h1 至 h6 段沿用阅读器标题样式
    bool continuation_ = false; // 当前段是否已输出过前序片段
    bool last_space_ = true; // 跨输出片段保留空白合并状态
    char quote_ = 0; // 标签属性引号
};

/** @brief 解码常用 HTML 实体及 Unicode 数字引用。 */
std::string DecodeHtmlEntities(const std::string& in);

/** @brief 解码 EPUB 链接中的百分号编码，格式无效时返回空串。 */
std::string DecodeUrlComponent(const std::string& in);

/** @brief 解析 ZIP 内相对路径、根路径和百分号编码，拒绝外部链接及越过根目录的路径。 */
std::string NormalizeZipPath(const std::string& base_dir, const std::string& href);

}  // namespace reader

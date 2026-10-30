#pragma once

#include <string>
#include <vector>

#include "reader_types.h"

namespace reader {

/** @brief 有界 HTML 流解析器，跨读取块保留标签、实体及段落状态。 */
class HtmlBlockParser {
public:
    HtmlBlockParser(const std::string& base, const ContentBlockSink& sink) : base_(base), sink_(sink) {}
    /** @brief 追加 HTML 数据块并同步输出正文，失败或消费者停止时返回 false。 */
    bool Feed(const uint8_t* data, size_t size);
    /** @brief 结束输入并输出剩余正文，返回处理是否成功。 */
    bool Finish();
private:
    bool Emit(bool more);
    bool Append(char c);
    bool Text(char c);
    bool Tag();
    std::string base_; // 图片相对路径基准
    ContentBlockSink sink_; // 同步内容消费者
    std::string text_, tag_, entity_; // 有界段落片段、标签及实体缓冲
    bool in_tag_ = false, in_head_ = false, in_script_ = false, in_style_ = false, comment_ = false; // 解析状态
    bool continuation_ = false; // 当前段是否已输出过前序片段
    bool last_space_ = true; // 跨输出片段保留空白合并状态
    char quote_ = 0; // 标签属性引号
};

// 解析 HTML 实体与数字引用（常用子集）。
std::string DecodeHtmlEntities(const std::string& in);

// 规范化 ZIP 内路径：去掉 ./ 、合并 ..、统一 /
std::string NormalizeZipPath(const std::string& base_dir, const std::string& href);

}  // namespace reader

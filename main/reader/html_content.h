#pragma once

#include <string>
#include <vector>

#include "reader_types.h"

namespace reader {

// 将 XHTML/HTML 粗解析为文本段 + 图片引用（参考 CrossPoint 思路，精简可靠）。
// base_dir：章节所在目录（用于解析相对 src），以 / 结尾或空。
void HtmlToBlocks(const std::string& html, const std::string& base_dir, std::vector<ContentBlock>& out);

// 解析 HTML 实体与数字引用（常用子集）。
std::string DecodeHtmlEntities(const std::string& in);

// 规范化 ZIP 内路径：去掉 ./ 、合并 ..、统一 /
std::string NormalizeZipPath(const std::string& base_dir, const std::string& href);

}  // namespace reader

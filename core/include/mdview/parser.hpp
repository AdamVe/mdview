#pragma once
#include <expected>
#include <string>
#include "doc_model.hpp"

namespace mdview {

    constexpr size_t MAX_INPUT_SIZE = 10 * 1024 * 1024;  // 10 MB
    constexpr int MAX_INLINE_DEPTH = 32;

    [[nodiscard]] std::expected<DocModel, ParserError> parse_markdown(const std::string &markdown_text);

} // namespace mdview

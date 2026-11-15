#pragma once
#include <expected>
#include <string>
#include "doc_model.hpp"

namespace mdview {

    // Real-world markdown is tiny (median README ~10 KB); anything past a few
    // hundred KB is machine-generated. 2 MB keeps GtkTextView rendering and
    // zoom rebuilds comfortably interactive without any virtualization.
    constexpr size_t MAX_INPUT_SIZE = 2 * 1024 * 1024; // 2 MB
    constexpr int MAX_INLINE_DEPTH = 32;
    constexpr int MAX_LIST_DEPTH = 16;

    [[nodiscard]] std::expected<DocModel, ParserError> parse_markdown(const std::string &markdown_text);

} // namespace mdview

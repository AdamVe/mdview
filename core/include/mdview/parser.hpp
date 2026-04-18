#pragma once
#include <expected>
#include <string>
#include "doc_model.hpp"

namespace mdview {

    [[nodiscard]] std::expected<DocModel, ParserError> parse_markdown(const std::string &markdown_text);

} // namespace mdview

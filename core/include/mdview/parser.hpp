#pragma once
#include <string>
#include "doc_model.hpp"

namespace mdview {

    [[nodiscard]] DocModel parse_markdown(const std::string &markdown_text);

} // namespace mdview

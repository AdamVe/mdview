#pragma once
#include "doc_model.hpp"
#include <string>

namespace mdview {

[[nodiscard]] DocModel parse_markdown(const std::string& markdown_text);

} // namespace mdview

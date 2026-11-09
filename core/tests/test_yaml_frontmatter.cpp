#include <gtest/gtest.h>
#include <string>
#include "mdview/parser.hpp"
#include "mdview/doc_model.hpp"

using namespace mdview;

TEST(ParserTest, Parse_YamlFrontMatter) {
    std::string md = R"(---
title: Test
author: Adam
---

# Header

Some content here.
)";
    auto result = parse_markdown(md);
    ASSERT_TRUE(result.has_value());
    std::cout << "Nodes: " << result.value().size() << std::endl;
    for (size_t i = 0; i < result.value().size(); ++i) {
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Heading>) {
                std::cout << "  [" << i << "] Heading" << std::endl;
            } else if constexpr (std::is_same_v<T, Paragraph>) {
                std::cout << "  [" << i << "] Paragraph" << std::endl;
            } else if constexpr (std::is_same_v<T, HRule>) {
                std::cout << "  [" << i << "] HRule" << std::endl;
            } else if constexpr (std::is_same_v<T, CodeBlock>) {
                std::cout << "  [" << i << "] CodeBlock lang=" << n.language << std::endl;
            }
        }, result.value()[i]);
    }
    // Should only have Heading and Paragraph, no HRule or CodeBlock for front matter
    ASSERT_EQ(result.value().size(), 2u);
}

#pragma once
#include <string>
#include <variant>
#include <vector>

namespace mdview {

    struct TextRun {
        std::string text;
        bool bold = false;
        bool italic = false;
        bool code = false; // inline code
        bool strikethrough = false;
    };

    struct Link {
        std::string text;
        std::string url;
    };

    using InlineNode = std::variant<TextRun, Link>;

    struct Heading {
        int level;
        std::vector<InlineNode> content;
    };
    struct Paragraph {
        std::vector<InlineNode> content;
    };
    struct CodeBlock {
        std::string language;
        std::string code;
    };
    struct BlockQuote {
        std::vector<InlineNode> content;
    };
    struct HRule {};
    struct BulletList {
        std::vector<std::vector<InlineNode>> items;
    };
    struct OrderedList {
        std::vector<std::vector<InlineNode>> items;
    };
    struct TaskList {
        std::vector<std::pair<bool, std::vector<InlineNode>>> items;
    };

    using DocNode = std::variant<Heading, Paragraph, CodeBlock, BlockQuote, HRule, BulletList, OrderedList, TaskList>;

    using DocModel = std::vector<DocNode>;

} // namespace mdview

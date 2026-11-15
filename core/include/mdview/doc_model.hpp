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

    struct Image {
        std::string url;
        std::string alt;
    };

    using InlineNode = std::variant<TextRun, Link, Image>;

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
    // One list item at any nesting level. kind is this entry's own marker
    // ('u' bullet, 'o' ordered, 't' task) so a sublist may differ from its
    // parent ("- item\n  1. nested"); children holds deeper levels verbatim.
    struct ListEntry {
        std::vector<InlineNode> content;
        char kind = 'u';
        bool checked = false; // meaningful when kind == 't'
        std::vector<ListEntry> children;
    };
    struct List {
        std::vector<ListEntry> items;
    };
    struct TableCell {
        std::vector<InlineNode> content;
    };
    struct Table {
        std::vector<TableCell> header;
        std::vector<std::vector<TableCell>> rows;
        std::vector<char> alignments;
    };

    enum class ParserError {
        InvalidInput,
        InternalError,
        MalformedInline,
        UnexpectedNode,
        InputTooLarge,
        MaxDepthExceeded
    };

    using DocNode = std::variant<Heading, Paragraph, CodeBlock, BlockQuote, HRule, List, Table>;

    using DocModel = std::vector<DocNode>;

} // namespace mdview

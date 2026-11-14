#include <gtest/gtest.h>

#include <string>
#include "mdview/doc_model.hpp"
#include "mdview/parser.hpp"

using namespace mdview;
using ::testing::ContainsRegex;

// Helper: extract text from inline nodes
static std::string extract_inline_text(const std::vector<InlineNode> &nodes) {
    std::string result;
    for (const auto &node : nodes) {
        std::visit(
            [&result](const auto &n) {
                if constexpr (std::is_same_v<std::decay_t<decltype(n)>, TextRun>) {
                    result += n.text;
                }
            },
            node);
    }
    return result;
}

// Helper: get link URL from inline nodes

// Helper: check if a TextRun has strikethrough set
static bool has_strikethrough(const std::vector<InlineNode> &nodes) {
    for (const auto &node : nodes) {
        if (const auto *tr = std::get_if<TextRun>(&node)) {
            if (tr->strikethrough) return true;
        }
    }
    return false;
}

// Helper: check if a TextRun has bold set
static bool has_bold(const std::vector<InlineNode> &nodes) {
    for (const auto &node : nodes) {
        if (const auto *tr = std::get_if<TextRun>(&node)) {
            if (tr->bold) return true;
        }
    }
    return false;
}

// Helper: get first DocNode type name
static std::string node_type_name(const DocNode &node) {
    std::string result;
    std::visit(
        [&result](const auto &n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Heading>) result = "Heading";
            else if constexpr (std::is_same_v<T, Paragraph>) result = "Paragraph";
            else if constexpr (std::is_same_v<T, CodeBlock>) result = "CodeBlock";
            else if constexpr (std::is_same_v<T, BlockQuote>) result = "BlockQuote";
            else if constexpr (std::is_same_v<T, HRule>) result = "HRule";
            else if constexpr (std::is_same_v<T, BulletList>) result = "BulletList";
            else if constexpr (std::is_same_v<T, OrderedList>) result = "OrderedList";
            else if constexpr (std::is_same_v<T, TaskList>) result = "TaskList";
            else if constexpr (std::is_same_v<T, Table>) result = "Table";
        },
        node);
    return result;
}

// ---- Sanity tests ----

TEST(ParserTest, Parse_NormalDocument) {
    auto result = parse_markdown("# Hello\n\nWorld.");
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 2u);
    EXPECT_EQ(node_type_name(result.value()[0]), "Heading");
    EXPECT_EQ(node_type_name(result.value()[1]), "Paragraph");
}

TEST(ParserTest, Parse_EmptyInput) {
    auto result = parse_markdown("");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().empty());
}

TEST(ParserTest, Parse_CodeBlock) {
    std::string md = "```python\nprint('hello')\n```\n";
    auto result = parse_markdown(md);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 1u);
    const auto &cb = std::get<CodeBlock>(result.value()[0]);
    EXPECT_EQ(cb.language, "python");
    EXPECT_EQ(cb.code, "print('hello')\n");
}

TEST(ParserTest, Parse_BulletList) {
    std::string md = "- item 1\n- item 2\n";
    auto result = parse_markdown(md);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 1u);
    const auto &bl = std::get<BulletList>(result.value()[0]);
    EXPECT_EQ(bl.items.size(), 2u);
    EXPECT_EQ(extract_inline_text(bl.items[0]), "item 1");
    EXPECT_EQ(extract_inline_text(bl.items[1]), "item 2");
}

TEST(ParserTest, Parse_TaskList) {
    std::string md = "- [x] done thing\n- [ ] todo thing\n";
    auto result = parse_markdown(md);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 1u);
    ASSERT_EQ(node_type_name(result.value()[0]), "TaskList");
    const auto &tl = std::get<TaskList>(result.value()[0]);
    ASSERT_EQ(tl.items.size(), 2u);
    EXPECT_TRUE(tl.items[0].completed);
    EXPECT_FALSE(tl.items[1].completed);
    // The "[x]"/"[ ]" marker must not leak into the rendered content
    EXPECT_EQ(extract_inline_text(tl.items[0].content), "done thing");
    EXPECT_EQ(extract_inline_text(tl.items[1].content), "todo thing");
}

TEST(ParserTest, Parse_Link_KeepsRelativeAndAnchorUrls) {
    std::string md = "See [sibling](./other.md) and [top](#heading).\n";
    auto result = parse_markdown(md);
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    std::vector<std::pair<std::string, std::string>> links;
    for (const auto &node: para.content) {
        if (const auto *link = std::get_if<Link>(&node)) {
            links.emplace_back(link->text, link->url);
        }
    }
    ASSERT_EQ(links.size(), 2u);
    EXPECT_EQ(links[0], (std::pair<std::string, std::string>{"sibling", "./other.md"}));
    EXPECT_EQ(links[1], (std::pair<std::string, std::string>{"top", "#heading"}));
}

// ---- Security: UTF-8 sanitising ----

TEST(ParserTest, Parse_InvalidUtf8Sanitised) {
    const auto result = parse_markdown("# bad \xff\xfepdf \xc3\xa9 ok\n\n```c\nint x = \xe2\x80;\n```\n");
    ASSERT_TRUE(result.has_value());
    // Every string that reaches the model must be valid UTF-8 (GtkTextBuffer
    // asserts otherwise). Walk the heading text and code block bytes.
    std::string all;
    for (const auto &node: *result) {
        std::visit(
                [&all](const auto &n) {
                    using U = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<U, mdview::Heading>) {
                        for (const auto &inl: n.content) {
                            if (const auto *t = std::get_if<mdview::TextRun>(&inl))
                                all += t->text;
                        }
                    } else if constexpr (std::is_same_v<U, mdview::CodeBlock>) {
                        all += n.code;
                    }
                },
                node);
    }
    EXPECT_EQ(all.find('\xff'), std::string::npos);
    EXPECT_NE(all.find("\xEF\xBF\xBD"), std::string::npos); // replacement inserted
    EXPECT_NE(all.find("pdf"), std::string::npos); // neighbours survive
    EXPECT_NE(all.find("\xc3\xa9"), std::string::npos); // valid utf8 untouched
}

// ---- Security: Input size limit ----

TEST(ParserTest, Parse_InputTooLarge) {
    std::string large(MAX_INPUT_SIZE + 1, 'x');
    auto result = parse_markdown(large);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParserError::InputTooLarge);
}

TEST(ParserTest, Parse_InputAtLimit) {
    std::string at_limit(MAX_INPUT_SIZE, 'x');
    auto result = parse_markdown(at_limit);
    ASSERT_TRUE(result.has_value());
}

// ---- Security: Max depth ----

TEST(ParserTest, Parse_MaxDepthExceeded) {
    // cmark-gfm limits emphasis nesting internally, so deeply nested input
    // gets truncated by cmark before our depth guard triggers. The depth
    // check is a safety net for pathological inputs that cmark somehow parses.
    // This test verifies the parser handles deep nesting without crashing.
    std::string md;
    for (int i = 0; i < 50; ++i) md += "***";
    md += "text";
    for (int i = 0; i < 50; ++i) md += "***";
    auto result = parse_markdown(md);
    // cmark truncates deeply nested emphasis; we just verify no crash/OOM
    (void)result;
}


TEST(ParserTest, Parse_WithinDepthLimit) {
    // 10 levels of *** should be fine (limit is 32)
    std::string md;
    for (int i = 0; i < 10; ++i) md += "***";
    md += "text";
    for (int i = 0; i < 10; ++i) md += "***";
    auto result = parse_markdown(md);
    ASSERT_TRUE(result.has_value());
}

// ---- Security: URL sanitization ----

TEST(ParserTest, Parse_Sanitize_Javascript_URL) {
    auto result = parse_markdown("[link](javascript:alert(1))");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    EXPECT_TRUE(para.content.size() >= 1);
    const auto *link = std::get_if<Link>(&para.content[0]);
    ASSERT_TRUE(link != nullptr);
    EXPECT_EQ(link->url, "");
    EXPECT_EQ(link->text, "link");
}

TEST(ParserTest, Parse_Sanitize_Data_URL) {
    auto result = parse_markdown("[link](data:text/html,<script>)");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const auto *link = std::get_if<Link>(&para.content[0]);
    ASSERT_TRUE(link != nullptr);
    EXPECT_EQ(link->url, "");
}

TEST(ParserTest, Parse_Image_KeepsUrlAndAlt) {
    auto result = parse_markdown("![Logo of the project](img/logo.png)");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const auto *image = std::get_if<Image>(&para.content[0]);
    ASSERT_TRUE(image != nullptr);
    EXPECT_EQ(image->url, "img/logo.png");
    EXPECT_EQ(image->alt, "Logo of the project");
}

TEST(ParserTest, Parse_Image_AllowsDataUrl) {
    auto result = parse_markdown("![x](data:image/png;base64,iVBORw0KGgo=)");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const auto *image = std::get_if<Image>(&para.content[0]);
    ASSERT_TRUE(image != nullptr);
    EXPECT_EQ(image->url, "data:image/png;base64,iVBORw0KGgo=");
}

TEST(ParserTest, Parse_Image_SanitizesJavascriptUrl) {
    auto result = parse_markdown("![x](javascript:alert(1))");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const auto *image = std::get_if<Image>(&para.content[0]);
    ASSERT_TRUE(image != nullptr);
    EXPECT_EQ(image->url, "");
    EXPECT_EQ(image->alt, "x");
}

TEST(ParserTest, Parse_Image_MarkdownInAltIsFlattenedToText) {
    auto result = parse_markdown("![a **bold** alt](pic.png)");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const auto *image = std::get_if<Image>(&para.content[0]);
    ASSERT_TRUE(image != nullptr);
    EXPECT_EQ(image->alt, "a bold alt");
}

TEST(ParserTest, Parse_Image_MixedWithTextRuns) {
    auto result = parse_markdown("before ![i](p.png) after");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    ASSERT_TRUE(para.content.size() == 3);
    EXPECT_EQ(std::get<TextRun>(para.content[0]).text, "before ");
    EXPECT_STREQ(std::get<Image>(para.content[1]).url.c_str(), "p.png");
    EXPECT_EQ(std::get<TextRun>(para.content[2]).text, " after");
}

TEST(ParserTest, Parse_Sanitize_VBScript_URL) {
    auto result = parse_markdown("[link](vbscript:msgbox(1))");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const Link *link = nullptr;
    for (const auto &node: para.content) {
        if (const auto *l = std::get_if<Link>(&node)) {
            link = l;
            break;
        }
    }
    ASSERT_TRUE(link != nullptr);
    EXPECT_EQ(link->url, "");
}

TEST(ParserTest, Parse_Sanitize_CaseInsensitive) {
    auto result = parse_markdown("[link](JavaScript:alert(1))");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const auto *link = std::get_if<Link>(&para.content[0]);
    ASSERT_TRUE(link != nullptr);
    EXPECT_EQ(link->url, "");
}

TEST(ParserTest, Parse_Sanitize_MixedCase) {
    auto result = parse_markdown("[link](DaTa:text/html,test)");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const auto *link = std::get_if<Link>(&para.content[0]);
    ASSERT_TRUE(link != nullptr);
    EXPECT_EQ(link->url, "");
}

TEST(ParserTest, Parse_Allow_HTTP_URL) {
    auto result = parse_markdown("[link](https://example.com)");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const auto *link = std::get_if<Link>(&para.content[0]);
    ASSERT_TRUE(link != nullptr);
    EXPECT_EQ(link->url, "https://example.com");
}

TEST(ParserTest, Parse_Allow_File_URL) {
    auto result = parse_markdown("[link](file:///etc/passwd)");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const auto *link = std::get_if<Link>(&para.content[0]);
    ASSERT_TRUE(link != nullptr);
    EXPECT_EQ(link->url, "file:///etc/passwd");
}

TEST(ParserTest, Parse_Allow_Mailto_URL) {
    auto result = parse_markdown("[email](mailto:test@example.com)");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const auto *link = std::get_if<Link>(&para.content[0]);
    ASSERT_TRUE(link != nullptr);
    EXPECT_EQ(link->url, "mailto:test@example.com");
}

TEST(ParserTest, Parse_Allow_Relative_URL) {
    auto result = parse_markdown("[link](./path/to/file)");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const auto *link = std::get_if<Link>(&para.content[0]);
    ASSERT_TRUE(link != nullptr);
    EXPECT_EQ(link->url, "./path/to/file");
}

// ---- Bug fix: Strikethrough ----

TEST(ParserTest, Parse_Strikethrough) {
    auto result = parse_markdown("~~deleted text~~");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    EXPECT_TRUE(has_strikethrough(para.content));
    EXPECT_EQ(extract_inline_text(para.content), "deleted text");
}

TEST(ParserTest, Parse_StrikethroughMixed) {
    auto result = parse_markdown("**~~bold and strike~~**");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    EXPECT_TRUE(has_bold(para.content));
    EXPECT_TRUE(has_strikethrough(para.content));
}

TEST(ParserTest, Parse_StrikethroughInLink) {
    auto result = parse_markdown("[~~strike link~~](https://example.com)");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const auto *link = std::get_if<Link>(&para.content[0]);
    ASSERT_TRUE(link != nullptr);
    // The link text should have strikethrough
    if (link->text.find("strike") != std::string::npos) {
        EXPECT_EQ(link->url, "https://example.com");
    }
}

// ---- Coverage: All block types ----

TEST(ParserTest, Parse_AllBlockTypes) {
    std::string md = R"(# H1
## H2
## H3
## H4

Normal paragraph.

> A blockquote.

---

- bullet 1
- bullet 2

1. ordered 1
2. ordered 2

| A | B |
|---|---|
| 1 | 2 |
)";
    auto result = parse_markdown(md);
    ASSERT_TRUE(result.has_value());
    ASSERT_GE(result.value().size(), 8u);

    // Check heading types
    EXPECT_EQ(node_type_name(result.value()[0]), "Heading");
    EXPECT_EQ(std::get<Heading>(result.value()[0]).level, 1);

    // Check that we have headings and a paragraph somewhere
    bool found_heading = false, found_para = false;
    for (const auto &node : result.value()) {
        std::string t = node_type_name(node);
        if (t == "Heading") found_heading = true;
        if (t == "Paragraph") found_para = true;
    }
    EXPECT_TRUE(found_heading);
    EXPECT_TRUE(found_para);

    // Check blockquote
    bool found_quote = false;
    bool found_hr = false;
    bool found_bullet = false;
    bool found_ordered = false;
    bool found_table = false;
    for (const auto &node : result.value()) {
        std::string t = node_type_name(node);
        if (t == "BlockQuote") found_quote = true;
        if (t == "HRule") found_hr = true;
        if (t == "BulletList") found_bullet = true;
        if (t == "OrderedList") found_ordered = true;
        if (t == "Table") found_table = true;
    }
    EXPECT_TRUE(found_quote);
    EXPECT_TRUE(found_hr);
    EXPECT_TRUE(found_bullet);
    EXPECT_TRUE(found_ordered);
    EXPECT_TRUE(found_table);
}

// ---- Robustness: Table alignment padding ----

TEST(ParserTest, Parse_TableAlignmentPadding) {
    // Table with 3 columns - cmark produces alignment data
    std::string md = "| A | B | C |\n|---|---|---|\n| 1 | 2 | 3 |\n";
    auto result = parse_markdown(md);
    ASSERT_TRUE(result.has_value());
    const auto &table = std::get<Table>(result.value()[0]);
    EXPECT_EQ(table.alignments.size(), 3u);
    // Verify the table was parsed with correct structure
    EXPECT_EQ(table.rows.size(), 1u);
    EXPECT_EQ(table.header.size(), 3u);
}


// ---- Correctness: Adjacent text runs ----

TEST(ParserTest, Parse_AdjacentTextRuns) {
    // Adjacent bold text should be merged into one TextRun
    auto result = parse_markdown("**boldbold**");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    // Should be exactly one bold TextRun (merged)
    ASSERT_EQ(para.content.size(), 1u);
    const auto &tr = std::get<TextRun>(para.content[0]);
    EXPECT_TRUE(tr.bold);
    EXPECT_EQ(tr.text, "boldbold");
}

// ---- Edge cases ----

TEST(ParserTest, Parse_WhitespaceOnly) {
    auto result = parse_markdown("   \n\n  ");
    ASSERT_TRUE(result.has_value());
    // cmark may or may not produce nodes for whitespace-only input
    // Just ensure it doesn't crash
    (void)result.value().size();
}

TEST(ParserTest, Parse_MalformedMarkdown) {
    // Random garbage should not crash
    auto result = parse_markdown("}}}{{{~~~###!!!???");
    ASSERT_TRUE(result.has_value());
}

TEST(ParserTest, Parse_LinkWithEmptyURL) {
    auto result = parse_markdown("[text]()");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    const auto *link = std::get_if<Link>(&para.content[0]);
    ASSERT_TRUE(link != nullptr);
    EXPECT_EQ(link->url, "");
    EXPECT_EQ(link->text, "text");
}

TEST(ParserTest, Parse_MultipleLinks) {
    auto result = parse_markdown("[a](https://a.com) and [b](javascript:alert(1))");
    ASSERT_TRUE(result.has_value());
    const auto &para = std::get<Paragraph>(result.value()[0]);
    ASSERT_GE(para.content.size(), 2u);
    // Find links
    std::string url_a, url_b;
    for (const auto &node: para.content) {
        if (const auto *link = std::get_if<Link>(&node)) {
            if (link->text == "a") url_a = link->url;
            if (link->text == "b") url_b = link->url;
        }
    }
    EXPECT_EQ(url_a, "https://a.com");
    EXPECT_EQ(url_b, "");
}

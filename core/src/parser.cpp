#include "mdview/parser.hpp"
#include <cmark-gfm.h>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include "cmark-gfm-core-extensions.h"
#include "cmark-gfm-extension_api.h"

namespace mdview {

    // Replace ill-formed UTF-8 with U+FFFD (one replacement per bad lead byte,
    // WHATWG-style). Untrusted documents reach GtkTextBuffer, which asserts on
    // invalid UTF-8; sanitising here keeps the frontend honest and the display
    // deterministic. Rejects overlong forms, surrogates, > U+10FFFF, truncation.
    static std::string sanitize_utf8(std::string_view in) {
        std::string out;
        out.reserve(in.size());
        size_t i = 0;
        while (i < in.size()) {
            const unsigned char b0 = static_cast<unsigned char>(in[i]);
            if (b0 < 0x80) {
                out += static_cast<char>(b0);
                ++i;
                continue;
            }
            size_t len = 0;
            unsigned cp = 0;
            if ((b0 & 0xE0) == 0xC0) {
                len = 2;
                cp = b0 & 0x1Fu;
            } else if ((b0 & 0xF0) == 0xE0) {
                len = 3;
                cp = b0 & 0x0Fu;
            } else if ((b0 & 0xF8) == 0xF0) {
                len = 4;
                cp = b0 & 0x07u;
            }
            bool ok = b0 >= 0xC2 && b0 <= 0xF4 && i + len <= in.size();
            for (size_t k = 1; ok && k < len; ++k) {
                const unsigned char bk = static_cast<unsigned char>(in[i + k]);
                if ((bk & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
                cp = (cp << 6) | (bk & 0x3Fu);
            }
            if (ok && ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)))
                ok = false; // overlong encoding
            if (ok && (cp > 0x10FFFFu || (cp >= 0xD800 && cp <= 0xDFFF)))
                ok = false;
            if (ok) {
                out.append(in.substr(i, len));
                i += len;
            } else {
                out += "\xEF\xBF\xBD";
                ++i;
            }
        }
        return out;
    }

    // Forward declarations
    static std::expected<std::vector<InlineNode>, ParserError> parse_inline_node(cmark_node *node, int depth);
    static std::expected<std::vector<InlineNode>, ParserError> parse_inline_children(cmark_node *node, int depth);
    static std::expected<std::vector<ListEntry>, ParserError> parse_list_entries(cmark_node *list, int depth);
    static std::expected<DocNode, ParserError> parse_block_node(cmark_node *node);
    static std::expected<std::vector<DocNode>, ParserError> parse_document(cmark_node *root);

    // Get literal as string_view - points directly to cmark's internal storage
    static std::string_view get_literal(cmark_node *node) {
        const char *text = cmark_node_get_literal(node);
        return text ? std::string_view(text) : std::string_view();
    }

    // Normalize a link/image URL: trim leading whitespace and drop dangerous
    // protocols. data: URLs are only meaningful for images, not for links.
    static std::string sanitize_url(const char *url, bool allow_data) {
        if (!url)
            return {};
        std::string raw(url);
        const auto ws_start = raw.find_first_not_of(" \t\r\n");
        if (ws_start != std::string::npos)
            raw = raw.substr(ws_start);
        else
            raw.clear();

        std::string lower;
        lower.reserve(raw.size());
        for (char c: raw)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool dangerous = lower.starts_with("javascript:") || lower.starts_with("vbscript:") ||
                               (!allow_data && lower.starts_with("data:"));
        return dangerous ? std::string() : sanitize_utf8(raw);
    }

    // Flatten inline children into plain text (used for image alt text)
    static std::string flatten_alt_text(cmark_node *node, int depth) {
        auto children_result = parse_inline_children(node, depth + 1);
        if (!children_result)
            return {};
        std::string text;
        for (const auto &child: *children_result) {
            std::visit(
                    [&]<typename T>(const T &c) {
                        using U = std::decay_t<T>;
                        if constexpr (std::is_same_v<U, TextRun>)
                            text += c.text;
                        else if constexpr (std::is_same_v<U, Link>)
                            text += c.text;
                        else if constexpr (std::is_same_v<U, Image>)
                            text += c.alt;
                    },
                    child);
        }
        return text;
    }

    // Parse a single inline node (text, code, emphasis, strong, link)
    static std::expected<std::vector<InlineNode>, ParserError> parse_inline_node(cmark_node *node, int depth) {
        if (depth >= MAX_INLINE_DEPTH) {
            return std::unexpected(ParserError::MaxDepthExceeded);
        }

        // Extension node types have no public enum values; match by type name.
        if (std::strcmp(cmark_node_get_type_string(node), "strikethrough") == 0) {
            auto children_result = parse_inline_children(node, depth + 1);
            if (!children_result) [[unlikely]] {
                return std::unexpected(children_result.error());
            }
            for (auto &child: *children_result) {
                std::visit(
                        [&]<typename T>(T &c) {
                            if constexpr (std::is_same_v<std::decay_t<T>, TextRun>) {
                                c.strikethrough = true;
                            }
                        },
                        child);
            }
            return children_result.value();
        }

        switch (cmark_node_get_type(node)) {
            case CMARK_NODE_TEXT: {
                TextRun run;
                run.text = sanitize_utf8(get_literal(node)); // Allocate: text may be transient
                return std::vector<InlineNode>{run};
            }

            case CMARK_NODE_CODE: {
                TextRun run;
                run.text = sanitize_utf8(get_literal(node)); // Allocate
                run.code = true;
                return std::vector<InlineNode>{run};
            }

            case CMARK_NODE_SOFTBREAK:
                // cmark splits paragraph text at every source newline; without
                // an explicit joiner the fragments weld together ("foo␊bar"
                // rendered as "foobar"). A plain space keeps soft-wrapped
                // sources reading correctly; the view wraps naturally anyway.
                return std::vector<InlineNode>{TextRun{.text = " "}};

            case CMARK_NODE_LINEBREAK:
                // Trailing-double-space / backslash break: honour it literally.
                return std::vector<InlineNode>{TextRun{.text = "\n"}};

            case CMARK_NODE_EMPH: {
                auto children_result = parse_inline_children(node, depth + 1);
                if (!children_result) [[unlikely]] {
                    return std::unexpected(children_result.error());
                }
                auto &children = *children_result;
                for (auto &child: children) {
                    std::visit(
                            [&]<typename T>(T &c) {
                                if constexpr (std::is_same_v<std::decay_t<T>, TextRun>) {
                                    c.italic = true;
                                }
                            },
                            child);
                }
                return children;
            }

            case CMARK_NODE_STRONG: {
                auto children_result = parse_inline_children(node, depth + 1);
                if (!children_result) [[unlikely]] {
                    return std::unexpected(children_result.error());
                }
                auto &children = *children_result;
                for (auto &child: children) {
                    std::visit(
                            [&]<typename T>(T &c) {
                                if constexpr (std::is_same_v<std::decay_t<T>, TextRun>) {
                                    c.bold = true;
                                }
                            },
                            child);
                }
                return children;
            }

            case CMARK_NODE_LINK: {
                auto children_result = parse_inline_children(node, depth + 1);
                if (!children_result) [[unlikely]] {
                    return std::unexpected(children_result.error());
                }
                auto &children = *children_result;

                Link link;
                if (children.size() == 1 && std::holds_alternative<TextRun>(children[0])) {
                    link.text = std::get<TextRun>(children[0]).text; // Copy
                } else {
                    std::string text;
                    for (const auto &child: children) {
                        std::visit(
                                [&]<typename T>(const T &c) {
                                    if constexpr (std::is_same_v<std::decay_t<T>, TextRun>) {
                                        text += c.text;
                                    }
                                },
                                child);
                    }
                    link.text = std::move(text); // Move
                }

                link.url = sanitize_url(cmark_node_get_url(node), /*allow_data=*/false);
                return std::vector<InlineNode>{link};
            }

            case CMARK_NODE_IMAGE: {
                Image image;
                image.alt = flatten_alt_text(node, depth);
                image.url = sanitize_url(cmark_node_get_url(node), /*allow_data=*/true);
                return std::vector<InlineNode>{image};
            }

            case CMARK_NODE_HTML_INLINE:
            default: {
                TextRun run;
                run.text = std::string(get_literal(node)); // Allocate
                return std::vector<InlineNode>{run};
            }
        }
    }

    // Parse all inline children of a node
    static std::expected<std::vector<InlineNode>, ParserError> parse_inline_children(cmark_node *node, int depth) {
        std::vector<InlineNode> result;
        cmark_node *child = cmark_node_first_child(node);

        while (child) {
            auto inl_result = parse_inline_node(child, depth + 1);
            if (!inl_result) [[unlikely]] {
                return std::unexpected(inl_result.error());
            }

            auto &inl = *inl_result;
            for (auto &inline_node: inl) {
                if (!result.empty() && std::holds_alternative<TextRun>(inline_node)) {
                    const TextRun &new_run = std::get<TextRun>(inline_node);
                    if (std::holds_alternative<TextRun>(result.back())) {
                        auto &last_run = std::get<TextRun>(result.back());
                        if (last_run.bold == new_run.bold && last_run.italic == new_run.italic &&
                            last_run.code == new_run.code && last_run.strikethrough == new_run.strikethrough) {
                            last_run.text += new_run.text; // Concatenate adjacent runs
                            continue;
                        }
                    }
                    result.push_back(std::move(inline_node));
                } else {
                    result.push_back(std::move(inline_node));
                }
            }
            child = cmark_node_next(child);
        }
        return result;
    }

    // Get paragraph text for task list detection (concatenates all text nodes)
    static std::string get_paragraph_text(cmark_node *node) {
        std::string result;
        cmark_node *child = cmark_node_first_child(node);

        while (child) {
            if (cmark_node_get_type(child) == CMARK_NODE_TEXT) {
                std::string_view text = get_literal(child);
                result.append(text.data(), text.size());
            }
            child = cmark_node_next(child);
        }
        return result;
    }

    // Check if a node is a task list item and extract completion status
    static bool is_task_list_item(cmark_node *node, bool &completed) {
        cmark_node *first_child = cmark_node_first_child(node);
        if (!first_child || cmark_node_get_type(first_child) != CMARK_NODE_PARAGRAPH) {
            return false;
        }

        const std::string text = get_paragraph_text(first_child);
        if (text.length() < 4) {
            return false;
        }

        if (text[0] == '[' && (text[1] == ' ' || text[1] == 'x') && text[2] == ']') {
            completed = (text[1] == 'x');
            return true;
        }
        return false;
    }

    // With the tasklist extension active, "[ ]"/"[x]" items become extension nodes
    // (type string "tasklist") and the marker is stripped from the paragraph text.
    static bool is_extension_task_list_item(cmark_node *node, bool &completed) {
        const char *type_string = cmark_node_get_type_string(node);
        if (!type_string || std::strcmp(type_string, "tasklist") != 0) {
            return false;
        }
        completed = cmark_gfm_extensions_get_tasklist_item_checked(node);
        return true;
    }

    // Marker kind for a whole list: ordered, task (first item has a checkbox),
    // or plain bullet — same detection rules as before nesting support.
    static char classify_list_kind(cmark_node *list) {
        if (cmark_node_get_list_type(list) == CMARK_ORDERED_LIST)
            return 'o';
        cmark_node *first_item = cmark_node_first_child(list);
        if (first_item) {
            bool completed = false;
            if (is_extension_task_list_item(first_item, completed) || is_task_list_item(first_item, completed))
                return 't';
        }
        return 'u';
    }

    // Recursively convert a cmark LIST into ListEntry items, carrying nested
    // sublists in children instead of dropping them.
    static std::expected<std::vector<ListEntry>, ParserError> parse_list_entries(cmark_node *list, int depth) {
        if (depth >= MAX_LIST_DEPTH) [[unlikely]] {
            return std::unexpected(ParserError::MaxDepthExceeded);
        }
        const char kind = classify_list_kind(list);
        std::vector<ListEntry> entries;
        for (cmark_node *item = cmark_node_first_child(list); item; item = cmark_node_next(item)) {
            if (cmark_node_get_type(item) != CMARK_NODE_ITEM) {
                continue;
            }
            ListEntry entry;
            entry.kind = kind;

            bool completed = false;
            const bool via_extension = (kind == 't') && is_extension_task_list_item(item, completed);
            if (kind == 't') {
                if (!via_extension) {
                    is_task_list_item(item, completed);
                }
                entry.checked = completed;
            }

            cmark_node *inner = cmark_node_first_child(item);
            if (kind == 't' && !via_extension) {
                // Legacy path: skip the "[x]" text marker before the paragraph
                int skipped = 0;
                while (inner && skipped < 2) {
                    cmark_node_type t = cmark_node_get_type(inner);
                    if (t == CMARK_NODE_TEXT || t == CMARK_NODE_EMPH || t == CMARK_NODE_STRONG) {
                        skipped++;
                    }
                    inner = cmark_node_next(inner);
                }
            }
            while (inner) {
                if (cmark_node_get_type(inner) == CMARK_NODE_PARAGRAPH) {
                    auto content_result = parse_inline_children(inner, 0);
                    if (!content_result) [[unlikely]] {
                        return std::unexpected(content_result.error());
                    }
                    entry.content = std::move(*content_result);
                    break;
                }
                inner = cmark_node_next(inner);
            }

            for (cmark_node *child = cmark_node_first_child(item); child; child = cmark_node_next(child)) {
                if (cmark_node_get_type(child) == CMARK_NODE_LIST) {
                    auto sub = parse_list_entries(child, depth + 1);
                    if (!sub) [[unlikely]] {
                        return std::unexpected(sub.error());
                    }
                    entry.children.insert(entry.children.end(), std::make_move_iterator(sub->begin()),
                                          std::make_move_iterator(sub->end()));
                }
            }
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    // Parse a single block node (heading, paragraph, code block, etc.)
    static std::expected<DocNode, ParserError> parse_block_node(cmark_node *node) {
        // Extension node types have no public enum values; match by type name.
        if (std::strcmp(cmark_node_get_type_string(node), "table") == 0) {
            Table table;
            uint16_t n_columns = cmark_gfm_extensions_get_table_columns(node);
            const uint8_t *alignments = cmark_gfm_extensions_get_table_alignments(node);
            if (alignments)
                table.alignments.assign(alignments, alignments + n_columns);
            table.alignments.resize(n_columns, 'l');

            cmark_node *child = cmark_node_first_child(node);
            while (child) {
                const char *row_kind = cmark_node_get_type_string(child);
                const bool is_header_node = std::strcmp(row_kind, "table_header") == 0;
                if (is_header_node || std::strcmp(row_kind, "table_row") == 0) {
                    std::vector<TableCell> row_cells;
                    cmark_node *cell_child = cmark_node_first_child(child);
                    while (cell_child) {
                        if (std::strcmp(cmark_node_get_type_string(cell_child), "table_cell") == 0) {
                            auto content_result = parse_inline_children(cell_child, 0);
                            if (!content_result) [[unlikely]] {
                                return std::unexpected(content_result.error());
                            }
                            row_cells.push_back({std::move(*content_result)});
                        }
                        cell_child = cmark_node_next(cell_child);
                    }

                    if (is_header_node || cmark_gfm_extensions_get_table_row_is_header(child)) {
                        table.header = std::move(row_cells);
                    } else {
                        table.rows.push_back(std::move(row_cells));
                    }
                }
                child = cmark_node_next(child);
            }
            return table;
        }

        switch (cmark_node_get_type(node)) {
            case CMARK_NODE_HEADING: {
                Heading heading;
                heading.level = cmark_node_get_heading_level(node);
                auto content_result = parse_inline_children(node, 0);
                if (!content_result) [[unlikely]] {
                    return std::unexpected(content_result.error());
                }
                heading.content = std::move(*content_result);
                return heading;
            }

            case CMARK_NODE_PARAGRAPH: {
                Paragraph para;
                auto content_result = parse_inline_children(node, 0);
                if (!content_result) [[unlikely]] {
                    return std::unexpected(content_result.error());
                }
                para.content = std::move(*content_result);
                return para;
            }

            case CMARK_NODE_CODE_BLOCK: {
                CodeBlock code_block;
                const char *lang = cmark_node_get_fence_info(node);
                code_block.language = lang ? sanitize_utf8(lang) : std::string();
                code_block.code = sanitize_utf8(get_literal(node)); // Allocate code content
                return code_block;
            }

            case CMARK_NODE_BLOCK_QUOTE: {
                BlockQuote bq;
                if (cmark_node *first_child = cmark_node_first_child(node)) {
                    auto content_result = parse_inline_children(first_child, 0);
                    if (!content_result) [[unlikely]] {
                        return std::unexpected(content_result.error());
                    }
                    bq.content = std::move(*content_result);
                }
                return bq;
            }

            case CMARK_NODE_THEMATIC_BREAK: {
                return HRule{};
            }

            case CMARK_NODE_LIST: {
                List list;
                auto entries = parse_list_entries(node, 0);
                if (!entries) [[unlikely]] {
                    return std::unexpected(entries.error());
                }
                list.items = std::move(*entries);
                return list;
            }

            default: {
                Paragraph para;
                return para;
            }
        }
    }

    // Check if a code block is YAML front matter
    static bool is_yaml_front_matter(cmark_node *node) {
        if (cmark_node_get_type(node) != CMARK_NODE_CODE_BLOCK) {
            return false;
        }
        const char *lang = cmark_node_get_fence_info(node);
        return lang && std::string(lang) == "yaml";
    }

    // Check if a node is a thematic break
    static bool is_thematic_break(cmark_node *node) { return cmark_node_get_type(node) == CMARK_NODE_THEMATIC_BREAK; }

    // Parse all document nodes with YAML front matter detection
    static std::expected<std::vector<DocNode>, ParserError> parse_document(cmark_node *root) {
        std::vector<DocNode> result;
        cmark_node *child = cmark_node_first_child(root);

        // Buffer for potential front matter nodes
        std::vector<DocNode> front_matter_buffer;
        bool in_front_matter = false;
        bool first_thematic_break_seen = false;
        bool content_started = false;

        while (child) {
            // Check for YAML front matter (fenced code block with yaml language)
            if (is_yaml_front_matter(child)) {
                // Skip YAML front matter
                child = cmark_node_next(child);
                continue;
            }

            // Check for --- boundaries front matter
            if (is_thematic_break(child)) {
                if (!first_thematic_break_seen && !content_started) {
                    // First --- seen at start of document, start buffering
                    first_thematic_break_seen = true;
                    in_front_matter = true;
                    child = cmark_node_next(child);
                    continue;
                } else if (in_front_matter) {
                    // Second --- seen, end of front matter - discard buffer and skip this ---
                    first_thematic_break_seen = false;
                    in_front_matter = false;
                    front_matter_buffer.clear();
                    child = cmark_node_next(child);
                    continue;
                }
            }

            if (in_front_matter) {
                // Buffer nodes between --- boundaries
                auto block_result = parse_block_node(child);
                if (!block_result) [[unlikely]] {
                    return std::unexpected(block_result.error());
                }
                // Check if this is a setext heading (level=2) that represents YAML front matter
                // In this case, we should NOT buffer it, and we should exit front matter mode
                if (std::holds_alternative<Heading>(*block_result)) {
                    const auto &h = std::get<Heading>(*block_result);
                    if (h.level == 2) {
                        // This is a setext heading with --- underline, which means
                        // cmark has parsed the YAML front matter as a heading.
                        // Don't buffer it, and exit front matter mode.
                        in_front_matter = false;
                        first_thematic_break_seen = false;
                        // Don't add to buffer, just skip
                        child = cmark_node_next(child);
                        continue;
                    }
                }
                front_matter_buffer.push_back(std::move(*block_result));
                child = cmark_node_next(child);
                continue;
            }

            // Not in front matter - parse normally
            auto block_result = parse_block_node(child);
            if (!block_result) [[unlikely]] {
                return std::unexpected(block_result.error());
            }
            result.push_back(std::move(*block_result));
            content_started = true;
            child = cmark_node_next(child);
        }

        // If we were buffering but never saw a second ---, the buffer contains content
        // that should be handled specially. This happens when cmark parses YAML front matter
        // as a setext heading (the second --- is used as the heading underline).
        // Check if the first buffered node is a heading level=2 (setext heading with ---).
        // If so, assume it's YAML front matter and discard it.
        if (in_front_matter && first_thematic_break_seen && !front_matter_buffer.empty()) {
            bool is_yaml_front_matter = false;
            if (!front_matter_buffer.empty()) {
                const auto &first_node = front_matter_buffer.front();
                if (std::holds_alternative<Heading>(first_node)) {
                    const auto &h = std::get<Heading>(first_node);
                    // Heading level=2 indicates a setext heading with --- underline
                    // This is how cmark parses YAML front matter
                    is_yaml_front_matter = (h.level == 2);
                }
            }
            if (is_yaml_front_matter) {
                // Discard YAML front matter
                front_matter_buffer.clear();
            } else {
                // Not YAML front matter, render the content
                result.insert(result.end(), front_matter_buffer.begin(), front_matter_buffer.end());
            }
        }

        return result;
    }

    // Top-level parse function
    std::expected<DocModel, ParserError> parse_markdown(const std::string &markdown_text) {
        if (markdown_text.size() > MAX_INPUT_SIZE) {
            return std::unexpected(ParserError::InputTooLarge);
        }
        cmark_gfm_core_extensions_ensure_registered();

        cmark_parser *parser = cmark_parser_new(CMARK_OPT_DEFAULT);

        // Attach the GFM extensions we parse for. cmark_parse_document() does
        // NOT do this — it only knows the built-in CommonMark node types.
        static constexpr const char *kExtensions[] = {"table", "strikethrough", "tasklist", "autolink", "tagfilter"};
        for (const char *name: kExtensions) {
            if (cmark_syntax_extension *ext = cmark_find_syntax_extension(name)) {
                cmark_parser_attach_syntax_extension(parser, ext);
            }
        }

        cmark_parser_feed(parser, markdown_text.c_str(), markdown_text.size());
        cmark_node *root = cmark_parser_finish(parser);
        cmark_parser_free(parser);

        auto result = parse_document(root);
        cmark_node_free(root);

        if (!result) {
            return std::unexpected(result.error()); // preserve the real failure reason
        }
        return std::move(*result);
    }

} // namespace mdview

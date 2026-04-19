#include "mdview/parser.hpp"
#include <cmark-gfm.h>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include "cmark-gfm-extension_api.h"

namespace mdview {

    // Forward declarations
    static std::expected<std::vector<InlineNode>, ParserError> parse_inline_node(cmark_node *node);
    static std::expected<std::vector<InlineNode>, ParserError> parse_inline_children(cmark_node *node);
    static std::expected<DocNode, ParserError> parse_block_node(cmark_node *node);
    static std::expected<std::vector<DocNode>, ParserError> parse_document(cmark_node *root);

    // Get literal as string_view - points directly to cmark's internal storage
    static std::string_view get_literal(cmark_node *node) {
        const char *text = cmark_node_get_literal(node);
        return text ? std::string_view(text) : std::string_view();
    }

    // Parse a single inline node (text, code, emphasis, strong, link)
    static std::expected<std::vector<InlineNode>, ParserError> parse_inline_node(cmark_node *node) {
        switch (cmark_node_get_type(node)) {
            case CMARK_NODE_TEXT: {
                TextRun run;
                run.text = std::string(get_literal(node));  // Allocate: text may be transient
                return std::vector<InlineNode>{run};
            }

            case CMARK_NODE_CODE: {
                TextRun run;
                run.text = std::string(get_literal(node));  // Allocate
                run.code = true;
                return std::vector<InlineNode>{run};
            }

            case CMARK_NODE_EMPH: {
                auto children_result = parse_inline_children(node);
                if (!children_result) {
                    return std::unexpected(children_result.error());
                }
                auto &children = *children_result;
                for (auto &child : children) {
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
                auto children_result = parse_inline_children(node);
                if (!children_result) {
                    return std::unexpected(children_result.error());
                }
                auto &children = *children_result;
                for (auto &child : children) {
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
                auto children_result = parse_inline_children(node);
                if (!children_result) {
                    return std::unexpected(children_result.error());
                }
                auto &children = *children_result;

                Link link;
                if (children.size() == 1 && std::holds_alternative<TextRun>(children[0])) {
                    link.text = std::get<TextRun>(children[0]).text;  // Copy
                } else {
                    std::string text;
                    for (const auto &child : children) {
                        std::visit(
                            [&]<typename T>(const T &c) {
                                if constexpr (std::is_same_v<std::decay_t<T>, TextRun>) {
                                    text += c.text;
                                }
                            },
                            child);
                    }
                    link.text = std::move(text);  // Move
                }

                const char *url = cmark_node_get_url(node);
                link.url = url ? std::string(url) : std::string();  // Allocate URL
                return std::vector<InlineNode>{link};
            }

            case CMARK_NODE_IMAGE:
            case CMARK_NODE_HTML_INLINE:
            default: {
                TextRun run;
                run.text = std::string(get_literal(node));  // Allocate
                return std::vector<InlineNode>{run};
            }
        }
    }

    // Parse all inline children of a node
    static std::expected<std::vector<InlineNode>, ParserError> parse_inline_children(cmark_node *node) {
        std::vector<InlineNode> result;
        cmark_node *child = cmark_node_first_child(node);

        while (child) {
            auto inl_result = parse_inline_node(child);
            if (!inl_result) {
                return std::unexpected(inl_result.error());
            }

            auto &inl = *inl_result;
            for (auto &inline_node : inl) {
                if (!result.empty() && std::holds_alternative<TextRun>(inline_node)) {
                    const TextRun &new_run = std::get<TextRun>(inline_node);
                    if (std::holds_alternative<TextRun>(result.back())) {
                        auto &last_run = std::get<TextRun>(result.back());
                        if (last_run.bold == new_run.bold &&
                            last_run.italic == new_run.italic &&
                            last_run.code == new_run.code &&
                            last_run.strikethrough == new_run.strikethrough) {
                            last_run.text += new_run.text;  // Concatenate adjacent runs
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

    // Parse a single block node (heading, paragraph, code block, etc.)
    static std::expected<DocNode, ParserError> parse_block_node(cmark_node *node) {
        // Extension node types have no public enum values; match by type name.
        if (std::strcmp(cmark_node_get_type_string(node), "table") == 0) {
            Table table;
            uint16_t n_columns = cmark_gfm_extensions_get_table_columns(node);
            uint8_t *alignments = cmark_gfm_extensions_get_table_alignments(node);
            table.alignments.assign(alignments, alignments + n_columns);

            cmark_node *child = cmark_node_first_child(node);
            while (child) {
                const char *row_kind = cmark_node_get_type_string(child);
                const bool is_header_node = std::strcmp(row_kind, "table_header") == 0;
                if (is_header_node || std::strcmp(row_kind, "table_row") == 0) {
                    std::vector<TableCell> row_cells;
                    cmark_node *cell_child = cmark_node_first_child(child);
                    while (cell_child) {
                        if (std::strcmp(cmark_node_get_type_string(cell_child), "table_cell") == 0) {
                            auto content_result = parse_inline_children(cell_child);
                            if (!content_result) {
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
                auto content_result = parse_inline_children(node);
                if (!content_result) {
                    return std::unexpected(content_result.error());
                }
                heading.content = std::move(*content_result);
                return heading;
            }

            case CMARK_NODE_PARAGRAPH: {
                Paragraph para;
                auto content_result = parse_inline_children(node);
                if (!content_result) {
                    return std::unexpected(content_result.error());
                }
                para.content = std::move(*content_result);
                return para;
            }

            case CMARK_NODE_CODE_BLOCK: {
                CodeBlock code_block;
                const char *lang = cmark_node_get_fence_info(node);
                code_block.language = lang ? std::string(lang) : std::string();
                code_block.code = std::string(get_literal(node));  // Allocate code content
                return code_block;
            }

            case CMARK_NODE_BLOCK_QUOTE: {
                BlockQuote bq;
                if (cmark_node *first_child = cmark_node_first_child(node)) {
                    auto content_result = parse_inline_children(first_child);
                    if (!content_result) {
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
                cmark_list_type list_type = cmark_node_get_list_type(node);

                if (list_type == CMARK_BULLET_LIST) {
                    cmark_node *first_item = cmark_node_first_child(node);
                    bool is_task_list = false;
                    if (first_item) {
                        bool completed;
                        is_task_list = is_task_list_item(first_item, completed);
                    }

                    if (is_task_list) {
                        TaskList tlist;
                        cmark_node *item = cmark_node_first_child(node);

                        while (item) {
                            bool completed;
                            if (is_task_list_item(item, completed)) {
                                std::vector<InlineNode> item_content;
                                cmark_node *inner = cmark_node_first_child(item);
                                int skipped = 0;

                                while (inner && skipped < 2) {
                                    cmark_node_type t = cmark_node_get_type(inner);
                                    if (t == CMARK_NODE_TEXT || t == CMARK_NODE_EMPH || t == CMARK_NODE_STRONG) {
                                        skipped++;
                                    }
                                    inner = cmark_node_next(inner);
                                }

                                while (inner) {
                                    if (cmark_node_get_type(inner) == CMARK_NODE_PARAGRAPH) {
                                        auto content_result = parse_inline_children(inner);
                                        if (!content_result) {
                                            return std::unexpected(content_result.error());
                                        }
                                        item_content = std::move(*content_result);
                                        break;
                                    }
                                    inner = cmark_node_next(inner);
                                }
                                tlist.items.emplace_back(completed, std::move(item_content));
                            } else {
                                std::vector<InlineNode> item_content;
                                cmark_node *inner = cmark_node_first_child(item);

                                while (inner) {
                                    if (cmark_node_get_type(inner) == CMARK_NODE_PARAGRAPH) {
                                        auto content_result = parse_inline_children(inner);
                                        if (!content_result) {
                                            return std::unexpected(content_result.error());
                                        }
                                        item_content = std::move(*content_result);
                                        break;
                                    }
                                    inner = cmark_node_next(inner);
                                }
                                tlist.items.emplace_back(false, std::move(item_content));
                            }
                            item = cmark_node_next(item);
                        }
                        return tlist;
                    } else {
                        BulletList blist;
                        cmark_node *item = cmark_node_first_child(node);

                        while (item) {
                            std::vector<InlineNode> item_content;
                            cmark_node *inner = cmark_node_first_child(item);

                            while (inner) {
                                if (cmark_node_get_type(inner) == CMARK_NODE_PARAGRAPH) {
                                    auto content_result = parse_inline_children(inner);
                                    if (!content_result) {
                                        return std::unexpected(content_result.error());
                                    }
                                    item_content = std::move(*content_result);
                                    break;
                                }
                                inner = cmark_node_next(inner);
                            }
                            blist.items.push_back(std::move(item_content));
                            item = cmark_node_next(item);
                        }
                        return blist;
                    }
                } else if (list_type == CMARK_ORDERED_LIST) {
                    OrderedList olist;
                    cmark_node *item = cmark_node_first_child(node);

                    while (item) {
                        std::vector<InlineNode> item_content;
                        cmark_node *inner = cmark_node_first_child(item);

                        while (inner) {
                            if (cmark_node_get_type(inner) == CMARK_NODE_PARAGRAPH) {
                                auto content_result = parse_inline_children(inner);
                                if (!content_result) {
                                    return std::unexpected(content_result.error());
                                }
                                item_content = std::move(*content_result);
                                break;
                            }
                            inner = cmark_node_next(inner);
                        }
                        olist.items.push_back(std::move(item_content));
                        item = cmark_node_next(item);
                    }
                    return olist;
                }

                // Fallback for unknown list types
                Paragraph para;
                return para;
            }

            default: {
                Paragraph para;
                return para;
            }
        }
    }

    // Parse all document nodes
    static std::expected<std::vector<DocNode>, ParserError> parse_document(cmark_node *root) {
        std::vector<DocNode> result;
        cmark_node *child = cmark_node_first_child(root);

        while (child) {
            auto block_result = parse_block_node(child);
            if (!block_result) {
                return std::unexpected(block_result.error());
            }
            result.push_back(std::move(*block_result));
            child = cmark_node_next(child);
        }
        return result;
    }

    // Top-level parse function
    std::expected<DocModel, ParserError> parse_markdown(const std::string &markdown_text) {
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
            return std::unexpected<ParserError>(ParserError::InternalError);
        }
        return std::move(*result);
    }

} // namespace mdview

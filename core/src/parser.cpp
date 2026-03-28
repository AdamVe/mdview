#include "mdview/parser.hpp"
#include <cmark-gfm.h>
#include <string>
#include <vector>

namespace mdview {

static std::vector<InlineNode> parse_inline_node(cmark_node* node);
static std::vector<InlineNode> parse_inline_children(cmark_node* node);
static DocNode parse_block_node(cmark_node* node);
static std::vector<DocNode> parse_document(cmark_node* root);

static std::string get_literal(cmark_node* node) {
    const char* text = cmark_node_get_literal(node);
    return text ? std::string(text) : std::string();
}

static std::vector<InlineNode> parse_inline_node(cmark_node* node) {
    cmark_node_type type = cmark_node_get_type(node);

    switch (type) {
        case CMARK_NODE_TEXT: {
            TextRun run;
            run.text = get_literal(node);
            return {run};
        }

        case CMARK_NODE_CODE: {
            TextRun run;
            run.text = get_literal(node);
            run.code = true;
            return {run};
        }

        case CMARK_NODE_EMPH: {
            std::vector<InlineNode> children = parse_inline_children(node);
            for (auto& child : children) {
                std::visit([&](auto& c) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(c)>, TextRun>) {
                        c.italic = true;
                    }
                }, child);
            }
            return children;
        }

        case CMARK_NODE_STRONG: {
            std::vector<InlineNode> children = parse_inline_children(node);
            for (auto& child : children) {
                std::visit([&](auto& c) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(c)>, TextRun>) {
                        c.bold = true;
                    }
                }, child);
            }
            return children;
        }

        case CMARK_NODE_LINK: {
            Link link;
            std::vector<InlineNode> children = parse_inline_children(node);
            if (children.size() == 1 && std::holds_alternative<TextRun>(children[0])) {
                link.text = std::get<TextRun>(children[0]).text;
            } else {
                std::string text;
                for (const auto& child : children) {
                    std::visit([&](const auto& c) {
                        if constexpr (std::is_same_v<std::decay_t<decltype(c)>, TextRun>) {
                            text += c.text;
                        }
                    }, child);
                }
                link.text = text;
            }
            const char* url = cmark_node_get_url(node);
            link.url = url ? std::string(url) : std::string();
            return {link};
        }

        case CMARK_NODE_IMAGE:
        case CMARK_NODE_HTML_INLINE:
        default: {
            TextRun run;
            run.text = get_literal(node);
            return {run};
        }
    }
}

static std::vector<InlineNode> parse_inline_children(cmark_node* node) {
    std::vector<InlineNode> result;
    cmark_node* child = cmark_node_first_child(node);
    while (child) {
        std::vector<InlineNode> inl = parse_inline_node(child);
        for (auto& inline_node : inl) {
            if (!result.empty() && std::holds_alternative<TextRun>(inline_node)) {
                const TextRun& new_run = std::get<TextRun>(inline_node);
                if (std::holds_alternative<TextRun>(result.back())) {
                    auto& last_run = std::get<TextRun>(result.back());
                    if (last_run.bold == new_run.bold &&
                        last_run.italic == new_run.italic &&
                        last_run.code == new_run.code &&
                        last_run.strikethrough == new_run.strikethrough) {
                        last_run.text += new_run.text;
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

static std::string get_paragraph_text(cmark_node* node) {
    std::string result;
    cmark_node* child = cmark_node_first_child(node);
    while (child) {
        if (cmark_node_get_type(child) == CMARK_NODE_TEXT) {
            const char* text = cmark_node_get_literal(child);
            if (text) {
                result += text;
            }
        }
        child = cmark_node_next(child);
    }
    return result;
}

static bool is_task_list_item(cmark_node* node, bool& completed) {
    cmark_node* first_child = cmark_node_first_child(node);
    if (!first_child || cmark_node_get_type(first_child) != CMARK_NODE_PARAGRAPH) {
        return false;
    }

    std::string text = get_paragraph_text(first_child);
    if (text.length() < 4) {
        return false;
    }

    if (text[0] == '[' && (text[1] == ' ' || text[1] == 'x') && text[2] == ']') {
        completed = (text[1] == 'x');
        return true;
    }
    return false;
}

static DocNode parse_block_node(cmark_node* node) {
    cmark_node_type type = cmark_node_get_type(node);

    switch (type) {
        case CMARK_NODE_HEADING: {
            Heading heading;
            heading.level = cmark_node_get_heading_level(node);
            heading.content = parse_inline_children(node);
            return heading;
        }

        case CMARK_NODE_PARAGRAPH: {
            Paragraph para;
            para.content = parse_inline_children(node);
            return para;
        }

        case CMARK_NODE_CODE_BLOCK: {
            CodeBlock codeblk;
            const char* lang = cmark_node_get_fence_info(node);
            codeblk.language = lang ? std::string(lang) : std::string();
            codeblk.code = get_literal(node);
            return codeblk;
        }

        case CMARK_NODE_BLOCK_QUOTE: {
            BlockQuote bq;
            // BLOCK_QUOTE contains one or more block nodes (usually paragraphs)
            // We need to get the inline content from the first child paragraph
            cmark_node* first_child = cmark_node_first_child(node);
            if (first_child) {
                bq.content = parse_inline_children(first_child);
            }
            return bq;
        }

        case CMARK_NODE_THEMATIC_BREAK: {
            return HRule{};
        }

        case CMARK_NODE_LIST: {
            cmark_list_type list_type = cmark_node_get_list_type(node);
            if (list_type == CMARK_BULLET_LIST) {
                cmark_node* first_item = cmark_node_first_child(node);
                bool is_task_list = false;
                if (first_item) {
                    bool completed;
                    is_task_list = is_task_list_item(first_item, completed);
                }

                if (is_task_list) {
                    TaskList tlist;
                    cmark_node* item = cmark_node_first_child(node);
                    while (item) {
                        bool completed;
                        if (is_task_list_item(item, completed)) {
                            std::vector<InlineNode> item_content;
                            cmark_node* inner = cmark_node_first_child(item);
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
                                    item_content = parse_inline_children(inner);
                                    break;
                                }
                                inner = cmark_node_next(inner);
                            }
                            tlist.items.emplace_back(completed, std::move(item_content));
                        } else {
                            std::vector<InlineNode> item_content;
                            cmark_node* inner = cmark_node_first_child(item);
                            while (inner) {
                                if (cmark_node_get_type(inner) == CMARK_NODE_PARAGRAPH) {
                                    item_content = parse_inline_children(inner);
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
                    cmark_node* item = cmark_node_first_child(node);
                    while (item) {
                        std::vector<InlineNode> item_content;
                        cmark_node* inner = cmark_node_first_child(item);
                        while (inner) {
                            if (cmark_node_get_type(inner) == CMARK_NODE_PARAGRAPH) {
                                item_content = parse_inline_children(inner);
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
                cmark_node* item = cmark_node_first_child(node);
                while (item) {
                    std::vector<InlineNode> item_content;
                    cmark_node* inner = cmark_node_first_child(item);
                    while (inner) {
                        if (cmark_node_get_type(inner) == CMARK_NODE_PARAGRAPH) {
                            item_content = parse_inline_children(inner);
                            break;
                        }
                        inner = cmark_node_next(inner);
                    }
                    olist.items.push_back(std::move(item_content));
                    item = cmark_node_next(item);
                }
                return olist;
            }
            Paragraph para;
            return para;
        }

        default: {
            Paragraph para;
            return para;
        }
    }
}

static std::vector<DocNode> parse_document(cmark_node* root) {
    std::vector<DocNode> result;
    cmark_node* child = cmark_node_first_child(root);
    while (child) {
        DocNode block = parse_block_node(child);
        result.push_back(std::move(block));
        child = cmark_node_next(child);
    }
    return result;
}

DocModel parse_markdown(const std::string& markdown_text) {
    cmark_node* root = cmark_parse_document(markdown_text.c_str(), markdown_text.size(), 0);
    DocModel model = parse_document(root);
    cmark_node_free(root);
    return model;
}

} // namespace mdview

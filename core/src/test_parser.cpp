#include <fstream>
#include <iostream>
#include <print>
#include <ranges>
#include <sstream>
#include "mdview/doc_model.hpp"
#include "mdview/parser.hpp"

class DocModelPrinter {
public:
    void print(const mdview::DocModel &model) {
        for (size_t i = 0; i < model.size(); ++i) {
            if (i > 0)
                std::print("\n");
            std::visit([this](const auto &node) { print_node(node); }, model[i]);
        }
    }

private:
    void print_node(const mdview::Heading &h) {
        std::print("Heading level={}: ", h.level);
        print_inline_vector(h.content);
    }

    void print_node(const mdview::Paragraph &p) {
        std::print("Paragraph: ");
        print_inline_vector(p.content);
    }

    void print_node(const mdview::CodeBlock &cb) {
        std::print("CodeBlock language=\"{}\": {}", cb.language, cb.code);
    }

    void print_node(const mdview::BlockQuote &bq) {
        std::print("BlockQuote: ");
        print_inline_vector(bq.content);
    }

    void print_node(const mdview::HRule &) { std::print("HRule"); }

    void print_node(const mdview::BulletList &bl) {
        std::print("BulletList with {} items:", bl.items.size());
        for (const auto &[i, item]: bl.items | std::views::enumerate) {
            std::print("\n  [{}] ", i + 1);
            print_inline_vector(item);
        }
    }

    void print_node(const mdview::OrderedList &ol) {
        std::print("OrderedList with {} items:", ol.items.size());
        for (const auto &[i, item]: ol.items | std::views::enumerate) {
            std::print("\n  {}. ", i + 1);
            print_inline_vector(item);
        }
    }

    void print_node(const mdview::TaskList &tl) {
        std::print("TaskList with {} items:", tl.items.size());
        for (const auto &[i, item]: tl.items | std::views::enumerate) {
            const char marker = item.completed ? 'x' : ' ';
            std::print("\n  [{}] ", marker);
            print_inline_vector(item.content);
        }
    }

    void print_node(const mdview::Table &t) {
        std::print("Table with {} columns, {} header rows, {} body rows",
                   t.alignments.size(), t.header.size(), t.rows.size());
        for (size_t c = 0; c < t.alignments.size(); ++c) {
            std::print("\n  col {}: align='{}'", c, t.alignments[c]);
        }
        for (size_t r = 0; r < t.header.size(); ++r) {
            std::print("\n  header[{}]: ", r);
            print_inline_vector(t.header[r].content);
        }
        for (size_t r = 0; r < t.rows.size(); ++r) {
            std::print("\n  row[{}]: ", r);
            for (size_t c = 0; c < t.rows[r].size(); ++c) {
                if (c > 0) std::print(" | ");
                print_inline_vector(t.rows[r][c].content);
            }
        }
    }

    void print_inline_vector(const std::vector<mdview::InlineNode> &nodes) {
        for (const auto &node: nodes) {
            std::visit([this](const auto &n) { print_inline(n); }, node);
        }
    }

    void print_inline(const mdview::TextRun &tr) {
        std::string s;
        if (tr.code)
            s += "`";
        if (tr.bold)
            s += "**";
        if (tr.italic)
            s += "*";
        if (tr.strikethrough)
            s += "~~";
        s += tr.text;
        if (tr.strikethrough)
            s += "~~";
        if (tr.italic)
            s += "*";
        if (tr.bold)
            s += "**";
        if (tr.code)
            s += "`";
        std::print("{}", s);
    }

    void print_inline(const mdview::Link &l) { std::print("[{}]({})", l.text, l.url); }
};

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::print("Usage: {} <markdown_file>\n", argv[0]);
        return 1;
    }

    std::ifstream file(argv[1]);
    if (!file) {
        std::print("Error: Cannot open file {}\n", argv[1]);
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    auto result = mdview::parse_markdown(content);
    if (!result) {
        std::print("Error: Failed to parse markdown\n");
        return 1;
    }
    mdview::DocModel model = std::move(*result);
    DocModelPrinter printer;
    printer.print(model);

    return 0;
}

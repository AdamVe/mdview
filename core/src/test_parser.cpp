#include <fstream>
#include <iostream>
#include <sstream>
#include "mdview/doc_model.hpp"
#include "mdview/parser.hpp"

class DocModelPrinter {
public:
    void print(const mdview::DocModel &model) {
        for (size_t i = 0; i < model.size(); ++i) {
            if (i > 0)
                std::cout << "\n";
            std::visit([this](const auto &node) { print_node(node); }, model[i]);
        }
    }

private:
    void print_node(const mdview::Heading &h) {
        std::cout << "Heading level=" << h.level << ": ";
        print_inline_vector(h.content);
    }

    void print_node(const mdview::Paragraph &p) {
        std::cout << "Paragraph: ";
        print_inline_vector(p.content);
    }

    void print_node(const mdview::CodeBlock &cb) {
        std::cout << "CodeBlock language=\"" << cb.language << "\": " << cb.code;
    }

    void print_node(const mdview::BlockQuote &bq) {
        std::cout << "BlockQuote: ";
        print_inline_vector(bq.content);
    }

    void print_node(const mdview::HRule &) { std::cout << "HRule"; }

    void print_node(const mdview::BulletList &bl) {
        std::cout << "BulletList with " << bl.items.size() << " items:";
        for (size_t i = 0; i < bl.items.size(); ++i) {
            std::cout << "\n  [" << (i + 1) << "] ";
            print_inline_vector(bl.items[i]);
        }
    }

    void print_node(const mdview::OrderedList &ol) {
        std::cout << "OrderedList with " << ol.items.size() << " items:";
        for (size_t i = 0; i < ol.items.size(); ++i) {
            std::cout << "\n  " << (i + 1) << ". ";
            print_inline_vector(ol.items[i]);
        }
    }

    void print_node(const mdview::TaskList &tl) {
        std::cout << "TaskList with " << tl.items.size() << " items:";
        for (const auto &[fst, snd]: tl.items) {
            const char marker = fst ? 'x' : ' ';
            std::cout << "\n  [" << marker << "] ";
            print_inline_vector(snd);
        }
    }

    void print_inline_vector(const std::vector<mdview::InlineNode> &nodes) {
        for (const auto &node: nodes) {
            std::visit([this](const auto &n) { print_inline(n); }, node);
        }
    }

    void print_inline(const mdview::TextRun &tr) {
        if (tr.code)
            std::cout << "`";
        if (tr.bold)
            std::cout << "**";
        if (tr.italic)
            std::cout << "*";
        if (tr.strikethrough)
            std::cout << "~~";
        std::cout << tr.text;
        if (tr.strikethrough)
            std::cout << "~~";
        if (tr.italic)
            std::cout << "*";
        if (tr.bold)
            std::cout << "**";
        if (tr.code)
            std::cout << "`";
    }

    void print_inline(const mdview::Link &l) { std::cout << "[" << l.text << "](" << l.url << ")"; }
};

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <markdown_file>\n";
        return 1;
    }

    std::ifstream file(argv[1]);
    if (!file) {
        std::cerr << "Error: Cannot open file " << argv[1] << "\n";
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    mdview::DocModel model = mdview::parse_markdown(content);
    DocModelPrinter printer;
    printer.print(model);

    return 0;
}

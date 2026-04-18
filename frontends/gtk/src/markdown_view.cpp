#include "markdown_view.hpp"
#include <fstream>
#include <gtkmm.h>
#include <iostream>
#include <sstream>
#include "mdview/parser.hpp"
#ifdef USE_ADWAITA
#include <adwaita.h>
#endif

extern "C" {
#include "mdview-resources.h"
}

MarkdownView::MarkdownView() {
    set_expand(true);

    // Load CSS from resource
    const auto css_provider = Gtk::CssProvider::create();
    try {
        GResource *resource = mdview_get_resource();
        g_resource_ref(resource);
        GError *error = nullptr;
        if (GBytes *bytes = g_resource_lookup_data(resource, "/com/adamve/mdview/style.css",
                                                   G_RESOURCE_LOOKUP_FLAGS_NONE, &error)) {
            gsize size = 0;
            if (const auto data = static_cast<const char *>(g_bytes_get_data(bytes, &size)); data && size > 0) {
                const std::string css_data(data, size);
                css_provider->load_from_data(css_data);
            }
            g_bytes_unref(bytes);
        }
        if (error) {
            g_error_free(error);
        }
    } catch (...) {
    }

    // Apply CSS provider to text view
    get_style_context()->add_provider_for_display(Gdk::Display::get_default(), css_provider,
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Create text buffer for markdown content
    m_tag_table = Gtk::TextTagTable::create();
    m_buffer = Gtk::TextBuffer::create(m_tag_table);
    setup_tags();

    set_buffer(m_buffer);
    set_editable(false);
    set_cursor_visible(false);

    // Enable word wrapping
    set_wrap_mode(Gtk::WrapMode::WORD_CHAR);

    add_css_class("markdown-view");

    setup_color_detection();
}

MarkdownView::~MarkdownView() = default;

void MarkdownView::setup_tags() {
    // Heading tags
    add_tag("h1", "scale=2.0;weight=bold;pixels-above=12;pixels-below=6;");
    add_tag("h2", "scale=1.6;weight=bold;pixels-above=10;pixels-below=4;");
    add_tag("h3", "scale=1.3;weight=bold;pixels-above=8;pixels-below=4;");
    add_tag("h4", "scale=1.1;weight=bold;");

    // Inline tags
    add_tag("bold", "weight=bold;");
    add_tag("italic", "style=italic;");
    add_tag("strikethrough", "strikethrough=true;");
    add_tag("inline_code", "family=monospace;");
    add_tag("code_block", "family=monospace;");

    // Block tags
    add_tag("blockquote", "indent=24;left-margin=24;style=italic;");
    add_tag("link", "foreground=blue;underline=single;");
    add_tag("hr", "");
    add_tag("bullet", "indent=24;left-margin=24;");
    add_tag("search_highlight", "background=yellow;");
}

void MarkdownView::add_tag(const Glib::ustring &name, const Glib::ustring & /*properties*/) const {
    const auto tag = Gtk::TextTag::create(name);
    if (name == "h1") {
        tag->property_scale() = 2.0;
        tag->property_weight() = Pango::Weight::BOLD;
        tag->property_pixels_above_lines() = 12;
        tag->property_pixels_below_lines() = 6;
    } else if (name == "h2") {
        tag->property_scale() = 1.6;
        tag->property_weight() = Pango::Weight::BOLD;
        tag->property_pixels_above_lines() = 10;
        tag->property_pixels_below_lines() = 4;
    } else if (name == "h3") {
        tag->property_scale() = 1.3;
        tag->property_weight() = Pango::Weight::BOLD;
        tag->property_pixels_above_lines() = 8;
        tag->property_pixels_below_lines() = 4;
    } else if (name == "h4") {
        tag->property_scale() = 1.1;
        tag->property_weight() = Pango::Weight::BOLD;
    } else if (name == "bold") {
        tag->property_weight() = Pango::Weight::BOLD;
    } else if (name == "italic") {
        tag->property_style() = Pango::Style::ITALIC;
    } else if (name == "strikethrough") {
        tag->property_strikethrough() = true;
    } else if (name == "inline_code") {
        tag->property_family() = "monospace";
    } else if (name == "code_block") {
        tag->property_family() = "monospace";
        tag->property_left_margin() = 16;
        tag->property_pixels_above_lines() = 8;
        tag->property_pixels_below_lines() = 8;
    } else if (name == "blockquote") {
        tag->property_left_margin() = 24;
        tag->property_indent() = 24;
        tag->property_style() = Pango::Style::ITALIC;
        tag->property_foreground() = "#888888";
    } else if (name == "link") {
        tag->property_foreground() = "#0000ff";
        tag->property_underline() = Pango::Underline::SINGLE;
    } else if (name == "hr") {
    } else if (name == "bullet") {
        tag->property_left_margin() = 24;
        tag->property_indent() = 24;
    } else if (name == "search_highlight") {
        tag->property_background() = "yellow";
    }
    m_tag_table->add(tag);
}

void MarkdownView::load(const mdview::DocModel &model) {
    m_buffer->set_text("");
    auto iter = m_buffer->end();

    for (const auto &node: model) {
        std::visit([this, &iter](const auto &n) { insert_node(n, iter); }, node);
    }

    m_has_content = !model.empty();
}

void MarkdownView::insert_node(const mdview::Heading &h, Gtk::TextIter &iter) {
    auto tag = m_tag_table->lookup("h" + std::to_string(std::min(h.level, 4)));
    if (!tag)
        tag = m_tag_table->lookup("h4");

    for (const auto &inline_node: h.content) {
        std::visit([this, &iter, &tag](const auto &inl) { insert_inline(inl, iter, tag); }, inline_node);
    }
    iter = m_buffer->insert(iter, "\n");
}

void MarkdownView::insert_node(const mdview::Paragraph &p, Gtk::TextIter &iter) {
    for (const auto &inline_node: p.content) {
        std::visit([this, &iter](const auto &inl) { insert_inline(inl, iter); }, inline_node);
    }
    iter = m_buffer->insert(iter, "\n");
}

void MarkdownView::insert_node(const mdview::CodeBlock &cb, Gtk::TextIter &iter) const {
    const auto tag = m_tag_table->lookup("code_block");

    if (!cb.language.empty()) {
        if (const auto lang_tag = m_tag_table->lookup("inline_code")) {
            iter = m_buffer->insert_with_tags(iter, Glib::ustring(cb.language), {lang_tag});
        } else {
            iter = m_buffer->insert(iter, cb.language);
        }
        iter = m_buffer->insert(iter, "\n");
    }

    iter = m_buffer->insert_with_tags(iter, Glib::ustring(cb.code), {tag});
    iter = m_buffer->insert(iter, "\n");
    iter = m_buffer->insert(iter, "\n");
}

void MarkdownView::insert_node(const mdview::BlockQuote &bq, Gtk::TextIter &iter) {
    auto tag = m_tag_table->lookup("blockquote");

    for (const auto &inline_node: bq.content) {
        std::visit([this, &iter, &tag](const auto &inl) { insert_inline(inl, iter, tag); }, inline_node);
    }
    iter = m_buffer->insert(iter, "\n");
}

void MarkdownView::insert_node(const mdview::HRule & /*hr*/, Gtk::TextIter &iter) const {
    const auto tag = m_tag_table->lookup("hr");
    iter = m_buffer->insert_with_tags(iter, Glib::ustring("— — —"), {tag});
    iter = m_buffer->insert(iter, "\n");
}

void MarkdownView::insert_node(const mdview::BulletList &bl, Gtk::TextIter &iter) {
    const auto tag = m_tag_table->lookup("bullet");

    for (const auto &item: bl.items) {
        iter = m_buffer->insert_with_tags(iter, Glib::ustring("• "), {tag});

        for (const auto &inline_node: item) {
            std::visit([this, &iter, &tag](const auto &inl) { insert_inline(inl, iter, tag); }, inline_node);
        }
        iter = m_buffer->insert(iter, "\n");
    }
}

void MarkdownView::insert_node(const mdview::OrderedList &ol, Gtk::TextIter &iter) {
    const auto tag = m_tag_table->lookup("bullet");

    int num = 1;
    for (const auto &item: ol.items) {
        Glib::ustring number = std::to_string(num++) + ". ";
        iter = m_buffer->insert(iter, number);

        for (const auto &inline_node: item) {
            std::visit([this, &iter, &tag](const auto &inl) { insert_inline(inl, iter, tag); }, inline_node);
        }
        iter = m_buffer->insert(iter, "\n");
    }
}

void MarkdownView::insert_node(const mdview::TaskList &tl, Gtk::TextIter &iter) {
    for (const auto &item : tl.items) {
        const char marker = item.completed ? 'x' : ' ';
        iter = m_buffer->insert(iter, "[");
        iter = m_buffer->insert(iter, Glib::ustring(1, marker));
        iter = m_buffer->insert(iter, "] ");

        for (const auto &inline_node : item.content) {
            std::visit([this, &iter](const auto &inl) { insert_inline(inl, iter); }, inline_node);
        }
        iter = m_buffer->insert(iter, "\n");
    }
}

void MarkdownView::insert_inline(const mdview::TextRun &tr, Gtk::TextIter &iter, Glib::RefPtr<Gtk::TextTag> tag) const {
    Glib::ustring text = tr.text;
    if (text.empty()) return;

    // Collect all applicable tags to apply atomically via insert_with_tags
    std::vector<Glib::RefPtr<Gtk::TextTag>> tags;
    if (tag) tags.push_back(tag);
    if (tr.code) {
        if (auto code_tag = m_tag_table->lookup("inline_code")) tags.push_back(code_tag);
    }
    if (tr.bold) {
        if (auto bold_tag = m_tag_table->lookup("bold")) tags.push_back(bold_tag);
    }
    if (tr.italic) {
        if (auto italic_tag = m_tag_table->lookup("italic")) tags.push_back(italic_tag);
    }
    if (tr.strikethrough) {
        if (auto strike_tag = m_tag_table->lookup("strikethrough")) tags.push_back(strike_tag);
    }

    if (tags.empty()) {
        iter = m_buffer->insert(iter, text);
    } else {
        iter = m_buffer->insert_with_tags(iter, text, tags);
    }
}

void MarkdownView::insert_inline(const mdview::Link &l, Gtk::TextIter &iter,
                                 const Glib::RefPtr<Gtk::TextTag> &tag) const {
    insert_inline(mdview::TextRun{l.text}, iter, tag);
}

void MarkdownView::setup_color_detection() {
    auto settings = Gtk::Settings::get_default();
    if (settings) {
        Glib::ustring theme;
        settings->get_property("gtk-theme-name", theme);
        m_dark_mode = theme.find("dark") != Glib::ustring::npos ||
                      theme.find("Dark") != Glib::ustring::npos;
    }
}

void MarkdownView::load_file(const Glib::ustring &path) {
    std::ifstream file(path);
    if (!file) {
        m_buffer->set_text("Error: Cannot open file\n" + path);
        m_has_content = false;
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    auto result = mdview::parse_markdown(content);
    if (!result) {
        m_buffer->set_text("Error: Failed to parse markdown document.");
        m_has_content = false;
        return;
    }
    load(std::move(*result));
}

void MarkdownView::update_colors() const { update_code_backgrounds(); }

void MarkdownView::update_code_backgrounds() const {
    Glib::ustring bg_color;
    if (m_dark_mode) {
        bg_color = "#2a2a2a";
    } else {
        bg_color = "#f0f0f0";
    }

    if (const auto inline_code_tag = m_tag_table->lookup("inline_code")) {
        inline_code_tag->property_background() = bg_color;
    }

    if (const auto code_block_tag = m_tag_table->lookup("code_block")) {
        code_block_tag->property_background() = bg_color;
    }
}

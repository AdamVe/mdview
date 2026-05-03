#include "markdown_view.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
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

    // Create text buffer for markdown content
    m_tag_table = Gtk::TextTagTable::create();
    m_buffer = Gtk::TextBuffer::create(m_tag_table);
    setup_tags();

    set_buffer(m_buffer);
    set_editable(false);
    set_cursor_visible(false);

    // Enable word wrapping (table_cell tag overrides this to NONE)
    set_wrap_mode(Gtk::WrapMode::WORD_CHAR);

    add_css_class("markdown-view");

    // Apply base CSS to text view
    get_style_context()->add_provider_for_display(Gdk::Display::get_default(), css_provider,
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    setup_color_detection();
    setup_search_colors();

    m_zoom_tag = m_tag_table->lookup("zoom");
    apply_zoom();
}

void MarkdownView::set_zoom(float zoom) {
    m_zoom = std::clamp(zoom, 0.5f, 1.9f);
    apply_zoom();
}

float MarkdownView::get_zoom() const { return m_zoom; }

void MarkdownView::apply_zoom() {
    if (m_zoom_tag) {
        m_zoom_tag->property_scale() = m_zoom;
    }

    // Re-apply zoom tag to all text in the buffer
    if (m_buffer && m_has_content) {
        auto start = m_buffer->begin();
        auto end = m_buffer->end();
        m_buffer->remove_tag(m_zoom_tag, start, end);
        m_buffer->apply_tag(m_zoom_tag, start, end);
    }
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
    add_tag("inline_code", "family=monospace;scale=1.1;");
    add_tag("code_block", "family=monospace;line-height=1;scale=1.1;");
    add_tag("code_block_padding_top", "family=monospace;line-height=1;");
    add_tag("code_block_padding_bottom", "family=monospace;line-height=1;");
    add_tag("code_block_language", "family=monospace;weight=bold;scale=0.8;");

    // Block tags
    add_tag("blockquote", "indent=24;left-margin=24;style=italic;");
    add_tag("link", "foreground=blue;underline=single;");
    add_tag("hr", "wrap-mode=none;");
    add_tag("bullet", "indent=24;left-margin=24;");
    add_tag("search_highlight", "background=#ffff00;");
    add_tag("search_highlight_current", "background=#ff9500;");
    add_tag("table_cell", "line-height=1;");

    // Zoom tag — scale applied to all text for per-window zoom
    add_tag("zoom", "");
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
        tag->property_scale() = 1.1;
    } else if (name == "code_block") {
        tag->property_family() = "monospace";
        tag->property_scale() = 1.1;
        tag->property_left_margin() = 0;
        tag->property_pixels_above_lines() = 0;
        tag->property_pixels_below_lines() = 0;
        tag->property_line_height() = 1.0;
        tag->property_wrap_mode() = Gtk::WrapMode::NONE;
    } else if (name == "code_block_padding_top") {
        tag->property_family() = "monospace";
        tag->property_scale() = 1.1;
        tag->property_left_margin() = 0;
        tag->property_pixels_above_lines() = 4;
        tag->property_pixels_below_lines() = 0;
        tag->property_line_height() = 1.0;
        tag->property_wrap_mode() = Gtk::WrapMode::NONE;
    } else if (name == "code_block_padding_bottom") {
        tag->property_family() = "monospace";
        tag->property_scale() = 1.1;
        tag->property_left_margin() = 0;
        tag->property_pixels_above_lines() = 0;
        tag->property_pixels_below_lines() = 8;
        tag->property_line_height() = 1.0;
        tag->property_wrap_mode() = Gtk::WrapMode::NONE;
    } else if (name == "code_block_language") {
        tag->property_family() = "monospace";
        tag->property_weight() = Pango::Weight::BOLD;
        tag->property_scale() = 0.8;
    } else if (name == "table_cell") {
        tag->property_family() = "monospace";
        tag->property_line_height() = 1.0;
        tag->property_wrap_mode() = Gtk::WrapMode::NONE;
    } else if (name == "blockquote") {
        tag->property_left_margin() = 24;
        tag->property_indent() = 24;
        tag->property_style() = Pango::Style::ITALIC;
        tag->property_foreground() = "#888888";
    } else if (name == "link") {
        tag->property_foreground() = "#0000ff";
        tag->property_underline() = Pango::Underline::SINGLE;
    } else if (name == "hr") {
        tag->property_wrap_mode() = Gtk::WrapMode::NONE;
    } else if (name == "bullet") {
        tag->property_left_margin() = 24;
        tag->property_indent() = 24;
    } else if (name == "search_highlight") {
        tag->property_background() = "#ffff00";
    } else if (name == "search_highlight_current") {
        tag->property_background() = "#ff9500";
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
    const auto code_tag = m_tag_table->lookup("code_block");
    const auto top_tag = m_tag_table->lookup("code_block_padding_top");
    const auto bottom_tag = m_tag_table->lookup("code_block_padding_bottom");
    const auto lang_tag = m_tag_table->lookup("code_block_language");

    if (!cb.language.empty()) {
        const auto padding_tag = m_tag_table->lookup("code_block_padding_top");
        if (padding_tag) {
            iter = m_buffer->insert_with_tags(iter, Glib::ustring(" "), {padding_tag});
        }
        if (lang_tag) {
            iter = m_buffer->insert_with_tags(iter, Glib::ustring(cb.language), {lang_tag});
        } else {
            iter = m_buffer->insert(iter, cb.language);
        }
        iter = m_buffer->insert(iter, "\n");
    }

    // Split code into lines to add padding
    std::vector<std::string> lines;
    std::stringstream ss(cb.code);
    std::string line;
    while (std::getline(ss, line)) {
        lines.push_back(line);
    }

    // Insert code lines with left padding (leading space)
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto &l = lines[i];
        auto tag = code_tag;
        if (i == 0) {
            tag = top_tag;  // First line gets top padding
        } else if (i == lines.size() - 1) {
            tag = bottom_tag;  // Last line gets bottom padding
        }
        if (l.empty()) {
            iter = m_buffer->insert_with_tags(iter, Glib::ustring("  "), {tag});
        } else {
            iter = m_buffer->insert_with_tags(iter, Glib::ustring("  " + l), {tag});
        }
        iter = m_buffer->insert(iter, "\n");
    }
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
    iter = m_buffer->insert_with_tags(iter, Glib::ustring("──────────────────────────────"), {tag});
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
    for (const auto &item: tl.items) {
        const char marker = item.completed ? 'x' : ' ';
        iter = m_buffer->insert(iter, "[");
        iter = m_buffer->insert(iter, Glib::ustring(1, marker));
        iter = m_buffer->insert(iter, "] ");

        for (const auto &inline_node: item.content) {
            std::visit([this, &iter](const auto &inl) { insert_inline(inl, iter); }, inline_node);
        }
        iter = m_buffer->insert(iter, "\n");
    }
}

void MarkdownView::insert_node(const mdview::Table &t, Gtk::TextIter &iter) {
    const auto table_cell_tag = m_tag_table->lookup("table_cell");

    // UTF-8 box-drawing constants
    static constexpr const char *kTopLeft = "┌";
    static constexpr const char *kTopRight = "┐";
    static constexpr const char *kBottomLeft = "└";
    static constexpr const char *kBottomRight = "┘";
    static constexpr const char *kTJunction = "┬";
    static constexpr const char *kPlus = "┼";
    static constexpr const char *kBJunction = "┴";
    static constexpr const char *kHLine = "─";
    static constexpr const char *kVLine = "│";
    static constexpr const char *kSepLeft = "├";
    static constexpr const char *kSepRight = "┤";

    // Repeat a UTF-8 string n times
    auto repeat = [](const char *s, size_t n) -> std::string {
        std::string result;
        result.reserve(std::strlen(s) * n);
        for (size_t i = 0; i < n; ++i) {
            result += s;
        }
        return result;
    };

    // Extract plain text from inline nodes for width computation
    auto extract_text = [](const std::vector<mdview::InlineNode> &nodes) -> std::string {
        std::string result;
        for (const auto &node: nodes) {
            std::visit(
                    [&result](const auto &n) {
                        if constexpr (std::is_same_v<std::decay_t<decltype(n)>, mdview::TextRun>) {
                            result += n.text;
                        }
                    },
                    node);
        }
        return result;
    };

    // Display width via Pango: measures actual rendered width in monospace font.
    // This correctly handles emoji (🖥🗑⭐), symbols (☀♥✔), and CJK characters
    // that wcwidth miscounts for monospace rendering.
    auto pango_layout = create_pango_layout("");
    PangoFontDescription *raw = pango_font_description_new();
    pango_font_description_set_family(raw, "monospace");
    pango_font_description_set_size(raw, 12 * PANGO_SCALE);
    Pango::FontDescription font_desc(raw, false);
    pango_layout->set_font_description(font_desc);

    // Get monospace character width (baseline)
    pango_layout->set_text("M");
    Pango::Rectangle baseline_lr = pango_layout->get_logical_extents();
    int char_width = baseline_lr.get_width();
    if (char_width <= 0)
        char_width = 12;

    // Strip variation selectors (U+FE00–U+FE0F) before measuring
    auto strip_variation_selectors = [](const std::string &s) -> Glib::ustring {
        Glib::ustring result;
        for (gunichar ch: Glib::ustring(s)) {
            if (ch < 0xFE00 || ch > 0xFE0F)
                result += ch;
        }
        return result;
    };

    auto display_width = [&](const std::string &s) -> size_t {
        Glib::ustring cleaned = strip_variation_selectors(s);
        if (cleaned.empty())
            return 0;
        pango_layout->set_text(cleaned);
        Pango::Rectangle lr = pango_layout->get_logical_extents();
        int w = lr.get_width();
        return (w + char_width / 2) / char_width; // round to nearest
    };

    // Compute max display width per column
    std::vector<size_t> col_widths(t.alignments.size(), 0);
    for (size_t c = 0; c < t.header.size(); ++c) {
        col_widths[c] = std::max(col_widths[c], display_width(extract_text(t.header[c].content)));
    }
    for (const auto &row: t.rows) {
        for (size_t c = 0; c < col_widths.size() && c < row.size(); ++c) {
            col_widths[c] = std::max(col_widths[c], display_width(extract_text(row[c].content)));
        }
    }
    for (auto &w: col_widths) {
        if (w == 0)
            w = 1;
    }

    auto write = [&](const Glib::ustring &s) {
        if (table_cell_tag) {
            iter = m_buffer->insert_with_tags(iter, s, {table_cell_tag});
        } else {
            iter = m_buffer->insert(iter, s);
        }
    };

    // Top border
    write(kTopLeft);
    for (size_t c = 0; c < col_widths.size(); ++c) {
        write(Glib::ustring(repeat(kHLine, col_widths[c] + 2)));
        write(c + 1 < col_widths.size() ? kTJunction : kTopRight);
    }
    write("\n");

    // Header row
    for (size_t c = 0; c < t.header.size() && c < col_widths.size(); ++c) {
        write(kVLine);
        std::string text = extract_text(t.header[c].content);
        char align = t.alignments.size() > c ? t.alignments[c] : 'l';
        size_t pad = col_widths[c] - display_width(text);
        if (align == 'r') {
            write(Glib::ustring(pad + 1, ' '));
            write(Glib::ustring(text));
        } else if (align == 'c') {
            size_t lp = pad / 2 + 1;
            write(Glib::ustring(lp, ' '));
            write(Glib::ustring(text));
            write(Glib::ustring(pad - lp + 1, ' '));
        } else {
            write(" ");
            write(Glib::ustring(text));
            write(Glib::ustring(pad + 1, ' '));
        }
    }
    write(kVLine);
    write("\n");

    // Separator line
    write(kSepLeft);
    for (size_t c = 0; c < col_widths.size(); ++c) {
        write(Glib::ustring(repeat(kHLine, col_widths[c] + 2)));
        write(c + 1 < col_widths.size() ? kPlus : kSepRight);
    }
    write("\n");

    // Body rows
    for (const auto &row: t.rows) {
        for (size_t c = 0; c < col_widths.size(); ++c) {
            write(kVLine);
            if (c >= row.size()) {
                write(Glib::ustring(std::string(col_widths[c] + 2, ' ')));
                continue;
            }
            std::string text = extract_text(row[c].content);
            char align = t.alignments.size() > c ? t.alignments[c] : 'l';
            size_t pad = col_widths[c] - display_width(text);
            if (align == 'r') {
                write(Glib::ustring(pad + 1, ' '));
                write(Glib::ustring(text));
            } else if (align == 'c') {
                size_t lp = pad / 2 + 1;
                write(Glib::ustring(lp, ' '));
                write(Glib::ustring(text));
                write(Glib::ustring(pad - lp + 1, ' '));
            } else {
                write(" ");
                write(Glib::ustring(text));
                write(Glib::ustring(pad + 1, ' '));
            }
        }
        write(kVLine);
        write("\n");
    }

    // Bottom border
    write(kBottomLeft);
    for (size_t c = 0; c < col_widths.size(); ++c) {
        write(Glib::ustring(repeat(kHLine, col_widths[c] + 2)));
        write(c + 1 < col_widths.size() ? kBJunction : kBottomRight);
    }
    write("\n");
}

void MarkdownView::insert_inline(const mdview::TextRun &tr, Gtk::TextIter &iter, Glib::RefPtr<Gtk::TextTag> tag) const {
    Glib::ustring text = tr.text;
    if (text.empty())
        return;

    // Collect all applicable tags to apply atomically via insert_with_tags
    std::vector<Glib::RefPtr<Gtk::TextTag>> tags;
    if (tag)
        tags.push_back(tag);
    if (tr.code) {
        if (auto code_tag = m_tag_table->lookup("inline_code"))
            tags.push_back(code_tag);
    }
    if (tr.bold) {
        if (auto bold_tag = m_tag_table->lookup("bold"))
            tags.push_back(bold_tag);
    }
    if (tr.italic) {
        if (auto italic_tag = m_tag_table->lookup("italic"))
            tags.push_back(italic_tag);
    }
    if (tr.strikethrough) {
        if (auto strike_tag = m_tag_table->lookup("strikethrough"))
            tags.push_back(strike_tag);
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
        m_dark_mode = theme.find("dark") != Glib::ustring::npos || theme.find("Dark") != Glib::ustring::npos;

        // Update colors when theme changes (connection auto-disconnects on widget destroy)
        auto settings_ref = settings;
        m_connections.emplace_back(settings_ref->property_gtk_theme_name().signal_changed().connect([this,
                                                                                                     settings_ref]() {
            Glib::ustring theme;
            settings_ref->get_property("gtk-theme-name", theme);
            m_dark_mode = theme.find("dark") != Glib::ustring::npos || theme.find("Dark") != Glib::ustring::npos;
            update_colors();
        }));
    }

    // Set initial code block background color
    update_code_backgrounds();
}

void MarkdownView::load_file(const Glib::ustring &path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        m_buffer->set_text("Error: Cannot open file\n" + path);
        m_has_content = false;
        return;
    }

    std::streamsize file_size = file.tellg();
    if (file_size > static_cast<std::streamsize>(mdview::MAX_INPUT_SIZE)) {
        m_buffer->set_text("Error: File too large (max 10 MB)\n" + path);
        m_has_content = false;
        return;
    }

    file.seekg(0);
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

void MarkdownView::update_colors() { update_code_backgrounds(); update_search_colors(); }

void MarkdownView::update_code_backgrounds() const {
    Glib::ustring bg_color;
    if (m_dark_mode) {
        bg_color = "#2a2a2a";
    } else {
        bg_color = "#f0f0f0";
    }

    if (const auto inline_code_tag = m_tag_table->lookup("inline_code")) {
        inline_code_tag->property_background_set() = false;
    }

    if (const auto code_block_tag = m_tag_table->lookup("code_block")) {
        code_block_tag->property_background() = bg_color;
        code_block_tag->property_paragraph_background() = bg_color;
    }

    if (const auto code_block_padding_top_tag = m_tag_table->lookup("code_block_padding_top")) {
        code_block_padding_top_tag->property_background() = bg_color;
        code_block_padding_top_tag->property_paragraph_background() = bg_color;
    }

    if (const auto code_block_padding_bottom_tag = m_tag_table->lookup("code_block_padding_bottom")) {
        code_block_padding_bottom_tag->property_background() = bg_color;
        code_block_padding_bottom_tag->property_paragraph_background() = bg_color;
    }

    if (const auto code_block_language_tag = m_tag_table->lookup("code_block_language")) {
        code_block_language_tag->property_background() = bg_color;
        code_block_language_tag->property_paragraph_background() = bg_color;
    }
}

void MarkdownView::setup_search_colors() {
    Gdk::RGBA selected_bg, selected_fg;
    auto style_context = get_style_context();
    
    if (style_context->lookup_color("theme_selected_bg_color", selected_bg) &&
        style_context->lookup_color("theme_selected_fg_color", selected_fg)) {
        
        auto highlight_tag = m_tag_table->lookup("search_highlight");
        auto current_tag = m_tag_table->lookup("search_highlight_current");
        
        if (highlight_tag && current_tag) {
            auto to_hex = [](double component) -> Glib::ustring {
                int value = static_cast<int>(std::round(component * 255.0));
                std::ostringstream oss;
                oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << value;
                return Glib::ustring(oss.str());
            };
            
            Glib::ustring bg_hex = Glib::ustring::format(
                "#", to_hex(selected_bg.get_red()), to_hex(selected_bg.get_green()), to_hex(selected_bg.get_blue())
            );
            
            Glib::ustring fg_hex = Glib::ustring::format(
                "#", to_hex(selected_fg.get_red()), to_hex(selected_fg.get_green()), to_hex(selected_fg.get_blue())
            );
            
            current_tag->property_background() = bg_hex;
            current_tag->property_foreground() = fg_hex;
            
            Gdk::RGBA dimmed_bg;
            dimmed_bg.set_rgba(
                selected_bg.get_red() * 0.6,
                selected_bg.get_green() * 0.6,
                selected_bg.get_blue() * 0.6,
                selected_bg.get_alpha()
            );
            
            Glib::ustring dimmed_hex = Glib::ustring::format(
                "#", to_hex(dimmed_bg.get_red()), to_hex(dimmed_bg.get_green()), to_hex(dimmed_bg.get_blue())
            );
            
            highlight_tag->property_background() = dimmed_hex;
            highlight_tag->property_foreground() = fg_hex;
        }
    }
}

void MarkdownView::update_search_colors() {
    setup_search_colors();
}

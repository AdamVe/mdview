#include "markdown_view.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <gtkmm.h>
#include "mdview/parser.hpp"

#ifdef USE_ADWAITA
#include <adwaita.h>
#endif

extern "C" {
#include "mdview-resources.h"
}

MarkdownView::MarkdownView()
    : Gtk::ScrolledWindow() {
    set_expand(true);

    auto css_provider = Gtk::CssProvider::create();
    try {
        GResource* resource = mdview_get_resource();
        g_resource_ref(resource);
        GError* error = nullptr;
        GBytes* bytes = g_resource_lookup_data(resource, "/io/github/mdview/style.css", G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
        if (bytes) {
            gsize size = 0;
            const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
            if (data && size > 0) {
                std::string css_data(data, size);
                css_provider->load_from_data(css_data);
            }
            g_bytes_unref(bytes);
        }
        if (error) {
            g_error_free(error);
        }
    } catch (...) {
    }

    auto text_view = Gtk::make_managed<Gtk::TextView>();
    text_view->get_style_context()->add_provider_for_display(
        Gdk::Display::get_default(), css_provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    m_tag_table = Gtk::TextTagTable::create();
    m_buffer = Gtk::TextBuffer::create(m_tag_table);
    setup_tags();

    text_view->set_buffer(m_buffer);
    text_view->set_editable(false);
    text_view->set_cursor_visible(false);

    text_view->add_css_class("markdown-view");

    auto gesture = Gtk::GestureClick::create();
    m_connections.push_back(gesture->signal_pressed().connect(sigc::track_obj([this, text_view](int button, double x, double y) {
        Gtk::TextBuffer::iterator iter;
        if (text_view->get_iter_at_location(iter, x, y)) {
            auto tag = m_buffer->get_tag_table()->lookup("link");
            if (tag) {
                auto tags = iter.get_tags();
                for (const auto& t : tags) {
                    if (t == tag) {
                        break;
                    }
                }
            }
        }
    }, *this)));
    text_view->add_controller(gesture);

    set_child(*text_view);

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
    add_tag("blockquote", "left-margin=24;style=italic;");
    add_tag("link", "foreground=blue;underline=single;");
    add_tag("hr", "");
    add_tag("bullet", "left-margin=24;");
    add_tag("search_highlight", "background=yellow;");
}

void MarkdownView::add_tag(const Glib::ustring& name, const Glib::ustring& properties) {
    auto tag = Gtk::TextTag::create(name);
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
        tag->property_style() = Pango::Style::ITALIC;
        tag->property_foreground() = "#888888";
    } else if (name == "link") {
        tag->property_foreground() = "#0000ff";
        tag->property_underline() = Pango::Underline::SINGLE;
    } else if (name == "hr") {
    } else if (name == "bullet") {
        tag->property_left_margin() = 24;
    } else if (name == "search_highlight") {
        tag->property_background() = "yellow";
    }
    m_tag_table->add(tag);
}

void MarkdownView::load(const mdview::DocModel& model) {
    m_buffer->set_text("");
    auto iter = m_buffer->end();

    for (const auto& node : model) {
        std::visit([this, &iter](const auto& n) { insert_node(n, iter); }, node);
    }
}

void MarkdownView::insert_node(const mdview::Heading& h, Gtk::TextIter& iter) {
    auto tag = m_tag_table->lookup("h" + std::to_string(std::min(h.level, 4)));
    if (!tag) tag = m_tag_table->lookup("h4");

    for (const auto& inline_node : h.content) {
        std::visit([this, &iter, &tag](const auto& inl) {
            insert_inline(inl, iter, tag);
        }, inline_node);
    }
    iter = m_buffer->insert(iter, "\n");
}

void MarkdownView::insert_node(const mdview::Paragraph& p, Gtk::TextIter& iter) {
    for (const auto& inline_node : p.content) {
        std::visit([this, &iter](const auto& inl) {
            insert_inline(inl, iter);
        }, inline_node);
    }
    iter = m_buffer->insert(iter, "\n");
}

void MarkdownView::insert_node(const mdview::CodeBlock& cb, Gtk::TextIter& iter) {
    auto tag = m_tag_table->lookup("code_block");

    if (!cb.language.empty()) {
        iter = m_buffer->insert(iter, cb.language);
        iter = m_buffer->insert(iter, "\n");
        auto lang_tag = m_tag_table->lookup("inline_code");
        if (lang_tag) {
            auto end = iter;
            end.backward_chars(cb.language.length() + 1);
            m_buffer->apply_tag(lang_tag, end, iter);
            iter = end;
        }
    }

    iter = m_buffer->insert(iter, cb.code);
    auto end = iter;
    end.backward_chars(cb.code.length());
    m_buffer->apply_tag(tag, end, iter);
    iter = m_buffer->insert(iter, "\n");
    iter = m_buffer->insert(iter, "\n");
}

void MarkdownView::insert_node(const mdview::BlockQuote& bq, Gtk::TextIter& iter) {
    auto tag = m_tag_table->lookup("blockquote");

    for (const auto& inline_node : bq.content) {
        std::visit([this, &iter, &tag](const auto& inl) {
            insert_inline(inl, iter, tag);
        }, inline_node);
    }
    iter = m_buffer->insert(iter, "\n");
}

void MarkdownView::insert_node(const mdview::HRule& hr, Gtk::TextIter& iter) {
    auto tag = m_tag_table->lookup("hr");
    iter = m_buffer->insert(iter, "— — —");
    auto end = iter;
    end.backward_chars(5);  // "— — —" is 5 characters
    m_buffer->apply_tag(tag, end, iter);
    iter = m_buffer->insert(iter, "\n");
}

void MarkdownView::insert_node(const mdview::BulletList& bl, Gtk::TextIter& iter) {
    auto tag = m_tag_table->lookup("bullet");

    for (const auto& item : bl.items) {
        iter = m_buffer->insert(iter, "• ");
        auto end = iter;
        end.backward_chars(2);  // "• " is 2 characters
        m_buffer->apply_tag(tag, end, iter);

        for (const auto& inline_node : item) {
            std::visit([this, &iter](const auto& inl) {
                insert_inline(inl, iter);
            }, inline_node);
        }
        iter = m_buffer->insert(iter, "\n");
    }
}

void MarkdownView::insert_node(const mdview::OrderedList& ol, Gtk::TextIter& iter) {
    int num = 1;
    for (const auto& item : ol.items) {
        Glib::ustring number = std::to_string(num++) + ". ";
        iter = m_buffer->insert(iter, number);

        for (const auto& inline_node : item) {
            std::visit([this, &iter](const auto& inl) {
                insert_inline(inl, iter);
            }, inline_node);
        }
        iter = m_buffer->insert(iter, "\n");
    }
}

void MarkdownView::insert_node(const mdview::TaskList& tl, Gtk::TextIter& iter) {
    for (const auto& item : tl.items) {
        char marker = item.first ? 'x' : ' ';
        iter = m_buffer->insert(iter, "[");
        iter = m_buffer->insert(iter, Glib::ustring(1, marker));
        iter = m_buffer->insert(iter, "] ");

        for (const auto& inline_node : item.second) {
            std::visit([this, &iter](const auto& inl) {
                insert_inline(inl, iter);
            }, inline_node);
        }
        iter = m_buffer->insert(iter, "\n");
    }
}

void MarkdownView::insert_inline(const mdview::TextRun& tr, Gtk::TextIter& iter,
                                  Glib::RefPtr<Gtk::TextTag> tag) {
    Glib::ustring text = tr.text;

    if (tag) {
        iter = m_buffer->insert(iter, text);
        auto end = iter;
        end.backward_chars(text.length());
        m_buffer->apply_tag(tag, end, iter);
    } else {
        iter = m_buffer->insert(iter, text);
    }

    // Apply inline tags
    if (tr.code) {
        auto code_tag = m_tag_table->lookup("inline_code");
        if (code_tag) {
            auto end = iter;
            end.backward_chars(text.length());
            m_buffer->apply_tag(code_tag, end, iter);
        }
    }
    if (tr.bold) {
        auto bold_tag = m_tag_table->lookup("bold");
        if (bold_tag) {
            auto end = iter;
            end.backward_chars(text.length());
            m_buffer->apply_tag(bold_tag, end, iter);
        }
    }
    if (tr.italic) {
        auto italic_tag = m_tag_table->lookup("italic");
        if (italic_tag) {
            auto end = iter;
            end.backward_chars(text.length());
            m_buffer->apply_tag(italic_tag, end, iter);
        }
    }
    if (tr.strikethrough) {
        auto strike_tag = m_tag_table->lookup("strikethrough");
        if (strike_tag) {
            auto end = iter;
            end.backward_chars(text.length());
            m_buffer->apply_tag(strike_tag, end, iter);
        }
    }
}

void MarkdownView::insert_inline(const mdview::Link& l, Gtk::TextIter& iter,
                                  Glib::RefPtr<Gtk::TextTag> tag) {
    insert_inline(mdview::TextRun{l.text}, iter, tag);
}

void MarkdownView::setup_color_detection() {
    m_dark_mode = false;
}

void MarkdownView::load_file(const Glib::ustring& path) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "Error: Cannot open file " << path << std::endl;
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    mdview::DocModel model = mdview::parse_markdown(content);
    load(model);
}

void MarkdownView::update_colors() {
    update_code_backgrounds();
}

void MarkdownView::update_code_backgrounds() {
    Glib::ustring bg_color;
    if (m_dark_mode) {
        bg_color = "#2a2a2a";
    } else {
        bg_color = "#f0f0f0";
    }

    auto inline_code_tag = m_tag_table->lookup("inline_code");
    if (inline_code_tag) {
        inline_code_tag->property_background() = bg_color;
    }

    auto code_block_tag = m_tag_table->lookup("code_block");
    if (code_block_tag) {
        code_block_tag->property_background() = bg_color;
    }
}

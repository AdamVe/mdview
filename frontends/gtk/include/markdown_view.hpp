#pragma once
#include <functional>
#include <gtkmm.h>
#include "mdview/doc_model.hpp"

class MarkdownView : public Gtk::TextView {
public:
    MarkdownView();
    ~MarkdownView() override;

    void load(const mdview::DocModel &model);
    void load_file(const Glib::ustring &path);
    void update_colors();
    void set_zoom(float zoom);
    float get_zoom() const;
    Glib::RefPtr<Gtk::TextBuffer> get_buffer() { return m_buffer; }

private:
    void setup_tags();
    void add_tag(const Glib::ustring &name, const Glib::ustring &properties) const;
    void insert_inline(const mdview::TextRun &tr, Gtk::TextIter &iter, Glib::RefPtr<Gtk::TextTag> tag = nullptr) const;
    void insert_inline(const mdview::Link &l, Gtk::TextIter &iter,
                       const Glib::RefPtr<Gtk::TextTag> &tag = nullptr) const;

    // Node insertion functions (must be declared before use in lambda)
    void insert_node(const mdview::Heading &h, Gtk::TextIter &iter);
    void insert_node(const mdview::Paragraph &p, Gtk::TextIter &iter);
    void insert_node(const mdview::CodeBlock &cb, Gtk::TextIter &iter) const;
    void insert_node(const mdview::BlockQuote &bq, Gtk::TextIter &iter);
    void insert_node(const mdview::HRule &hr, Gtk::TextIter &iter) const;
    void insert_node(const mdview::BulletList &bl, Gtk::TextIter &iter);
    void insert_node(const mdview::OrderedList &ol, Gtk::TextIter &iter);
    void insert_node(const mdview::TaskList &tl, Gtk::TextIter &iter);
    void insert_node(const mdview::Table &t, Gtk::TextIter &iter);

    void apply_zoom();
    void setup_color_detection();
    void update_code_backgrounds() const;
    bool animate_zoom();
    void setup_search_colors();
    void update_search_colors();

    Glib::RefPtr<Gtk::TextBuffer> m_buffer;
    Glib::RefPtr<Gtk::TextTagTable> m_tag_table;
    Glib::RefPtr<Gtk::TextTag> m_zoom_tag;
    float m_zoom = 1.0f;
    float m_target_zoom = 1.0f;
    bool m_zoom_animating = false;
    bool m_dark_mode = false;
    bool m_has_content = false;

    std::vector<sigc::connection> m_connections;
};

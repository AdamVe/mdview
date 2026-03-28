#pragma once
#include <gtkmm.h>
#include "mdview/doc_model.hpp"

class MarkdownView : public Gtk::ScrolledWindow {
public:
    MarkdownView();
    ~MarkdownView() override;

    void load(const mdview::DocModel& model);
    void load_file(const Glib::ustring& path);
    void update_colors();
    Glib::RefPtr<Gtk::TextBuffer> get_buffer() { return m_buffer; }

private:
    void setup_tags();
    void add_tag(const Glib::ustring& name, const Glib::ustring& properties);
    void insert_inline(const mdview::TextRun& tr, Gtk::TextIter& iter,
                       Glib::RefPtr<Gtk::TextTag> tag = nullptr);
    void insert_inline(const mdview::Link& l, Gtk::TextIter& iter,
                       Glib::RefPtr<Gtk::TextTag> tag = nullptr);

    // Node insertion functions (must be declared before use in lambda)
    void insert_node(const mdview::Heading& h, Gtk::TextIter& iter);
    void insert_node(const mdview::Paragraph& p, Gtk::TextIter& iter);
    void insert_node(const mdview::CodeBlock& cb, Gtk::TextIter& iter);
    void insert_node(const mdview::BlockQuote& bq, Gtk::TextIter& iter);
    void insert_node(const mdview::HRule& hr, Gtk::TextIter& iter);
    void insert_node(const mdview::BulletList& bl, Gtk::TextIter& iter);
    void insert_node(const mdview::OrderedList& ol, Gtk::TextIter& iter);
    void insert_node(const mdview::TaskList& tl, Gtk::TextIter& iter);

    void setup_color_detection();
    void update_code_backgrounds();

    Glib::RefPtr<Gtk::TextBuffer> m_buffer;
    Glib::RefPtr<Gtk::TextTagTable> m_tag_table;
    bool m_dark_mode = false;

    std::vector<sigc::connection> m_connections;
};

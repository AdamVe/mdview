#pragma once
#include <gtkmm.h>

class SearchBar : public Gtk::SearchBar {
public:
    SearchBar();
    ~SearchBar() override;

    void find_next();
    void find_previous();
    void clear_highlights();
    void set_buffer(Glib::RefPtr<Gtk::TextBuffer> buffer);
    void set_text_view(Gtk::TextView *text_view);
    void set_hadjustment(Glib::RefPtr<Gtk::Adjustment> hadjustment);
    void update_highlight_colors();

private:
    void on_search_changed();
    void on_match_case_changed();
    void on_highlight_all_changed();
    void on_whole_words_changed();
    void update_matches();
    void apply_highlights();
    void update_match_count();
    void scroll_to_match();
    void set_match_index(int index);

    bool is_whole_word(const Gtk::TextIter &match_start, const Gtk::TextIter &match_end, const Glib::ustring &text);

    Gtk::SearchEntry *m_search_entry = nullptr;
    Gtk::CheckButton *m_highlight_all_button = nullptr;
    Gtk::CheckButton *m_case_button = nullptr;
    Gtk::CheckButton *m_whole_words_button = nullptr;
    Gtk::Label *m_match_label = nullptr;
    Glib::RefPtr<Gtk::TextBuffer> m_buffer;
    Gtk::TextView *m_text_view;
    Glib::RefPtr<Gtk::Adjustment> m_hadjustment;
    struct Match {
        int start_offset;
        int length;
    };
    std::vector<Match> m_matches;
    int m_current_match_index;
    int m_total_matches;
    bool m_matches_capped = false; // hit the scan cap; count shown as "N+"
    bool m_has_matches;
    bool m_highlight_all;
    bool m_whole_words;

    std::vector<sigc::connection> m_buffer_connections;
    sigc::connection m_buffer_changed_timeout;
    bool m_buffer_changed_pending = false;
};

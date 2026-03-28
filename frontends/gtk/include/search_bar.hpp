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

private:
    void on_search_changed();
    void update_matches();
    void update_match_count();
    void scroll_to_match();

    Gtk::SearchEntry* m_search_entry;
    Glib::RefPtr<Gtk::TextBuffer> m_buffer;
    std::vector<int> m_matches;  // Store offsets instead of iterators
    int m_current_match_index;
    int m_total_matches;
    bool m_has_matches = false;
};

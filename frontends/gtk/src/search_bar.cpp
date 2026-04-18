#include "search_bar.hpp"
#include <format>

SearchBar::SearchBar() :
    m_search_entry(Gtk::make_managed<Gtk::SearchEntry>()), m_buffer(nullptr), m_current_match_index(-1),
    m_total_matches(0) {
    const auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    box->append(*m_search_entry);

    const auto match_label = Gtk::make_managed<Gtk::Label>("");
    match_label->add_css_class("dim-label");
    box->append(*match_label);

    set_child(*box);

    connect_entry(*m_search_entry);

    m_search_entry->signal_search_changed().connect([this]() { on_search_changed(); });
    m_search_entry->signal_next_match().connect([this]() { find_next(); });
    m_search_entry->signal_previous_match().connect([this]() { find_previous(); });
    m_search_entry->signal_stop_search().connect([this]() { set_search_mode(false); });
}

SearchBar::~SearchBar() = default;

void SearchBar::set_buffer(Glib::RefPtr<Gtk::TextBuffer> buffer) { m_buffer = std::move(buffer); }

void SearchBar::on_search_changed() {
    if (const Glib::ustring text = m_search_entry->get_text(); text.empty()) {
        clear_highlights();
        return;
    }

    update_matches();
}

void SearchBar::update_matches() {
    clear_highlights();

    const Glib::ustring text = m_search_entry->get_text();
    if (!m_buffer || text.empty()) {
        m_has_matches = false;
        m_matches.clear();
        m_total_matches = 0;
        update_match_count();
        return;
    }

    m_matches.clear();
    Gtk::TextIter start = m_buffer->get_iter_at_offset(0);

    Gtk::TextIter match_start, match_end;
    while (start.forward_search(text, Gtk::TextSearchFlags(), match_start, match_end)) {
        m_matches.push_back(match_start.get_offset());
        start = match_end;
        start.forward_char();
    }

    m_total_matches = static_cast<int>(m_matches.size());
    m_has_matches = m_total_matches > 0;
    m_current_match_index = -1;

    update_match_count();

    if (m_has_matches) {
        m_current_match_index = 0;

        if (const auto tag = m_buffer->get_tag_table()->lookup("search_highlight")) {
            const Gtk::TextIter start_iter = m_buffer->get_iter_at_offset(m_matches[0]);
            Gtk::TextIter end_iter = start_iter;
            end_iter.forward_chars(text.length());
            m_buffer->apply_tag(tag, start_iter, end_iter);
        }
    }
}

void SearchBar::update_match_count() {
    const auto box = dynamic_cast<Gtk::Box *>(get_child());
    if (!box)
        return;

    const auto children = box->get_children();
    if (children.size() < 2)
        return;

    const auto match_label = dynamic_cast<Gtk::Label *>(children[1]);
    if (!match_label)
        return;

    if (m_total_matches > 0 && m_current_match_index >= 0) {
        const std::string count_text = std::format("{} / {}", m_current_match_index + 1, m_total_matches);
        match_label->set_text(count_text);
        match_label->set_visible(true);
    } else {
        match_label->set_text("");
        match_label->set_visible(false);
    }
}

void SearchBar::scroll_to_match() const {
    if (m_current_match_index < 0 || m_matches.empty())
        return;

    if (!m_buffer)
        return;

    const Gtk::TextIter iter = m_buffer->get_iter_at_offset(m_matches[m_current_match_index]);
    m_buffer->place_cursor(iter);
}

void SearchBar::find_next() {
    if (!m_has_matches || m_matches.empty())
        return;

    if (m_current_match_index < 0) {
        m_current_match_index = 0;
    } else if (m_current_match_index >= m_total_matches - 1) {
        m_current_match_index = 0; // Wrap to first
    } else {
        m_current_match_index++;
    }

    clear_current_highlight();

    if (const auto tag = m_buffer->get_tag_table()->lookup("search_highlight")) {
        if (m_current_match_index >= 0) {
            const Gtk::TextIter start_iter = m_buffer->get_iter_at_offset(m_matches[m_current_match_index]);
            Gtk::TextIter end_iter = start_iter;
            end_iter.forward_chars(m_search_entry->get_text().length());
            m_buffer->apply_tag(tag, start_iter, end_iter);
        }
    }

    update_match_count();
    scroll_to_match();
}

void SearchBar::find_previous() {
    if (!m_has_matches || m_matches.empty())
        return;

    if (m_current_match_index < 0) {
        m_current_match_index = m_total_matches - 1;
    } else if (m_current_match_index <= 0) {
        m_current_match_index = m_total_matches - 1; // Wrap to last
    } else {
        m_current_match_index--;
    }

    clear_current_highlight();

    if (const auto tag = m_buffer->get_tag_table()->lookup("search_highlight")) {
        if (m_current_match_index >= 0) {
            const Gtk::TextIter start_iter = m_buffer->get_iter_at_offset(m_matches[m_current_match_index]);
            Gtk::TextIter end_iter = start_iter;
            end_iter.forward_chars(m_search_entry->get_text().length());
            m_buffer->apply_tag(tag, start_iter, end_iter);
        }
    }

    update_match_count();
    scroll_to_match();
}

void SearchBar::clear_current_highlight() const {
    if (const auto tag = m_buffer->get_tag_table()->lookup("search_highlight")) {
        m_buffer->remove_tag(tag, m_buffer->begin(), m_buffer->end());
    }
}

void SearchBar::clear_highlights() {
    if (const auto tag = m_buffer->get_tag_table()->lookup("search_highlight")) {
        m_buffer->remove_tag(tag, m_buffer->begin(), m_buffer->end());
    }
    m_matches.clear();
    m_current_match_index = -1;
    m_total_matches = 0;
    m_has_matches = false;
    update_match_count();
}

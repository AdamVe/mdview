#include "search_bar.hpp"
#include <string>

namespace {
    constexpr const char *s_search_bar_ui_path = "/com/adamve/mdview/search_bar.ui";
}

SearchBar::SearchBar() :
    m_buffer(nullptr),
    m_text_view(nullptr),
    m_hadjustment(nullptr),
    m_current_match_index(-1),
    m_total_matches(0),
    m_has_matches(false),
    m_highlight_all(true),
    m_whole_words(false) {

    const auto builder = Gtk::Builder::create_from_resource(s_search_bar_ui_path);

    m_search_entry = builder->get_widget<Gtk::SearchEntry>("search_entry");
    m_highlight_all_button = builder->get_widget<Gtk::CheckButton>("highlight_all_button");
    m_case_button = builder->get_widget<Gtk::CheckButton>("case_button");
    m_whole_words_button = builder->get_widget<Gtk::CheckButton>("whole_words_button");
    m_match_label = builder->get_widget<Gtk::Label>("match_label");

    m_search_entry->set_hexpand(true);
    m_highlight_all_button->set_active(true);
    m_whole_words_button->set_active(false);

    set_child(*m_search_entry->get_parent());

    connect_entry(*m_search_entry);

    m_search_entry->signal_search_changed().connect([this]() { on_search_changed(); });
    m_search_entry->signal_next_match().connect([this]() { find_next(); });
    m_search_entry->signal_previous_match().connect([this]() { find_previous(); });
    m_search_entry->signal_stop_search().connect([this]() { set_search_mode(false); });
    m_case_button->signal_toggled().connect([this]() { on_match_case_changed(); });
    m_highlight_all_button->signal_toggled().connect([this]() { on_highlight_all_changed(); });
    m_whole_words_button->signal_toggled().connect([this]() { on_whole_words_changed(); });
}

SearchBar::~SearchBar() {
    for (auto &conn : m_buffer_connections) {
        conn.disconnect();
    }
    m_buffer_connections.clear();
}

void SearchBar::set_buffer(Glib::RefPtr<Gtk::TextBuffer> buffer) {
    m_buffer = std::move(buffer);
    clear_highlights();
}

void SearchBar::set_text_view(Gtk::TextView *text_view) {
    m_text_view = text_view;
    if (!m_buffer || !m_text_view)
        return;

    m_buffer_connections.emplace_back(
            m_buffer->signal_mark_set().connect([this](const Gtk::TextIter &iter, Glib::RefPtr<Gtk::TextMark> mark) {
                if (mark && mark->get_name() == "insert") {
                    if (m_has_matches && m_total_matches > 0) {
                        const int cursor_offset = iter.get_offset();

                        int closest_idx = -1;
                         int closest_dist = INT32_MAX;
                         for (int i = 0; i < m_total_matches; i++) {
                             const int dist = std::abs(cursor_offset - m_matches[i].start_offset);
                             if (dist < closest_dist) {
                                 closest_dist = dist;
                                 closest_idx = i;
                             }
                         }

                        if (closest_idx >= 0 && closest_dist < 30) {
                            set_match_index(closest_idx);
                        }
                    }
                }
            }));

    Glib::RefPtr<Gtk::TextBuffer> buf = m_buffer;
    m_buffer_connections.emplace_back(
            m_buffer->signal_changed().connect([this, buf]() {
                const Glib::ustring text = m_search_entry->get_text();
                if (text.empty()) {
                    clear_highlights();
                } else {
                    m_buffer_changed_pending = true;
                    if (!m_buffer_changed_timeout.connected()) {
                        m_buffer_changed_timeout = Glib::signal_timeout().connect([this, buf]() {
                            m_buffer_changed_pending = false;
                            if (m_has_matches && buf) {
                                update_matches();
                            }
                            return false;
                        }, 200);
                    }
                }
            }));
}

void SearchBar::set_hadjustment(Glib::RefPtr<Gtk::Adjustment> hadjustment) {
    m_hadjustment = hadjustment;
}

void SearchBar::update_highlight_colors() {
    apply_highlights();
}

void SearchBar::on_search_changed() {
    if (m_search_entry->get_text().empty()) {
        clear_highlights();
        return;
    }

    update_matches();
}

void SearchBar::on_match_case_changed() {
    update_matches();
}

void SearchBar::on_highlight_all_changed() {
    m_highlight_all = m_highlight_all_button->get_active();
    apply_highlights();
}

void SearchBar::on_whole_words_changed() {
    m_whole_words = m_whole_words_button->get_active();
    update_matches();
}

bool SearchBar::is_whole_word(const Gtk::TextIter &match_start, const Gtk::TextIter &match_end, const Glib::ustring & /*text*/) {
    Gtk::TextIter prev = match_start;
    if (prev.backward_char()) {
        gunichar ch = prev.get_char();
        if (g_unichar_isalnum(ch) || ch == '_') {
            return false;
        }
    }

    // match_end points to the character after the match
    // Check if we're not at the end of the buffer
    if (!match_end.is_end()) {
        gunichar ch = match_end.get_char();
        if (g_unichar_isalnum(ch) || ch == '_') {
            return false;
        }
    }

    return true;
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

    Gtk::TextSearchFlags flags = Gtk::TextSearchFlags::CASE_INSENSITIVE;
    if (m_case_button->get_active()) {
        flags = Gtk::TextSearchFlags();
    }

    Gtk::TextIter match_start, match_end;
    // Pathological queries ("a" in a 2 MB document) would otherwise collect
    // millions of matches per keystroke and freeze the UI.
    constexpr size_t kMaxMatches = 10000;
    m_matches_capped = false;
    while (m_matches.size() < kMaxMatches && start.forward_search(text, flags, match_start, match_end)) {
        int match_length = static_cast<int>(match_end.get_offset() - match_start.get_offset());
        if (m_whole_words) {
            if (is_whole_word(match_start, match_end, text)) {
                m_matches.push_back({match_start.get_offset(), match_length});
            }
        } else {
            m_matches.push_back({match_start.get_offset(), match_length});
        }
        start = match_end;
    }
    m_matches_capped = m_matches.size() == kMaxMatches;

    m_total_matches = static_cast<int>(m_matches.size());
    m_has_matches = m_total_matches > 0;
    update_match_count();

    if (m_has_matches) {
        apply_highlights();
        set_match_index(0);
    }
}

void SearchBar::apply_highlights() {
    if (!m_buffer || m_matches.empty())
        return;

    const Glib::ustring text = m_search_entry->get_text();
    if (text.empty())
        return;

    const auto highlight_tag = m_buffer->get_tag_table()->lookup("search_highlight");
    const auto current_tag = m_buffer->get_tag_table()->lookup("search_highlight_current");

    if (!highlight_tag && !current_tag)
        return;

    m_buffer->begin();
    if (highlight_tag) {
        m_buffer->remove_tag(highlight_tag, m_buffer->begin(), m_buffer->end());
    }
    if (current_tag) {
        m_buffer->remove_tag(current_tag, m_buffer->begin(), m_buffer->end());
    }

    if (m_highlight_all) {
        for (int i = 0; i < m_total_matches; i++) {
            const Gtk::TextIter start_iter = m_buffer->get_iter_at_offset(m_matches[i].start_offset);
            Gtk::TextIter end_iter = start_iter;
            end_iter.forward_chars(m_matches[i].length);

            if (highlight_tag) {
                m_buffer->apply_tag(highlight_tag, start_iter, end_iter);
            }
        }
    }

    if (current_tag && m_current_match_index >= 0 && m_current_match_index < m_total_matches) {
        const Gtk::TextIter start_iter = m_buffer->get_iter_at_offset(m_matches[m_current_match_index].start_offset);
        Gtk::TextIter end_iter = start_iter;
        end_iter.forward_chars(m_matches[m_current_match_index].length);
        m_buffer->apply_tag(current_tag, start_iter, end_iter);
    }
}

void SearchBar::update_match_count() {
    if (!m_match_label)
        return;

    if (m_has_matches && m_current_match_index >= 0) {
        const std::string count_text = std::to_string(m_current_match_index + 1) + " / " +
                                       std::to_string(m_total_matches) + (m_matches_capped ? "+" : "");
        m_match_label->set_text(count_text);
    } else {
        m_match_label->set_text("No match");
    }
}

void SearchBar::scroll_to_match() {
    if (m_current_match_index < 0 || m_matches.empty())
        return;

 if (!m_buffer || !m_text_view)
        return;

    const Glib::ustring search_text = m_search_entry->get_text();
    Gtk::TextIter iter = m_buffer->get_iter_at_offset(m_matches[m_current_match_index].start_offset);
    Gtk::TextIter match_end = iter;
    match_end.forward_chars(m_matches[m_current_match_index].length);

    Gdk::Rectangle match_rect;
    m_text_view->get_iter_location(iter, match_rect);

    Gdk::Rectangle visible_rect;
    m_text_view->get_visible_rect(visible_rect);

    const bool match_visible = (match_rect.get_x() >= visible_rect.get_x() &&
                                match_rect.get_y() >= visible_rect.get_y() &&
                                match_rect.get_x() + match_rect.get_width() <= visible_rect.get_x() + visible_rect.get_width() &&
                                match_rect.get_y() + match_rect.get_height() <= visible_rect.get_y() + visible_rect.get_height());

    if (!match_visible) {
        m_text_view->scroll_to(iter, 0.0, 0.0, 0.5);
    }

    // Handle horizontal scrolling
    if (m_hadjustment) {
        Gdk::Rectangle end_rect;
        m_text_view->get_iter_location(match_end, end_rect);

        const double page_width = visible_rect.get_width();
        const double match_left = match_rect.get_x() - visible_rect.get_x();
        const double match_right = end_rect.get_x() + end_rect.get_width() - visible_rect.get_x();
        const double current_pos = m_hadjustment->get_value();
        const double max_pos = m_hadjustment->get_upper();

        // If match is fully visible horizontally, reset to 0
        if (match_left >= current_pos && match_right <= current_pos + page_width) {
            m_hadjustment->set_value(0);
        }
        // If match is to the left of visible area, scroll left so the whole match is visible
        else if (match_left < current_pos) {
            m_hadjustment->set_value(std::max(0.0, match_left));
        }
        // If match is to the right of visible area, scroll right so the whole match is visible
        else if (match_right > current_pos + page_width) {
            m_hadjustment->set_value(std::min(max_pos, match_right - page_width));
        }
    }
}

void SearchBar::set_match_index(int index) {
    if (index < 0 || index >= m_total_matches)
        return;

    m_current_match_index = index;
    apply_highlights();
    update_match_count();
    scroll_to_match();
}

void SearchBar::find_next() {
    if (!m_has_matches || m_matches.empty())
        return;

    if (m_hadjustment) {
        m_hadjustment->set_value(0);
    }

    int next_idx = m_current_match_index + 1;
    if (next_idx >= m_total_matches) {
        next_idx = 0;
    }

    set_match_index(next_idx);
}

void SearchBar::find_previous() {
    if (!m_has_matches || m_matches.empty())
        return;

    if (m_hadjustment) {
        m_hadjustment->set_value(0);
    }

    int prev_idx = m_current_match_index - 1;
    if (prev_idx < 0) {
        prev_idx = m_total_matches - 1;
    }

    set_match_index(prev_idx);
}

void SearchBar::clear_highlights() {
    // Guard before touching the buffer: this runs on every buffer change, and
    // remove_tag(begin, end) walks the whole run tree — quadratic during
    // document load when no highlights were ever applied.
    if (m_buffer && !m_matches.empty()) {
        const auto highlight_tag = m_buffer->get_tag_table()->lookup("search_highlight");
        const auto current_tag = m_buffer->get_tag_table()->lookup("search_highlight_current");

        if (highlight_tag) {
            m_buffer->remove_tag(highlight_tag, m_buffer->begin(), m_buffer->end());
        }
        if (current_tag) {
            m_buffer->remove_tag(current_tag, m_buffer->begin(), m_buffer->end());
        }
    }

    m_matches.clear();
    m_current_match_index = -1;
    m_total_matches = 0;
    m_has_matches = false;
    update_match_count();
}

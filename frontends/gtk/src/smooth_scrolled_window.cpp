#include "smooth_scrolled_window.hpp"

#include <gtkmm/adjustment.h>
#include <gtkmm/eventcontrollerscroll.h>

SmoothScrolledWindow::SmoothScrolledWindow() {
    set_kinetic_scrolling(true);
    auto scroll_controller = Gtk::EventControllerScroll::create();
    scroll_controller->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
    m_scroll_connection = scroll_controller->signal_scroll().connect(
            sigc::mem_fun(*this, &SmoothScrolledWindow::on_scroll_event), false);
    add_controller(scroll_controller);
}

bool SmoothScrolledWindow::on_scroll_event(double /*dx*/, double dy) {
    auto adj = get_vadjustment();

    m_target_scroll_y = adj->get_value() + (dy * m_sensitivity);

    // Clamp
    double max_val = adj->get_upper() - adj->get_page_size();
    if (m_target_scroll_y < 0)
        m_target_scroll_y = 0;
    if (m_target_scroll_y > max_val)
        m_target_scroll_y = max_val;

    if (!m_is_animating) {
        m_is_animating = true;
        add_tick_callback([this, adj](const Glib::RefPtr<Gdk::FrameClock> &clock) -> bool {
            (void) clock;
            return this->animate_scroll(adj, m_target_scroll_y);
        });
    }

    return true;
}

bool SmoothScrolledWindow::animate_scroll(const Glib::RefPtr<Gtk::Adjustment> &adj, double target) {
    double current = adj->get_value();
    double diff = target - current;

    if (std::abs(diff) < 0.5) {
        adj->set_value(target);
        m_is_animating = false;
        return false;
    }

    adj->set_value(current + (diff * m_smoothing));
    return true;
}

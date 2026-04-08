#pragma once

#include <gtkmm/scrolledwindow.h>

class SmoothScrolledWindow : public Gtk::ScrolledWindow {
public:
    SmoothScrolledWindow();
    ~SmoothScrolledWindow() override = default;

private:
    bool on_scroll_event(double dx, double dy);
    bool animate_scroll(const Glib::RefPtr<Gtk::Adjustment>& adj, double target);

    sigc::connection m_scroll_connection;

    double m_target_scroll_y = 0.0;
    bool m_is_animating = false;

    const double m_sensitivity = 650.0;
    const double m_smoothing = 0.15;
};
#pragma once

#include <gtkmm/scrolledwindow.h>

class SmoothScrolledWindow : public Gtk::ScrolledWindow {
public:
    SmoothScrolledWindow();
    ~SmoothScrolledWindow() override = default;
};

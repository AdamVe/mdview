#pragma once
#include <gtkmm.h>
#include "main_window.hpp"

class App : public Gtk::Application {
public:
    App();
    ~App() override;

    static Glib::RefPtr<App> create();

    MainWindow* create_window();

protected:
    void on_startup() override;
};

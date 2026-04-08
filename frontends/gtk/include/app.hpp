#pragma once
#include <gtkmm.h>
#include <vector>
#include "main_window.hpp"

class App : public Gtk::Application {
public:
    App();
    ~App() override;

    static Glib::RefPtr<App> create();

    MainWindow *create_window();

protected:
    void on_activate() override;
    void on_open(const std::vector<Glib::RefPtr<Gio::File>> &files, const Glib::ustring &hint) override;
};

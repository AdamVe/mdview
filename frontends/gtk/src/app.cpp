#include "app.hpp"
#include <iostream>
#include <memory>

App::App()
    : Gtk::Application("com.adamve.mdview") {
}

App::~App() = default;

Glib::RefPtr<App> App::create() {
    return std::make_shared<App>();
}

MainWindow* App::create_window() {
    return new MainWindow(Glib::RefPtr<App>(this));
}

void App::on_startup() {
    Gtk::Application::on_startup();
}


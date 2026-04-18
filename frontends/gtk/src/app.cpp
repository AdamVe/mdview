#include "app.hpp"

App::App() : Application("com.adamve.mdview") {
    g_application_set_flags(G_APPLICATION(gobj()), static_cast<GApplicationFlags>(G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE));
}

App::~App() = default;

Glib::RefPtr<App> App::create() {
    return std::make_shared<App>();
}

MainWindow *App::create_window() {
    const auto builder = Gtk::Builder::create_from_resource("/com/adamve/mdview/mdview.ui");

    auto *window = Gtk::Builder::get_widget_derived<MainWindow>(builder, "main_window", Glib::RefPtr<App>(this));

    window->set_data("builder", new Glib::RefPtr<Gtk::Builder>(builder),
                     [](void *p) { delete static_cast<Glib::RefPtr<Gtk::Builder> *>(p); });

    add_window(*window);
    window->show();
    return window;
}

void App::on_activate() {
    auto *win = create_window();
    win->present();
}

void App::on_open(const std::vector<Glib::RefPtr<Gio::File>> &files, const Glib::ustring & /*hint*/) {
    for (const auto &file : files) {
        try {
            const auto path = file->get_path();
            if (!path.empty()) {
                auto *window = create_window();
                window->open_file(path);
                window->present();
            }
        } catch (const std::exception &e) {
            // Silently ignore errors
        }
    }
}
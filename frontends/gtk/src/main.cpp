#include "app.hpp"

static void on_activate(Glib::RefPtr<App> app, MainWindow* /*window*/, int argc, char* argv[]) {
    MainWindow* win = app->create_window();

    if (argc > 1) {
        Glib::ustring path = argv[1];
        win->open_file(path);
    }

    win->present();
}

int main(int argc, char* argv[]) {
    auto app = App::create();

    [[maybe_unused]] auto conn = app->signal_activate().connect([app, argc, argv]() {
        on_activate(app, nullptr, argc, argv);
    });

    return app->run();
}

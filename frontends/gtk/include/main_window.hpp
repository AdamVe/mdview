#pragma once
#include <gtkmm.h>
#include "markdown_view.hpp"
#include "search_bar.hpp"

class App;

class MainWindow : public Gtk::ApplicationWindow {
public:
    explicit MainWindow(const Glib::RefPtr<App>& app);
    ~MainWindow() override;

    void open_file(const Glib::ustring& path);
    void toggle_search();
    void close_search();
    void find_next();
    void find_previous();

private:
    void setup_ui();
    void setup_actions();
    void on_open_file();
    void on_style_changed();

    Glib::RefPtr<App> m_app;
    MarkdownView* m_markdown_view;
    SearchBar* m_search_bar;
    Gtk::Box* m_toolbar_view;
    Gtk::HeaderBar* m_header_bar;
    Glib::RefPtr<Gtk::FileDialog> m_file_dialog;
    Glib::RefPtr<Gtk::Filter> m_file_filter;

    std::vector<sigc::connection> m_connections;
};

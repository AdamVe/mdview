#pragma once
#include <gtkmm.h>
#include "markdown_view.hpp"
#include "search_bar.hpp"

class App;

class MainWindow : public Gtk::ApplicationWindow {
public:
    MainWindow(BaseObjectType *cobject, const Glib::RefPtr<Gtk::Builder> &builder, const Glib::RefPtr<App> &app);
    ~MainWindow() override;

    void open_file(const Glib::ustring &path);
    void toggle_search() const;
    void close_search() const;
    void find_next() const;
    void find_previous() const;
    void show_about();

private:
    void setup_actions();
    void on_open_file();
    void on_style_changed() const;

    Glib::RefPtr<App> m_app;
    MarkdownView *m_markdown_view;
    SearchBar *m_search_bar;
    Gtk::Box *m_content_box;
    Gtk::Box *m_empty_state_box{};
    Gtk::HeaderBar *m_header_bar;
    Gtk::Stack *m_main_stack{};
    Gtk::ScrolledWindow *m_content_scrolled_window{};
    Gtk::Button *m_open_button{};
    Gtk::MenuButton *m_menu_button{};
    Glib::RefPtr<Gtk::FileDialog> m_file_dialog;
    Glib::RefPtr<Gtk::Filter> m_file_filter;

    std::vector<sigc::connection> m_connections;
};

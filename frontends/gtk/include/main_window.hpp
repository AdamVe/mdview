#pragma once
#include <gtkmm.h>
#include "markdown_view.hpp"
#include "search_bar.hpp"

class SmoothScrolledWindow;

class App;

class MainWindow : public Gtk::ApplicationWindow {
public:
    MainWindow(BaseObjectType *cobject, const Glib::RefPtr<Gtk::Builder> &builder, App *app);
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
    void on_style_changed();
    void on_zoom_scroll(double dx, double dy, Gdk::ModifierType state);
    void on_zoom_in();
    void on_zoom_out();
    void on_zoom_reset();

    App *m_app{};

    // Owned widgets (created via Gtk::make_managed, ref-counted by GTK tree)
    MarkdownView *m_markdown_view;
    SearchBar *m_search_bar;
    SmoothScrolledWindow *m_content_scrolled_window;
    Gtk::Button *m_open_button;
    Gtk::MenuButton *m_menu_button;

    // Builder-extracted widgets (owned by UI tree)
    Gtk::Box *m_content_box;
    Gtk::Box *m_empty_state_box{};
    Gtk::HeaderBar *m_header_bar;
    Gtk::Stack *m_main_stack{};

    Glib::RefPtr<Gtk::FileDialog> m_file_dialog;
    Glib::RefPtr<Gtk::EventControllerScroll> m_zoom_scroll_controller;

    std::vector<sigc::connection> m_connections;
};

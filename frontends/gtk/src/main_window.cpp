#include "main_window.hpp"

#include "app.hpp"

MainWindow::MainWindow(const Glib::RefPtr<App> &app)
        : Gtk::ApplicationWindow(app),
          m_app(app),
          m_markdown_view(nullptr),
          m_search_bar(nullptr),
          m_toolbar_view(nullptr),
          m_header_bar(nullptr),
          m_file_dialog(nullptr) {
    set_title("mdview");
    set_default_size(800, 600);

    setup_ui();
    setup_actions();
}

MainWindow::~MainWindow() = default;

void MainWindow::setup_ui() {
    m_toolbar_view = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

    m_header_bar = Gtk::make_managed<Gtk::HeaderBar>();
    set_titlebar(*m_header_bar);

    m_file_dialog = Gtk::FileDialog::create();
    m_file_dialog->set_title("Open Markdown File");

    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto filter = Gtk::FileFilter::create();
    filter->set_name("Markdown files");
    filter->add_pattern("*.md");
    filters->append(filter);
    m_file_dialog->set_filters(filters);

    auto open_button = Gtk::make_managed<Gtk::Button>();
    open_button->set_icon_name("document-open-symbolic");
    open_button->set_tooltip_text("Open File");
    m_connections.emplace_back(open_button->signal_clicked().connect(sigc::track_obj([this]() { on_open_file(); }, *this)));
    m_header_bar->pack_start(*open_button);

    m_markdown_view = Gtk::make_managed<MarkdownView>();
    m_toolbar_view->append(*m_markdown_view);
    m_search_bar = Gtk::make_managed<SearchBar>();
    m_search_bar->set_search_mode(false);
    m_search_bar->set_buffer(m_markdown_view->get_buffer());
    m_toolbar_view->append(*m_search_bar);
    m_search_bar->set_key_capture_widget(*this);
    set_child(*m_toolbar_view);
}

void MainWindow::setup_actions() {
    m_app->set_accel_for_action("win.search", {"<Ctrl>f"});
    add_action("search", [this]() { toggle_search(); });
    m_app->set_accel_for_action("win.close", {"<Ctrl>w"});
    add_action("close", [this]() { close(); });
    m_app->set_accel_for_action("win.open", {"<Ctrl>o"});
    add_action("open", [this]() { on_open_file(); });
}

void MainWindow::on_open_file() {
    m_file_dialog->open(*this, [this](Glib::RefPtr<Gio::AsyncResult> &result) {
        try {
            auto file = m_file_dialog->open_finish(result);
            open_file(file->get_path());
        } catch (const Gtk::DialogError &e) {
            // user cancelled — not an error
        }
    });
}

void MainWindow::open_file(const Glib::ustring &path) {
    m_markdown_view->load_file(path);
    set_title(Glib::ustring("mdview - ") + Glib::path_get_basename(path.c_str()));
}

void MainWindow::toggle_search() {
    bool current_mode = m_search_bar->get_search_mode();
    m_search_bar->set_search_mode(!current_mode);
}

void MainWindow::close_search() {
    m_search_bar->set_search_mode(false);
}

void MainWindow::find_next() {
    m_search_bar->find_next();
}

void MainWindow::find_previous() {
    m_search_bar->find_previous();
}

void MainWindow::on_style_changed() {
    m_markdown_view->update_colors();
}


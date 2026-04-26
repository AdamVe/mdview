#include "main_window.hpp"

#include "app.hpp"
#include "markdown_view.hpp"
#include "search_bar.hpp"
#include "smooth_scrolled_window.hpp"

#include <gtkmm/builder.h>
#include <gtkmm/menubutton.h>

#ifdef USE_ADWAITA
#include <adwaita.h>
#endif

namespace {
    constexpr struct {
        const char *name = "mdview";
        const char *icon = "com.adamve.mdview";
        const char *version = MDVIEW_VERSION;
        const char *comments = "A native, lightweight Markdown viewer.";
        const char *website = "https://github.com/adamve/mdview";
    } s_app_info;
} // namespace

MainWindow::MainWindow(BaseObjectType *cobject, const Glib::RefPtr<Gtk::Builder> &builder, App *app) :
    ApplicationWindow(cobject), m_app(app), m_content_box(nullptr), m_header_bar(nullptr), m_file_dialog(nullptr) {

    set_title("mdview");

    // Get widgets from builder (owned by UI tree, kept as raw pointers)
    m_content_box = builder->get_widget<Gtk::Box>("content_box");
    m_main_stack = builder->get_widget<Gtk::Stack>("main_stack");

    // Owned widgets created via make_managed (ref-counted, managed by GTK tree)
    m_content_scrolled_window = Gtk::make_managed<SmoothScrolledWindow>();
    m_content_scrolled_window->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    m_content_scrolled_window->set_vexpand(true);
    m_content_scrolled_window->set_hexpand(true);

    m_main_stack->add(*m_content_scrolled_window, "content");

    // Get empty state box for stack child
    m_empty_state_box = builder->get_widget<Gtk::Box>("empty_state_box");

    // Header bar — programmatic since it needs custom buttons
    m_header_bar = Gtk::make_managed<Gtk::HeaderBar>();
    set_titlebar(*m_header_bar);
    m_open_button = Gtk::make_managed<Gtk::Button>();
    m_open_button->set_icon_name("document-open-symbolic");
    m_open_button->set_tooltip_text("Open File");
    m_header_bar->pack_start(*m_open_button);

    // Menu button with application menu
    m_menu_button = Gtk::make_managed<Gtk::MenuButton>();
    m_menu_button->set_icon_name("open-menu-symbolic");
    m_header_bar->pack_end(*m_menu_button);

    // Create MarkdownView and add to content scrolled window
    m_markdown_view = Gtk::make_managed<MarkdownView>();
    m_content_scrolled_window->set_child(*m_markdown_view);

    // Zoom scroll controller on the markdown view
    m_zoom_scroll_controller = Gtk::EventControllerScroll::create();
    m_zoom_scroll_controller->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
    m_zoom_scroll_controller->signal_scroll().connect(
            [this](double dx, double dy) {
                auto state = m_zoom_scroll_controller->get_current_event_state();
                if ((state & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType::CONTROL_MASK || dy == 0) {
                    return false;
                }
                on_zoom_scroll(dx, dy, state);
                return true;
            },
            false);
    m_markdown_view->add_controller(m_zoom_scroll_controller);

    // Create SearchBar and connect to markdown view buffer
    m_search_bar = Gtk::make_managed<SearchBar>();
    m_search_bar->set_buffer(m_markdown_view->get_buffer());
    m_search_bar->set_key_capture_widget(*this);
    m_content_box->prepend(*m_search_bar);

    // Connect header bar open button
    m_connections.emplace_back(
            m_open_button->signal_clicked().connect(sigc::track_obj([this]() { on_open_file(); }, *this)));

    // Setup file dialog
    m_file_dialog = Gtk::FileDialog::create();
    m_file_dialog->set_title("Open Markdown File");

    const auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    const auto filter = Gtk::FileFilter::create();
    filter->set_name("Markdown files");
    filter->add_pattern("*.md");
    filters->append(filter);
    m_file_dialog->set_filters(filters);

    // Setup menu
    const auto menu = Gio::Menu::create();
    menu->append("Open", "win.open");
    menu->append("Search", "win.search");
    const auto zoom_section = Gio::Menu::create();
    zoom_section->append("Zoom In", "win.zoom_in");
    zoom_section->append("Zoom Out", "win.zoom_out");
    zoom_section->append("Zoom Reset", "win.zoom_reset");
    menu->append_section(zoom_section);
    const auto section = Gio::Menu::create();
    section->append("About", "win.about");
    section->append("Quit", "win.close");
    menu->append_section(section);
    m_menu_button->set_menu_model(menu);

    // Get empty state button from builder and connect it
    auto *empty_state_button = builder->get_widget<Gtk::Button>("empty_state_button");
    if (empty_state_button) {
        m_connections.emplace_back(
                empty_state_button->signal_clicked().connect(sigc::track_obj([this]() { on_open_file(); }, *this)));
    }

    // Show empty state initially (no file loaded)
    if (m_empty_state_box && m_main_stack) {
        m_main_stack->set_visible_child(*m_empty_state_box);
    }

    setup_actions();
}

MainWindow::~MainWindow() = default;

void MainWindow::setup_actions() {
    m_app->set_accel_for_action("win.search", {"<Ctrl>f"});
    add_action("search", [this]() { toggle_search(); });
    m_app->set_accel_for_action("win.close", {"<Ctrl>w"});
    add_action("close", [this]() { close(); });
    m_app->set_accel_for_action("win.open", {"<Ctrl>o"});
    add_action("open", [this]() { on_open_file(); });
    m_app->set_accel_for_action("win.about", {"<Alt>F1"});
    add_action("about", [this]() { show_about(); });

    // Zoom actions
    m_app->set_accels_for_action("win.zoom_in", {"<Ctrl>plus", "<Ctrl>KP_Add"});
    add_action("zoom_in", [this]() { on_zoom_in(); });

    m_app->set_accels_for_action("win.zoom_out", {"<Ctrl>minus", "<Ctrl>KP_Subtract"});
    add_action("zoom_out", [this]() { on_zoom_out(); });

    m_app->set_accel_for_action("win.zoom_reset", "<Ctrl>0");
    add_action("zoom_reset", [this]() { on_zoom_reset(); });
}

void MainWindow::on_open_file() {
    m_file_dialog->open(*this, [this](const Glib::RefPtr<Gio::AsyncResult> &result) {
        try {
            const auto file = m_file_dialog->open_finish(result);
            open_file(file->get_path());
        } catch (const Gtk::DialogError &e) {
            // user canceled — not an error
        }
    });
}

void MainWindow::open_file(const Glib::ustring &path) {
    m_markdown_view->load_file(path);
    set_title(Glib::ustring("mdview - ") + Glib::path_get_basename(path.c_str()));
    if (m_main_stack) {
        m_main_stack->set_visible_child(*m_content_scrolled_window);
    }
}

void MainWindow::toggle_search() const {
    const bool current_mode = m_search_bar->get_search_mode();
    m_search_bar->set_search_mode(!current_mode);
}

void MainWindow::close_search() const { m_search_bar->set_search_mode(false); }

void MainWindow::find_next() const { m_search_bar->find_next(); }

void MainWindow::find_previous() const { m_search_bar->find_previous(); }

void MainWindow::on_style_changed() { m_markdown_view->update_colors(); }

void MainWindow::on_zoom_in() { m_markdown_view->set_zoom(m_markdown_view->get_zoom() + 0.1f); }

void MainWindow::on_zoom_out() { m_markdown_view->set_zoom(m_markdown_view->get_zoom() - 0.1f); }

void MainWindow::on_zoom_reset() { m_markdown_view->set_zoom(1.0f); }

void MainWindow::on_zoom_scroll(double /*dx*/, double dy, Gdk::ModifierType state) {
    if ((state & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType::CONTROL_MASK)
        return;

    float current = m_markdown_view->get_zoom();
    if (dy > 0) {
        m_markdown_view->set_zoom(current - 0.1f);
    } else {
        m_markdown_view->set_zoom(current + 0.1f);
    }
}

void MainWindow::show_about() {
#ifdef USE_ADWAITA
    auto *about = static_cast<AdwAboutDialog *>(
            g_object_new(ADW_TYPE_ABOUT_DIALOG, "application-name", s_app_info.name, "application-icon",
                         s_app_info.icon, "version", s_app_info.version, "comments", s_app_info.comments,
                         "license-type", GTK_LICENSE_MIT_X11, "website", s_app_info.website, nullptr));
    adw_dialog_present(ADW_DIALOG(about), GTK_WIDGET(this->gobj()));
#else
    auto about = Gtk::manage(new Gtk::AboutDialog());
    about->set_program_name(s_app_info.name);
    about->set_logo_icon_name(s_app_info.icon);
    about->set_version(s_app_info.version);
    about->set_comments(s_app_info.comments);
    about->set_license_type(Gtk::License::MIT_X11);
    about->set_website(s_app_info.website);
    about->set_transient_for(*this);
    about->set_modal(true);
    about->present();
#endif
}

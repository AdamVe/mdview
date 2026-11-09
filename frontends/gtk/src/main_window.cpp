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

    // Overlay above the viewport hosts the hover copy-code-block button; it
    // must sit outside the scrolled area so buffer_to_window_coords map into it.
    m_text_overlay = Gtk::make_managed<Gtk::Overlay>();
    m_text_overlay->set_child(*m_content_scrolled_window);
    m_toast_label = Gtk::make_managed<Gtk::Label>();
    m_toast_label->add_css_class("caption");
    m_toast_label->set_margin_start(12);
    m_toast_label->set_margin_end(12);
    m_toast_label->set_margin_top(5);
    m_toast_label->set_margin_bottom(5);
    m_toast_box = Gtk::make_managed<Gtk::Box>();
    m_toast_box->add_css_class("osd");
    m_toast_box->add_css_class("rounded");
    m_toast_box->set_halign(Gtk::Align::CENTER);
    m_toast_box->set_valign(Gtk::Align::END);
    m_toast_box->set_margin_bottom(28);
    m_toast_box->append(*m_toast_label);
    m_toast_box->hide();
    m_text_overlay->add_overlay(*m_toast_box);
    m_main_stack->add(*m_text_overlay, "content");

    // Get empty state box for stack child
    m_empty_state_box = builder->get_widget<Gtk::Box>("empty_state_box");

    // Header bar — programmatic since it needs custom buttons
    m_header_bar = Gtk::make_managed<Gtk::HeaderBar>();
    set_titlebar(*m_header_bar);

    // HIG: navigation pair at the start, linked as one control; primary
    // actions and the menu at the end. Action-name binding keeps the buttons
    // greyed until there is history and routes clicks through win.back/forward.
    auto *nav_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    nav_box->add_css_class("linked");
    auto *back_button = Gtk::make_managed<Gtk::Button>();
    back_button->set_icon_name("go-previous-symbolic");
    back_button->set_tooltip_text("Back (Alt+Left / Cmd+[)");
    back_button->set_action_name("win.back");
    nav_box->append(*back_button);
    auto *forward_button = Gtk::make_managed<Gtk::Button>();
    forward_button->set_icon_name("go-next-symbolic");
    forward_button->set_tooltip_text("Forward (Alt+Right / Cmd+])");
    forward_button->set_action_name("win.forward");
    nav_box->append(*forward_button);
    m_header_bar->pack_start(*nav_box);

    m_open_button = Gtk::make_managed<Gtk::Button>();
    m_open_button->set_icon_name("document-open-symbolic");
    m_open_button->set_tooltip_text("Open File");

    // Document outline (table of contents); hidden until a document has headings
    m_toc_button = Gtk::make_managed<Gtk::MenuButton>();
    m_toc_button->set_icon_name("view-list-symbolic");
    m_toc_button->set_tooltip_text("Table of Contents");
    m_toc_button->set_visible(false);
    m_toc_popover = Gtk::manage(new Gtk::Popover());
    m_toc_popover->set_child(*Gtk::make_managed<Gtk::ScrolledWindow>());
    auto &scroller = static_cast<Gtk::ScrolledWindow &>(*m_toc_popover->get_child());
    scroller.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller.set_size_request(380, 420);
    m_toc_list = Gtk::manage(new Gtk::ListBox());
    m_toc_list->set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_toc_list->signal_row_activated().connect([this](Gtk::ListBoxRow *row) {
        const int index = row ? row->get_index() : -1;
        const auto &entries = m_markdown_view->toc();
        if (index >= 0 && static_cast<size_t>(index) < entries.size())
            m_markdown_view->jump_to_anchor(entries[static_cast<size_t>(index)].anchor);
        m_toc_popover->popdown();
    });
    scroller.set_child(*m_toc_list);
    m_toc_popover->signal_show().connect([this]() { sync_toc_selection(); });
    m_toc_button->set_popover(*m_toc_popover);
    m_menu_button = Gtk::make_managed<Gtk::MenuButton>();
    m_menu_button->set_icon_name("open-menu-symbolic");
    // pack_end prepends: menu rightmost, then open, then the outline toggle.
    m_header_bar->pack_end(*m_menu_button);
    m_header_bar->pack_end(*m_open_button);
    m_header_bar->pack_end(*m_toc_button);

    // Create MarkdownView and add to content scrolled window
    m_markdown_view = Gtk::make_managed<MarkdownView>();
    m_content_scrolled_window->set_child(*m_markdown_view);

    // Relative links to other markdown files open in a new window
    m_connections.emplace_back(m_markdown_view->signal_open_file().connect([this](const Glib::ustring &path) {
        auto *window = m_app->create_window();
        window->open_file(path);
        window->present();
    }));

    // Keep header-bar back/forward sensitivity in sync with view history
    m_connections.emplace_back(m_markdown_view->signal_nav_state().connect([this](bool can_back, bool can_forward) {
        m_act_back->set_enabled(can_back);
        m_act_forward->set_enabled(can_forward);
    }));

    // Zoom scroll controller on scrolled window to handle all scroll events
    m_zoom_scroll_controller = Gtk::EventControllerScroll::create();
    m_zoom_scroll_controller->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
    m_zoom_scroll_controller->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
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
    m_content_scrolled_window->add_controller(m_zoom_scroll_controller);

    m_markdown_view->signal_copied().connect([this](const Glib::ustring &message) { show_toast(message); });
    m_markdown_view->signal_loaded().connect([this] { rebuild_toc_popover(); });

    // Create SearchBar and connect to markdown view buffer
    m_search_bar = Gtk::make_managed<SearchBar>();
    m_search_bar->set_buffer(m_markdown_view->get_buffer());
    m_search_bar->set_text_view(m_markdown_view);
    m_search_bar->set_hadjustment(m_content_scrolled_window->get_hadjustment());
    m_search_bar->set_key_capture_widget(*this);
    m_content_box->append(*m_search_bar);

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
    const auto nav_section = Gio::Menu::create();
    nav_section->append("Back", "win.back");
    nav_section->append("Forward", "win.forward");
    menu->append_section(nav_section);
    menu->append("Open", "win.open");
    menu->append("Search", "win.search");
    menu->append("Find Next", "win.find_next");
    menu->append("Find Previous", "win.find_previous");
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
    // Browser conventions per platform: Alt+arrows (Win/Linux), brackets
    // (macOS Cmd+[ / Cmd+] — GDK maps Command to Control; also Ctrl+[ on
    // Win/Linux for free).
    m_app->set_accels_for_action("win.back", {"<Alt>Left", "<Ctrl>bracketleft"});
    m_act_back = add_action("back", [this]() { m_markdown_view->navigate_back(); });
    m_act_back->set_enabled(false); // until a document populates the history
    m_app->set_accels_for_action("win.forward", {"<Alt>Right", "<Ctrl>bracketright"});
    m_act_forward = add_action("forward", [this]() { m_markdown_view->navigate_forward(); });
    m_act_forward->set_enabled(false);
    m_app->set_accel_for_action("win.close", {"<Ctrl>w"});
    add_action("close", [this]() { close(); });
    m_app->set_accel_for_action("win.open", {"<Ctrl>o"});
    add_action("open", [this]() { on_open_file(); });
    m_app->set_accel_for_action("win.about", {"<Alt>F1"});
    add_action("about", [this]() { show_about(); });

    // Zoom actions
    // plus needs Shift on many layouts; '=' is the unshifted twin and the
    // common second binding (matches browser zoom behavior).
    m_app->set_accels_for_action("win.zoom_in", {"<Ctrl>plus", "<Ctrl>KP_Add", "<Ctrl>equal"});
    add_action("zoom_in", [this]() { on_zoom_in(); });

    m_app->set_accels_for_action("win.zoom_out", {"<Ctrl>minus", "<Ctrl>KP_Subtract"});
    add_action("zoom_out", [this]() { on_zoom_out(); });

    m_app->set_accel_for_action("win.zoom_reset", "<Ctrl>0");
    add_action("zoom_reset", [this]() { on_zoom_reset(); });

    m_app->set_accel_for_action("win.find_next", "<Ctrl>g");
    add_action("find_next", [this]() { find_next(); });

    m_app->set_accel_for_action("win.find_previous", "<Ctrl><Shift>g");
    add_action("find_previous", [this]() { find_previous(); });
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
    rebuild_toc_popover();
    if (m_main_stack) {
        m_main_stack->set_visible_child("content");
    }
}

// Repopulates the outline popover from the freshly loaded document; rows are
// indented per heading level and h1/h2 get bold labels.
void MainWindow::rebuild_toc_popover() {
    for (auto *child = m_toc_list->get_first_child(); child != nullptr;) {
        auto *next = child->get_next_sibling();
        m_toc_list->remove(*child);
        child = next;
    }
    const auto &entries = m_markdown_view->toc();
    for (const auto &entry: entries) {
        auto *row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_margin_start((std::max(1, entry.level) - 1) * 16);
        auto *label = Gtk::make_managed<Gtk::Label>(entry.title);
        label->set_xalign(0.0f);
        label->set_ellipsize(Pango::EllipsizeMode::END);
        label->set_max_width_chars(48);
        if (entry.level <= 2)
            label->set_markup("<b>" + Glib::Markup::escape_text(entry.title) + "</b>");
        row->set_child(*label);
        m_toc_list->append(*row);
    }
    m_toc_button->set_visible(!entries.empty());
}

// Highlights and reveals the TOC row for the section currently under the
// viewport top, so reopening the popover continues where the reader is.
void MainWindow::sync_toc_selection() {
    const int index = m_markdown_view->visible_toc_index();
    auto *row = index >= 0 ? m_toc_list->get_row_at_index(index) : nullptr;
    if (row)
        m_toc_list->select_row(*row);
    else
        m_toc_list->unselect_all();
    if (!row)
        return;
    // Rows are only allocated once the popover has mapped; center the row in
    // the scroller from the next idle pass.
    Glib::signal_idle().connect_once([this, row] {
        const auto bounds = row->compute_bounds(*m_toc_list);
        if (!bounds)
            return;
        if (auto *scroller = dynamic_cast<Gtk::ScrolledWindow *>(m_toc_popover->get_child())) {
            if (const auto adj = scroller->get_vadjustment()) {
                const double target = bounds->get_y() + bounds->get_height() / 2.0 - adj->get_page_size() / 2.0;
                adj->set_value(std::clamp(target, adj->get_lower(), adj->get_upper() - adj->get_page_size()));
            }
        }
    });
}

// Brief status pill (GNOME toast style) confirming a clipboard write.
void MainWindow::show_toast(const Glib::ustring &message) {
    if (m_toast_timer)
        m_toast_timer->destroy();
    m_toast_label->set_text(message);
    m_toast_box->set_visible(true);
    m_toast_timer = Glib::TimeoutSource::create(1600);
    m_toast_timer->connect([this] {
        m_toast_box->set_visible(false);
        return false;
    });
    m_toast_timer->attach();
}

void MainWindow::toggle_search() const {
    const bool current_mode = m_search_bar->get_search_mode();
    m_search_bar->set_search_mode(!current_mode);
}

void MainWindow::close_search() const { m_search_bar->set_search_mode(false); }

void MainWindow::find_next() const { m_search_bar->find_next(); }

void MainWindow::find_previous() const { m_search_bar->find_previous(); }

void MainWindow::on_style_changed() {
    m_markdown_view->update_colors();
    if (m_search_bar) {
        m_search_bar->update_highlight_colors();
    }
}

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

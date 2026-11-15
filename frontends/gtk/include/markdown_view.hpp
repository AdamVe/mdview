#pragma once
#include <deque>
#include <flat_map>
#include <flat_set>
#include <functional>
#include <gtkmm.h>
#include <memory>
#include <string>
#include <vector>
#include "mdview/doc_model.hpp"

class MarkdownView : public Gtk::TextView {
public:
    MarkdownView();
    ~MarkdownView() override;

    void load(const mdview::DocModel &model);
    void load_file(const Glib::ustring &path);
    void update_colors();
    void set_zoom(float zoom);
    float get_zoom() const;
    Glib::RefPtr<Gtk::TextBuffer> get_buffer() { return m_buffer; }

    // Document outline for the header-bar table-of-contents popover (built
    // during load(); anchor is the internal key of the heading line).
    struct TocEntry {
        int level = 1;
        Glib::ustring title;
        Glib::ustring anchor;
    };
    const std::vector<TocEntry> &toc() const { return m_toc; }
    void jump_to_anchor(const Glib::ustring &anchor);
    // Index into toc() of the section covering the viewport top (-1 if none).
    int visible_toc_index() const;

    // Code block under widget coordinates (for the hover copy button).
    bool code_block_at_point(double x, double y, int &start, int &end) const;
    const std::vector<std::pair<int, int>> &code_blocks() const { return m_code_blocks; }
    int measure_margin() const { return m_measure_margin; }
    void copy_code_block_range(int start, int end);

    // Bounds of the code block containing iter (buffer offsets); used by the
    // context menu and exposed for unit tests.
    bool code_block_bounds(const Gtk::TextConstIter &iter, int &start, int &end) const;

    // Reconstructs markdown source for a buffer range (context-menu copy);
    // exposed for unit tests.
    Glib::ustring build_markdown(const Gtk::TextConstIter &start, const Gtk::TextConstIter &end) const;

    // Anchor-jump history (Alt+Left / Alt+Right, mouse back/forward buttons)
    void navigate_back();
    void navigate_forward();
    bool can_navigate_back() const { return !m_nav_back.empty(); }
    bool can_navigate_forward() const { return !m_nav_fwd.empty(); }

    // Emitted when a relative link to another markdown file should be opened.
    sigc::signal<void(const Glib::ustring &path)> &signal_open_file() { return m_signal_open_file; }

    // Emitted whenever anchor-history availability changes (header-bar buttons).
    sigc::signal<void(bool can_back, bool can_forward)> &signal_nav_state() { return m_signal_nav_state; }

    // Emitted after anything lands on the clipboard; payload is the toast message.
    sigc::signal<void(const Glib::ustring &message)> &signal_copied() { return m_signal_copied; }

    // Emitted after every successful (re)load, including live-reloads.
    sigc::signal<void()> &signal_loaded() { return m_signal_loaded; }

protected:
    // Keep embedded images fitted to the view when the window is resized
    void size_allocate_vfunc(int width, int height, int baseline) override;
    // Paint embedded images over the text window at their anchor positions
    void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot> &snapshot) override;

private:
    enum class LineKind { Paragraph, Heading, CodeLang, CodeLine, BlockQuote, HRule, ListItem, TableLine };
    struct LineInfo {
        LineKind kind = LineKind::Paragraph;
        std::string info; // heading level or code fence language
    };

    void setup_tags();
    void add_tag(const Glib::ustring &name, const Glib::ustring &properties) const;
    Glib::RefPtr<Gtk::TextTag> grid_fix_tag(const Glib::ustring &family, int scale_milli, int rise_pu = 0);
    void insert_inline(const mdview::TextRun &tr, Gtk::TextIter &iter, Glib::RefPtr<Gtk::TextTag> tag = nullptr) const;
    void insert_inline(const mdview::Link &l, Gtk::TextIter &iter, const Glib::RefPtr<Gtk::TextTag> &tag = nullptr);
    void insert_inline(const mdview::Image &img, Gtk::TextIter &iter, const Glib::RefPtr<Gtk::TextTag> &tag = nullptr);

    // Images: resolved/decoded pixbufs cached per document load
    Glib::RefPtr<Gdk::Pixbuf> load_image(const std::string &url);
    static bool is_remote_url(const std::string &url);
    // Images fetched only after an explicit click: remote urls, plus local SVG
    // (whose loader resolves external references on its own).
    static bool needs_click_to_load(const std::string &url);
    double image_scale_for(int natural_width) const;
    void rescale_images();
    int base_line_height();
    Glib::RefPtr<Gtk::TextTag> image_line_tag(unsigned id, int nat_w, int nat_h);
    void apply_image_line_height(Gtk::TextTag &tag, int rendered_height_px);
    void draw_placeholder(const Glib::RefPtr<Gtk::Snapshot> &snapshot, int x, int y, int w, int h, bool failed,
                          const std::string &url);

    // Node insertion functions (must be declared before use in lambda)
    void insert_node(const mdview::Heading &h, Gtk::TextIter &iter);
    void insert_node(const mdview::Paragraph &p, Gtk::TextIter &iter);
    void insert_node(const mdview::CodeBlock &cb, Gtk::TextIter &iter);
    void watch_current_file();
    void reload_preserving_view();
    bool visible_offset_range(int &top, int &bottom) const;
    bool copy_icon_rect(int block_index, Gdk::Rectangle &out) const;
    int copy_icon_at_point(double x, double y) const;
    void paint_copy_icons(const Glib::RefPtr<Gtk::Snapshot> &snapshot);
    void paint_quote_bars(const Glib::RefPtr<Gtk::Snapshot> &snapshot);
    int ink_overhang_px();
    int quote_pad();
    void insert_node(const mdview::BlockQuote &bq, Gtk::TextIter &iter);
    void insert_node(const mdview::HRule &hr, Gtk::TextIter &iter) const;
    Glib::RefPtr<Gtk::TextTag> quote_tag(int depth);
    void insert_node(const mdview::List &list, Gtk::TextIter &iter);
    void insert_list_entries(const std::vector<mdview::ListEntry> &entries, Gtk::TextIter &iter, int depth);
    Glib::RefPtr<Gtk::TextTag> list_tag(int depth);
    void insert_node(const mdview::Table &t, Gtk::TextIter &iter);

    void apply_zoom();

    // Zoom-independent bases of table run tags (grid fixes and padding
    // spaces). Live `rise` is absolute Pango units, so it must be recomputed
    // from the base whenever the zoom factor changes; live `scale` needs no
    // refresh because the buffer-wide zoom tag multiplies it.
    struct GridTagBase {
        Glib::RefPtr<Gtk::TextTag> tag;
        int scale_milli = 1000;
        int rise_pu = 0;
    };
    std::vector<GridTagBase> m_grid_tags;
    void setup_color_detection();

    // Typography: measure column + vertical rhythm (spacing stored in em of
    // the base line height so it can be refreshed on zoom, like GridTagBase).
    struct RhythmTag {
        Glib::RefPtr<Gtk::TextTag> tag;
        double above_em = 0.0;
        double below_em = 0.0;
    };
    std::vector<RhythmTag> m_rhythm_tags;
    Glib::RefPtr<Gtk::TextTag> m_measure_tag;
    mutable Glib::ustring m_overhang_key; // cache key for ink_overhang_px()
    mutable int m_overhang_px = 0;
    int m_measure_margin = 0; // fixed left gutter once allocated
    int m_measure_margin_right = 0; // dynamic right margin enforcing the line-length cap
    void update_typographic_spacing();
    bool update_measure_margins(); // true when the margin changed
    void force_full_relayout();
    void style_block_range(int start, int end, const mdview::DocNode &node);
    void strip_body_from_image_markers();
    void update_code_backgrounds() const;
    void update_link_colors() const;
    void setup_search_colors();
    void update_search_colors();
    void update_list_indent();

    // Links
    Glib::RefPtr<Gtk::TextTag> get_link_tag(const std::string &url);
    static Glib::ustring url_at(const Gtk::TextConstIter &iter);
    void follow_link(const Glib::ustring &url);
    void launch_uri(const std::string &uri);
    void update_nav_actions();
    void clear_dynamic_state();

    // Block map (buffer line start offset -> block info), rebuilt on every load()
    void record_lines(int start, int end, LineKind kind, const std::string &info = {});
    void record_block(int start, int end, const mdview::DocNode &node);
    LineInfo line_info_at(int line_start) const;

    // Interaction
    Gtk::TextConstIter iter_at_xy(double x, double y) const;
    void setup_gestures();
    void setup_context_menu();
    void show_context_menu(double x, double y);
    void on_primary_pressed(int n_press, double x, double y);
    void on_primary_released(int n_press, double x, double y);
    void on_hover_motion(double x, double y);
    void on_hover_leave();

    // Copy helpers
    bool selection_bounds(Gtk::TextConstIter &start, Gtk::TextConstIter &end) const;
    void set_selection(int start, int end);
    Glib::RefPtr<Gdk::Clipboard> clip() const;
    void copy_selection_text();
    void copy_selection_markdown();
    void select_all_buffer();
    Glib::ustring extract_code_text(int start, int end) const;
    Glib::ustring build_inline_markdown(int start_offset, int end_offset) const;

    Glib::RefPtr<Gtk::TextBuffer> m_buffer;
    Glib::RefPtr<Gtk::TextTagTable> m_tag_table;
    Glib::RefPtr<Gtk::TextTag> m_zoom_tag;
    float m_zoom = 1.0f;
    bool m_dark_mode = false;
    bool m_has_content = false;
    int m_base_indent = 8;
    int m_last_alloc_width = 0;

    std::flat_map<int, LineInfo> m_line_info;
    std::flat_map<std::string, Glib::RefPtr<Gtk::TextTag>> m_link_tags;
    unsigned m_link_tag_counter = 0;
    std::flat_set<std::string> m_slugs;
    std::vector<TocEntry> m_toc;
    std::map<Glib::ustring, int> m_anchor_offsets; // anchor name -> buffer offset
    std::vector<std::pair<int, int>> m_code_blocks;
    Glib::ustring m_current_path;

    // Images painted in snapshot_vfunc: anchor mark + natural pixbuf + cached texture.
    // A null pixbuf with a remote url is a click-to-load placeholder (failed = last
    // attempt errored; clicking retries).
    struct ImagePlacement {
        int offset; // buffer offset of the zero-width marker line
        std::string url;
        std::string alt;
        Glib::RefPtr<Gdk::Pixbuf> pixbuf;
        mutable Glib::RefPtr<Gdk::Texture> texture;
        bool failed = false;
    };
    std::vector<ImagePlacement> m_images;
    bool image_rect_buffer(const ImagePlacement &img, Gdk::Rectangle &out) const;
    int pending_image_at_point(double x, double y) const;
    const ImagePlacement *image_at_anchor(const Gtk::TextIter &iter) const;
    void load_remote_image(size_t index);
    // Line-stretch tags backing each embedded image's reserved height (parallel to m_images)
    struct ImgLine {
        Glib::RefPtr<Gtk::TextTag> tag;
        int nat_w = 0;
        int nat_h = 0;
    };
    std::vector<ImgLine> m_imgline_tags;
    std::flat_map<std::string, Glib::RefPtr<Gdk::Pixbuf>> m_image_cache;
    std::deque<std::string> m_image_cache_order; // FIFO eviction order
    size_t m_image_cache_bytes = 0; // decoded bytes held by the cache
    unsigned m_image_counter = 0;

    // Scroll positions of anchor jumps (back stack) and undone jumps (forward)
    std::vector<double> m_nav_back;
    std::vector<double> m_nav_fwd;

    Gtk::PopoverMenu *m_ctx_menu = nullptr;
    Glib::RefPtr<Gio::SimpleActionGroup> m_ctx_actions;
    Glib::RefPtr<Gio::SimpleAction> m_act_copy;
    Glib::RefPtr<Gio::SimpleAction> m_act_copy_md;
    Glib::RefPtr<Gio::SimpleAction> m_act_back;
    Glib::RefPtr<Gio::SimpleAction> m_act_fwd;
    Gtk::TextConstIter m_ctx_iter;

    Glib::RefPtr<Gtk::EventControllerMotion> m_motion_controller;
    Glib::RefPtr<Gtk::GestureClick> m_click_gesture;
    Glib::RefPtr<Gtk::GestureClick> m_ctx_gesture;
    bool m_pointer_over_link = false;
    double m_press_x = 0.0;
    double m_press_y = 0.0;

    sigc::signal<void(const Glib::ustring &path)> m_signal_open_file;
    sigc::signal<void(bool, bool)> m_signal_nav_state;
    sigc::signal<void(const Glib::ustring &)> m_signal_copied;
    sigc::signal<void()> m_signal_loaded;
    Glib::RefPtr<Gio::FileMonitor> m_file_monitor;
    Glib::RefPtr<Glib::TimeoutSource> m_reload_timer;

    std::vector<sigc::connection> m_connections;
};

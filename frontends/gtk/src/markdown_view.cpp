#include "markdown_view.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <gio/gio.h>
#include <gtkmm.h>
#include <iomanip>
#include <iostream>
#include <libsoup/soup.h>
#include <print>
#include <sstream>
#include "mdview/parser.hpp"
#ifdef USE_ADWAITA
#include <adwaita.h>
#endif

extern "C" {
#include "mdview-resources.h"
}

namespace {
    constexpr const char *kUrlTagPrefix = "url#";
    constexpr const char *kUrlDataKey = "mdview-url";
    constexpr const char *kAnchorPrefix = "mdview-anchor:";

    // GitHub-style anchor slug: lowercase alphanumerics, spaces to dashes, drop the rest
    std::string slugify(const std::string &text) {
        std::string out;
        for (gunichar ch: Glib::ustring(text)) {
            if (ch == ' ' || ch == '\t') {
                out += '-';
            } else if (g_unichar_isalnum(ch)) {
                char buf[8];
                const int n = g_unichar_to_utf8(g_unichar_tolower(ch), buf);
                out.append(buf, n);
            } else if (ch == '-' || ch == '_') {
                out += static_cast<char>(ch);
            }
        }
        return out;
    }

    bool is_box_char(gunichar ch) {
        static constexpr gunichar box[] = {0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518,
                                           0x251C, 0x2524, 0x252C, 0x2534, 0x253C};
        return std::find(std::begin(box), std::end(box), ch) != std::end(box);
    }

    std::string trim_ws(const std::string &s) {
        const auto b = s.find_first_not_of(" \t");
        if (b == std::string::npos)
            return {};
        const auto e = s.find_last_not_of(" \t");
        return s.substr(b, e - b + 1);
    }

    // True when url begins with a scheme like "https:", "mailto:", "file:"
    bool has_uri_scheme(const std::string &url) {
        for (size_t i = 0; i < url.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(url[i]);
            if (c == ':')
                return i > 0;
            if (!(std::isalnum(c) || c == '+' || c == '-' || c == '.'))
                return false;
        }
        return false;
    }

    std::string to_lower_ascii(std::string s) {
        for (char &c: s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // Read an entire stream into memory, refusing anything past max_bytes so a
    // hostile http:// response cannot exhaust RAM before gdk-pixbuf sees it.
    // Decoded-pixbuf cache budget: enough for hundreds of full-size images,
    // low enough that a crafted document listing thousands of distinct
    // pictures cannot exhaust memory (oldest entries evict first).
    constexpr size_t kImageCacheBudget = 192ull * 1024 * 1024;

    std::vector<uint8_t> read_capped(const Glib::RefPtr<Gio::InputStream> &stream, guint64 max_bytes) {
        std::vector<uint8_t> out;
        uint8_t buf[64 * 1024];
        gssize n = 0;
        while ((n = stream->read(buf, sizeof(buf))) > 0) {
            if (out.size() + static_cast<size_t>(n) > max_bytes)
                throw std::runtime_error("image larger than 64 MiB");
            out.insert(out.end(), buf, buf + n);
        }
        return out;
    }

    // Synchronous GET with libsoup (redirects followed by default, 30 s I/O
    // timeout). Gio routes http(s) through gvfs helpers on the session bus,
    // which Flatpak sandboxes do not ship — fetching directly works everywhere.
    std::vector<uint8_t> fetch_http(const std::string &url, guint64 max_bytes) {
        g_autoptr(SoupSession) session = soup_session_new();
        g_object_set(session, "timeout", 30u, nullptr);
        g_autoptr(GError) error = nullptr;
        g_autoptr(SoupMessage) msg = soup_message_new(SOUP_METHOD_GET, url.c_str());
        if (!msg)
            throw std::invalid_argument("invalid image URL");
        GInputStream *body = soup_session_send(session, msg, nullptr, &error);
        if (!body)
            throw std::runtime_error(std::string("image download failed: ") +
                                     (error ? error->message : "unknown error"));
        const guint status = soup_message_get_status(msg);
        if (status < 200 || status >= 300) {
            g_object_unref(body);
            throw std::runtime_error("image download failed: HTTP " + std::to_string(status));
        }
        return read_capped(Glib::wrap(body), max_bytes); // wrap adopts the full ref
    }

    // Decompression-bomb guard: read natural pixel dimensions from the header
    // of common formats so decoding can be downscaled up front. Returns
    // {0, 0} when the format is unknown or the header looks truncated.
    std::pair<int, int> sniff_image_size(const uint8_t *d, size_t n) {
        // Network-order (big-endian) and little-endian reads via std::byteswap,
        // which compilers fold straight into a bswap/movbe instruction.
        static_assert(std::endian::native == std::endian::little, "header sniffing assumes a little-endian host");
        const auto be16 = [d](size_t i) {
            std::uint16_t v;
            std::memcpy(&v, d + i, sizeof(v));
            return static_cast<int>(std::byteswap(v));
        };
        const auto le16 = [d](size_t i) {
            std::uint16_t v;
            std::memcpy(&v, d + i, sizeof(v));
            return static_cast<int>(v);
        };
        if (n >= 24 && std::memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0 && std::memcmp(d + 12, "IHDR", 4) == 0)
            return {be16(16) * 65536 + be16(18), be16(20) * 65536 + be16(22)};
        if ((n >= 10 && std::memcmp(d, "GIF87a", 6) == 0) || (n >= 10 && std::memcmp(d, "GIF89a", 6) == 0))
            return {le16(6), le16(8)};
        if (n >= 26 && d[0] == 'B' && d[1] == 'M') {
            const int w = d[18] | (d[19] << 8) | (d[20] << 16) | (d[21] << 24);
            const int h = std::abs(d[22] | (d[23] << 8) | (d[24] << 16) | (d[25] << 24));
            return {w, h};
        }
        if (n >= 30 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0) {
            if (std::memcmp(d + 12, "VP8X", 4) == 0) {
                const int w = (d[24] | (d[25] << 8) | (d[26] << 16)) + 1;
                const int h = (d[27] | (d[28] << 8) | (d[29] << 16)) + 1;
                return {w, h};
            }
            if (std::memcmp(d + 12, "VP8L", 4) == 0 && d[20] == 0x2F) {
                const uint32_t v = d[21] | (d[22] << 8) | (d[23] << 16) | (static_cast<uint32_t>(d[24]) << 24);
                return {static_cast<int>(v & 0x3FFF) + 1, static_cast<int>((v >> 14) & 0x3FFF) + 1};
            }
        }
        if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) { // JPEG: walk marker segments to an SOFn
            size_t i = 2;
            while (i + 9 < n) {
                if (d[i] != 0xFF) {
                    ++i;
                    continue;
                }
                const uint8_t m = d[i + 1];
                if (m == 0xFF) {
                    ++i;
                    continue;
                }
                if (m == 0x01 || (m >= 0xD0 && m <= 0xD9)) {
                    i += 2;
                    continue;
                }
                const bool sof = (m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) || (m >= 0xC9 && m <= 0xCB) ||
                                 (m >= 0xCD && m <= 0xCF);
                if (sof)
                    return {be16(i + 7), be16(i + 5)};
                i += 2 + be16(i + 2);
            }
        }
        return {};
    }
} // namespace

MarkdownView::MarkdownView() {
    set_expand(true);

    // Load CSS from resource
    const auto css_provider = Gtk::CssProvider::create();
    try {
        GResource *resource = mdview_get_resource();
        g_resource_ref(resource);
        GError *error = nullptr;
        if (GBytes *bytes = g_resource_lookup_data(resource, "/com/adamve/mdview/style.css",
                                                   G_RESOURCE_LOOKUP_FLAGS_NONE, &error)) {
            gsize size = 0;
            if (const auto data = static_cast<const char *>(g_bytes_get_data(bytes, &size)); data && size > 0) {
                const std::string css_data(data, size);
                css_provider->load_from_data(css_data);
            }
            g_bytes_unref(bytes);
        }
        if (error) {
            g_error_free(error);
        }
    } catch (...) {
    }

    // Create text buffer for markdown content
    m_tag_table = Gtk::TextTagTable::create();
    m_buffer = Gtk::TextBuffer::create(m_tag_table);
    setup_tags();

    set_buffer(m_buffer);
    set_editable(false);
    set_cursor_visible(false);

    // Enable word wrapping (table_cell tag overrides this to NONE)
    set_wrap_mode(Gtk::WrapMode::WORD_CHAR);

    // Full pixel hinting snaps glyph stems onto pixel boundaries. Without it,
    // sub-pixel pen placement splits thin box-drawing stems (│ ─) across two
    // half-lit columns, so stacked table borders read as dashed hairlines.
    {
        auto *pc = gtk_widget_get_pango_context(GTK_WIDGET(gobj()));
        if (cairo_font_options_t *fo = cairo_font_options_create()) {
            cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
            cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
            pango_cairo_context_set_font_options(pc, fo);
            pango_context_changed(pc);
            cairo_font_options_destroy(fo);
        }
    }

    add_css_class("markdown-view");

    // Apply base CSS to text view
    get_style_context()->add_provider_for_display(Gdk::Display::get_default(), css_provider,
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    setup_color_detection();
    setup_search_colors();
    setup_context_menu();
    setup_gestures();

    m_zoom_tag = m_tag_table->lookup("zoom");
    set_top_margin(16);
    set_bottom_margin(32);
    apply_zoom();
}

void MarkdownView::set_zoom(float zoom) {
    m_zoom = std::clamp(zoom, 0.5f, 1.9f);
    apply_zoom();
}

float MarkdownView::get_zoom() const { return m_zoom; }

void MarkdownView::apply_zoom() {
    if (m_zoom_tag) {
        m_zoom_tag->property_scale() = m_zoom;
    }

    // Grid-fix scales are NOT refreshed here: the buffer-wide zoom tag already
    // multiplies them, so their width correction holds at any zoom. Only `rise`
    // is absolute (Pango units) and must be rescaled by hand.
    for (const auto &g: m_grid_tags) {
        if (g.rise_pu != 0)
            g.tag->property_rise() = static_cast<int>(std::lround(g.rise_pu * static_cast<double>(m_zoom)));
    }

    update_list_indent();
    update_typographic_spacing();
    update_measure_margins();
    rescale_images();

    // Margins/scales above only take visible effect once the tags have been
    // touched on the buffer; property edits alone leave cached layouts stale.
    force_full_relayout();
}

// GtkTextView caches per-line layout geometry; changing a tag's margin or
// scale property does NOT dirty the lines referencing it. Removing and
// re-applying the buffer-wide tags rewrites line attributes and forces a
// complete relayout.
void MarkdownView::force_full_relayout() {
    if (!m_buffer || !m_has_content)
        return;
    const auto start = m_buffer->begin();
    const auto end = m_buffer->end();
    m_buffer->remove_tag(m_zoom_tag, start, end);
    m_buffer->apply_tag(m_zoom_tag, start, end);
    if (m_measure_tag) {
        m_buffer->remove_tag(m_measure_tag, start, end);
        m_buffer->apply_tag(m_measure_tag, start, end);
    }
}

MarkdownView::~MarkdownView() = default;

void MarkdownView::size_allocate_vfunc(int width, int height, int baseline) {
    Gtk::TextView::size_allocate_vfunc(width, height, baseline);
    if (width != m_last_alloc_width) {
        m_last_alloc_width = width;
        if (update_measure_margins())
            force_full_relayout(); // tag property edits alone do not invalidate cached line layouts
        rescale_images();
    }
}

void MarkdownView::setup_tags() {
    // Heading tags
    add_tag("h1", "scale=2.0;weight=bold;pixels-above=12;pixels-below=6;");
    add_tag("h2", "scale=1.6;weight=bold;pixels-above=10;pixels-below=4;");
    add_tag("h3", "scale=1.3;weight=bold;pixels-above=8;pixels-below=4;");
    add_tag("h4", "scale=1.1;weight=bold;");

    // Inline tags
    add_tag("bold", "weight=bold;");
    add_tag("italic", "style=italic;");
    add_tag("strikethrough", "strikethrough=true;");
    add_tag("inline_code", "family=monospace;scale=1.1;");
    add_tag("code_block", "family=monospace;line-height=1;scale=1.1;");
    add_tag("code_block_padding_top", "family=monospace;line-height=1;");
    add_tag("code_block_padding_bottom", "family=monospace;line-height=1;");
    add_tag("code_block_language", "family=monospace;weight=bold;scale=0.8;");

    // Block tags
    add_tag("blockquote", "left-margin=8;"); // no first-line indent: quotes have no marker to align under
    add_tag("link", "foreground=blue;underline=single;");
    add_tag("hr", "wrap-mode=none;");
    add_tag("bullet", "indent=8;left-margin=8;");
    add_tag("search_highlight", "background=#ffff00;");
    add_tag("search_highlight_current", "background=#ff9500;");
    add_tag("table_cell", "line-height=1;");

    // Zoom tag — scale applied to all text for per-window zoom
    add_tag("zoom", "");
    // Measure tag — page margins applied to all text (tag margins override
    // the view margin, so per-family tags must add the measure themselves).
    add_tag("measure", "");
    m_measure_tag = m_tag_table->lookup("measure");

    // Vertical rhythm (em of base line height): space above headings exceeds
    // space below so sections group visually; paragraphs get one blank-ish
    // gap, list items stay tight within their list.
    const auto rhythm = [this](const char *name, double above, double below) {
        if (const auto tag = m_tag_table->lookup(name))
            m_rhythm_tags.push_back({tag, above, below});
    };
    rhythm("h1", 1.30, 0.50);
    rhythm("h2", 1.10, 0.40);
    rhythm("h3", 0.90, 0.30);
    rhythm("h4", 0.80, 0.25);
    rhythm("hr", 1.00, 1.00);
    rhythm("para_gap", 0.0, 0.75);
    rhythm("list_gap", 0.0, 0.12);
    rhythm("quote_gap", 0.0, 0.60);
}

// Absolute px spacings must be recomputed whenever zoom changes.
void MarkdownView::update_typographic_spacing() {
    const int base_h = base_line_height();
    if (base_h <= 0)
        return;
    for (const auto &r: m_rhythm_tags) {
        r.tag->property_pixels_above_lines() = static_cast<int>(std::lround(r.above_em * base_h * m_zoom));
        r.tag->property_pixels_below_lines() = static_cast<int>(std::lround(r.below_em * base_h * m_zoom));
    }
}

// Book measure: keep a constant left gutter and cap line length near 78
// characters so long lines stay readable on wide windows (60-90 chars is the
// classic range). The cap grows only the RIGHT margin; the left edge never
// moves, so resizing the window does not slide the text sideways.
bool MarkdownView::update_measure_margins() {
    const int width = get_allocated_width();
    if (width < 200)
        return false;
    const auto layout = create_pango_layout("M");
    int adv = 0;
    int h = 0;
    layout->get_pixel_size(adv, h);
    if (adv <= 0)
        return false;
    // The whole measure scales with zoom so the layout stays proportional to
    // the glyphs: the gutter grows with the text and the ~78-character cap
    // holds at every zoom level.
    constexpr int kBaseLeftMargin = 48;
    constexpr int kMinRightMargin = 24;
    const int left = std::max(24, static_cast<int>(std::lround(kBaseLeftMargin * m_zoom)));
    const int target = static_cast<int>(78.0 * adv * m_zoom);
    const int right = std::max(kMinRightMargin, width - left - target);
    if (right == m_measure_margin_right && m_measure_margin == left)
        return false;
    m_measure_margin = left;
    m_measure_margin_right = right;
    if (m_measure_tag) {
        m_measure_tag->property_left_margin() = left;
        m_measure_tag->property_right_margin() = right;
    }
    // Tags carrying their own left-margin override the measure tag; keep them
    // at measure + their design indent so every column starts aligned.
    for (const char *name: {"code_block", "code_block_padding_top", "code_block_padding_bottom", "table_cell"}) {
        if (const auto tag = m_tag_table->lookup(name))
            tag->property_left_margin() = left;
    }
    update_list_indent();
    rescale_images();
    return true;
}

Glib::RefPtr<Gtk::TextTag> MarkdownView::grid_fix_tag(const Glib::ustring &family, int scale_milli, int rise_pu) {
    // scale_milli is zoom-independent: the buffer-wide zoom tag multiplies it.
    // rise_pu is a base in Pango units and needs explicit rescaling (apply_zoom).
    // Exact values: coarser snapping saved tags but let sub-pixel drift
    // accumulate across a row's cells into visible border misalignment.
    const Glib::ustring name =
            "mdview-gridfix#" + family + "#" + std::to_string(scale_milli) + "#" + std::to_string(rise_pu);
    if (const auto existing = m_tag_table->lookup(name))
        return existing;
    const auto tag = Gtk::TextTag::create(name);
    tag->property_family() = family;
    tag->property_scale() = scale_milli / 1000.0;
    if (rise_pu != 0)
        tag->property_rise() = static_cast<int>(std::lround(rise_pu * static_cast<double>(m_zoom)));
    m_tag_table->add(tag);
    m_grid_tags.push_back({.tag = tag, .scale_milli = scale_milli, .rise_pu = rise_pu});
    return tag;
}

void MarkdownView::add_tag(const Glib::ustring &name, const Glib::ustring & /*properties*/) const {
    const auto tag = Gtk::TextTag::create(name);
    // Heading scales follow a ~1.26 (major-third-ish) modular chain from h4
    // up to h1; generous above-tight-below spacing comes from m_rhythm_tags.
    if (name == "h1") {
        tag->property_scale() = 2.0;
        tag->property_weight() = Pango::Weight::BOLD;
        tag->property_line_height() = 1.1;
    } else if (name == "h2") {
        tag->property_scale() = 1.59;
        tag->property_weight() = Pango::Weight::BOLD;
        tag->property_line_height() = 1.1;
    } else if (name == "h3") {
        tag->property_scale() = 1.26;
        tag->property_weight() = Pango::Weight::BOLD;
        tag->property_line_height() = 1.1;
    } else if (name == "h4") {
        tag->property_scale() = 1.13;
        tag->property_weight() = Pango::Weight::BOLD;
        tag->property_line_height() = 1.1;
    } else if (name == "body") {
        // Reading measure leading for prose (Bringhurst: ~1.4-1.5).
        tag->property_line_height() = 1.45;
    } else if (name == "para_gap") {
        tag->property_pixels_below_lines() = 12;
    } else if (name == "list_gap") {
        tag->property_pixels_below_lines() = 2;
    } else if (name == "quote_gap") {
        tag->property_pixels_below_lines() = 10;
    } else if (name == "bold") {
        tag->property_weight() = Pango::Weight::BOLD;
    } else if (name == "italic") {
        tag->property_style() = Pango::Style::ITALIC;
    } else if (name == "strikethrough") {
        tag->property_strikethrough() = true;
    } else if (name == "inline_code") {
        tag->property_family() = "monospace"; // deliberately unscaled: 1.1 read as oversized next to prose
    } else if (name == "code_block") {
        tag->property_family() = "monospace";
        tag->property_scale() = 1.1;
        tag->property_left_margin() = 0;
        tag->property_pixels_above_lines() = 0;
        tag->property_pixels_below_lines() = 0;
        tag->property_line_height() = 1.0;
        tag->property_wrap_mode() = Gtk::WrapMode::NONE;
    } else if (name == "code_block_padding_top") {
        tag->property_family() = "monospace";
        tag->property_scale() = 1.1;
        tag->property_left_margin() = 0;
        tag->property_pixels_above_lines() = 4;
        tag->property_pixels_below_lines() = 0;
        tag->property_line_height() = 1.0;
        tag->property_wrap_mode() = Gtk::WrapMode::NONE;
    } else if (name == "code_block_padding_bottom") {
        tag->property_family() = "monospace";
        tag->property_scale() = 1.1;
        tag->property_left_margin() = 0;
        tag->property_pixels_above_lines() = 0;
        tag->property_pixels_below_lines() = 8;
        tag->property_line_height() = 1.0;
        tag->property_wrap_mode() = Gtk::WrapMode::NONE;
    } else if (name == "code_block_language") {
        tag->property_family() = "monospace";
        tag->property_weight() = Pango::Weight::BOLD;
        tag->property_scale() = 0.8;
    } else if (name == "table_cell") {
        tag->property_family() = "monospace";
        tag->property_line_height() = 1.0;
        tag->property_wrap_mode() = Gtk::WrapMode::NONE;
    } else if (name == "blockquote") {
        tag->property_left_margin() = 8;
    } else if (name == "link") {
        tag->property_foreground() = "#0000ff";
        tag->property_underline() = Pango::Underline::SINGLE;
    } else if (name == "hr") {
        tag->property_wrap_mode() = Gtk::WrapMode::NONE;
    } else if (name == "bullet") {
        tag->property_left_margin() = 24;
        tag->property_indent() = 24;
    } else if (name == "search_highlight") {
        tag->property_background() = "#ffff00";
    } else if (name == "search_highlight_current") {
        tag->property_background() = "#ff9500";
    }
    m_tag_table->add(tag);
}

void MarkdownView::clear_dynamic_state() {
    m_anchor_offsets.clear();
    for (const auto &[url, tag]: m_link_tags) {
        m_tag_table->remove(tag);
    }
    m_link_tags.clear();
    m_line_info.clear();
    m_slugs.clear();
    m_toc.clear();
    m_link_tag_counter = 0;
    m_images.clear();
    for (auto &[tag, w, h]: m_imgline_tags)
        m_tag_table->remove(tag);
    m_imgline_tags.clear();
    m_image_counter = 0;
    m_code_blocks.clear();
    m_nav_back.clear();
    m_nav_fwd.clear();
    update_nav_actions();
}

void MarkdownView::record_lines(int start, int end, LineKind kind, const std::string &info) {
    Gtk::TextIter it = m_buffer->get_iter_at_offset(start);
    it.set_line_offset(0);
    while (it.get_offset() < end) {
        m_line_info[it.get_offset()] = LineInfo{kind, info};
        if (!it.forward_line())
            break;
    }
}

// One line per list entry, in insertion order (children immediately follow
// their parent), tagged "<kind><depth><x|->" so copy-as-Markdown can rebuild
// nesting, ordered/bullet/task kind and checkbox state.
void MarkdownView::record_list_lines(const std::vector<mdview::ListEntry> &entries, Gtk::TextIter &it, int depth) {
    for (const auto &entry: entries) {
        std::string meta{entry.kind};
        meta += std::to_string(depth);
        meta += entry.checked ? 'x' : '-';
        m_line_info[it.get_offset()] = LineInfo{LineKind::ListItem, meta};
        it.forward_line();
        record_list_lines(entry.children, it, depth + 1);
    }
}

void MarkdownView::record_block(int start, int end, const mdview::DocNode &node) {
    std::visit(
            [this, start, end]<typename T>(const T &n) {
                using U = std::decay_t<T>;
                if constexpr (std::is_same_v<U, mdview::Heading>) {
                    record_lines(start, end, LineKind::Heading, std::to_string(std::min(n.level, 4)));
                } else if constexpr (std::is_same_v<U, mdview::Paragraph>) {
                    record_lines(start, end, LineKind::Paragraph);
                } else if constexpr (std::is_same_v<U, mdview::CodeBlock>) {
                    if (!n.language.empty()) {
                        Gtk::TextIter nl = m_buffer->get_iter_at_offset(start);
                        if (nl.forward_to_line_end()) {
                            const int after_lang = nl.get_offset() + 1;
                            record_lines(start, after_lang, LineKind::CodeLang, n.language);
                            record_lines(after_lang, end, LineKind::CodeLine);
                            return;
                        }
                    }
                    record_lines(start, end, LineKind::CodeLine);
                } else if constexpr (std::is_same_v<U, mdview::BlockQuote>) {
                    record_lines(start, end, LineKind::BlockQuote, std::to_string(n.depth));
                } else if constexpr (std::is_same_v<U, mdview::HRule>) {
                    record_lines(start, end, LineKind::HRule);
                } else if constexpr (std::is_same_v<U, mdview::List>) {
                    Gtk::TextIter it = m_buffer->get_iter_at_offset(start);
                    it.set_line_offset(0);
                    record_list_lines(n.items, it, 0);
                } else if constexpr (std::is_same_v<U, mdview::Table>) {
                    record_lines(start, end, LineKind::TableLine);
                }
            },
            node);
}

MarkdownView::LineInfo MarkdownView::line_info_at(int line_start) const {
    const auto it = m_line_info.find(line_start);
    return it != m_line_info.end() ? it->second : LineInfo{};
}

void MarkdownView::load(const mdview::DocModel &model) {
    const bool perf = g_getenv("MDVIEW_DEBUG_PERF") != nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    clear_dynamic_state();
    m_buffer->set_text("");
    Gtk::TextIter iter = m_buffer->end();
    for (const auto &node: model) {
        const int start = iter.get_offset();
        std::visit([this, &iter](const auto &n) { insert_node(n, iter); }, node);
        style_block_range(start, iter.get_offset(), node);
        record_block(start, iter.get_offset(), node);
    }
    strip_body_from_image_markers();
    // Freshly inserted text carries no zoom tag; restore the active zoom.
    apply_zoom();
    if (perf) {
        const long long ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[perf] render %zu blocks %lldms\n", model.size(), ms);
    }
    m_has_content = !model.empty();
}

// Prose blocks get comfortable leading plus their kind's trailing gap; code,
// tables and rules keep their own compact metrics untouched.
void MarkdownView::style_block_range(int start, int end, const mdview::DocNode &node) {
    if (end <= start)
        return;
    const auto s = m_buffer->get_iter_at_offset(start);
    const auto e = m_buffer->get_iter_at_offset(end);
    const auto apply = [this, &s, &e](const char *name) {
        if (const auto tag = m_tag_table->lookup(name))
            m_buffer->apply_tag(tag, s, e);
    };
    std::visit(
            [&](const auto &n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, mdview::Paragraph>) {
                    apply("body");
                    apply("para_gap");
                } else if constexpr (std::is_same_v<T, mdview::List>) {
                    apply("body");
                    apply("list_gap");
                } else if constexpr (std::is_same_v<T, mdview::BlockQuote>) {
                    apply("body");
                    apply("quote_gap");
                }
            },
            node);
}

// Image marker lines carry an exact line-height factor; body leading would
// multiply onto it and break the reserved height.
void MarkdownView::strip_body_from_image_markers() {
    const std::array<const char *, 4> names = {"body", "para_gap", "list_gap", "quote_gap"};
    for (const auto &img: m_images) {
        auto s = m_buffer->get_iter_at_offset(img.offset);
        auto e = s;
        e.forward_char();
        for (const char *name: names) {
            if (const auto tag = m_tag_table->lookup(name))
                m_buffer->remove_tag(tag, s, e);
        }
    }
}

void MarkdownView::insert_node(const mdview::Heading &h, Gtk::TextIter &iter) {
    auto tag = m_tag_table->lookup("h" + std::to_string(std::min(h.level, 4)));
    if (!tag)
        tag = m_tag_table->lookup("h4");

    // Anchor offsets are recorded here rather than as buffer marks: every
    // GtkTextMark taxes all later edits, and positions cannot drift anyway
    // once a document is loaded.
    const int line_start = iter.get_offset();
    std::string plain;
    for (const auto &inline_node: h.content) {
        std::visit(
                [&plain](const auto &inl) {
                    using U = std::decay_t<decltype(inl)>;
                    if constexpr (std::is_same_v<U, mdview::Image>)
                        plain += inl.alt;
                    else
                        plain += inl.text;
                },
                inline_node);
    }
    Glib::ustring anchor;
    if (const std::string slug = slugify(plain); !slug.empty()) {
        anchor = Glib::ustring(kAnchorPrefix) + slug;
        for (int dup = 1; m_anchor_offsets.contains(anchor); ++dup)
            anchor = Glib::ustring(kAnchorPrefix) + slug + "-" + std::to_string(dup);
    }

    for (const auto &inline_node: h.content) {
        std::visit([this, &iter, &tag](const auto &inl) { insert_inline(inl, iter, tag); }, inline_node);
    }
    iter = m_buffer->insert(iter, "\n");

    if (!anchor.empty()) {
        m_slugs.insert(anchor);
        m_anchor_offsets.emplace(anchor, line_start);
        m_toc.push_back({h.level, Glib::ustring(plain), anchor});
    }
}

void MarkdownView::insert_node(const mdview::Paragraph &p, Gtk::TextIter &iter) {
    for (const auto &inline_node: p.content) {
        std::visit([this, &iter](const auto &inl) { insert_inline(inl, iter); }, inline_node);
    }
    iter = m_buffer->insert(iter, "\n");
}

void MarkdownView::insert_node(const mdview::CodeBlock &cb, Gtk::TextIter &iter) {
    const auto code_tag = m_tag_table->lookup("code_block");
    const auto top_tag = m_tag_table->lookup("code_block_padding_top");
    const auto bottom_tag = m_tag_table->lookup("code_block_padding_bottom");
    const auto lang_tag = m_tag_table->lookup("code_block_language");

    // Top line reserves room for the painted copy icon (snapshot_vfunc); the
    // language label follows the gap. Blocks without a language get a plain
    // spacer line so the icon always has its corner.
    {
        const Glib::ustring lead("    ");
        if (top_tag)
            iter = m_buffer->insert_with_tags(iter, lead, {top_tag});
        else
            iter = m_buffer->insert(iter, lead);
        if (!cb.language.empty()) {
            if (lang_tag)
                iter = m_buffer->insert_with_tags(iter, Glib::ustring(cb.language), {lang_tag});
            else
                iter = m_buffer->insert(iter, cb.language);
        }
        iter = m_buffer->insert(iter, "\n");
    }

    // Split code into lines to add padding
    std::vector<std::string> lines;
    std::stringstream ss(cb.code);
    std::string line;
    while (std::getline(ss, line)) {
        lines.push_back(line);
    }

    // Insert code lines with left padding (leading space). Batching keeps the
    // buffer-edit count flat in line count: GtkTextView updates every mark on
    // every edit, which is quadratic on large documents otherwise.
    const int code_start = iter.get_offset();
    auto code_line = [](const std::string &l) { return l.empty() ? std::string("  ") : "  " + l; };
    if (!lines.empty()) {
        iter = m_buffer->insert_with_tags(iter, code_line(lines.front()) + "\n", {top_tag ? top_tag : code_tag});
        std::string body;
        for (size_t i = 1; i + 1 < lines.size(); ++i) {
            body += code_line(lines[i]);
            body += "\n";
        }
        if (!body.empty())
            iter = m_buffer->insert_with_tags(iter, body, {code_tag});
        if (lines.size() > 1) {
            const Glib::ustring last = code_line(lines.back()) + "\n";
            iter = m_buffer->insert_with_tags(iter, last, {bottom_tag ? bottom_tag : code_tag});
        }
    }
    m_code_blocks.emplace_back(code_start, iter.get_offset());
    iter = m_buffer->insert(iter, "\n");
}

Glib::RefPtr<Gtk::TextTag> MarkdownView::quote_tag(int depth) {
    // Lazy "blockquote-dN" tags for every depth (list_tag() pattern). The
    // legacy plain "blockquote" tag stays theme-painted but is no longer the
    // layout carrier: margin updates on it proved unreliable next to the
    // buffer-wide measure tag, while freshly created per-depth tags honour
    // theirs exactly.
    const Glib::ustring name = "blockquote-d" + std::to_string(depth);
    if (const auto existing = m_tag_table->lookup(name))
        return existing;
    const auto tag = Gtk::TextTag::create(name);
    m_tag_table->add(tag);
    tag->property_left_margin() = m_measure_margin + quote_pad() + m_base_indent * m_zoom * depth;
    tag->property_background() = m_dark_mode ? "#2a2a2a" : "#f0f0f0";
    tag->property_paragraph_background() = m_dark_mode ? "#2a2a2a" : "#f0f0f0";
    return tag;
}

void MarkdownView::insert_node(const mdview::BlockQuote &bq, Gtk::TextIter &iter) {
    update_list_indent();
    auto tag = quote_tag(bq.depth);

    for (const auto &inline_node: bq.content) {
        std::visit([this, &iter, &tag](const auto &inl) { insert_inline(inl, iter, tag); }, inline_node);
    }
    iter = m_buffer->insert(iter, "\n");
}

void MarkdownView::insert_node(const mdview::HRule & /*hr*/, Gtk::TextIter &iter) const {
    const auto tag = m_tag_table->lookup("hr");
    iter = m_buffer->insert_with_tags(iter, Glib::ustring("──────────────────────────────"), {tag});
    iter = m_buffer->insert(iter, "\n");
}

Glib::RefPtr<Gtk::TextTag> MarkdownView::list_tag(int depth) {
    // Depth 0 reuses the classic "bullet" tag (kept current by update_list_indent);
    // deeper levels get lazily created "bullet-dN" siblings with proportional margins.
    const Glib::ustring name = depth == 0 ? Glib::ustring("bullet") : Glib::ustring("bullet-d" + std::to_string(depth));
    if (const auto existing = m_tag_table->lookup(name))
        return existing;
    const auto tag = Gtk::TextTag::create(name);
    m_tag_table->add(tag);
    const int extra = static_cast<int>(m_base_indent * m_zoom);
    tag->property_left_margin() = m_measure_margin + extra * (depth + 1);
    tag->property_indent() = extra;
    return tag;
}

void MarkdownView::insert_node(const mdview::List &list, Gtk::TextIter &iter) {
    update_list_indent();
    insert_list_entries(list.items, iter, 0);
}

void MarkdownView::insert_list_entries(const std::vector<mdview::ListEntry> &entries, Gtk::TextIter &iter, int depth) {
    const auto tag = list_tag(depth);

    int number = 1;
    for (const auto &entry: entries) {
        Glib::ustring marker;
        switch (entry.kind) {
            case 'o':
                marker = std::format("{}. ", number++);
                break;
            case 't':
                marker = entry.checked ? "\u2611 " : "\u2610 "; // ballot box with / without X
                break;
            default:
                marker = "\u2022 ";
                break;
        }
        iter = m_buffer->insert_with_tags(iter, marker, {tag});

        for (const auto &inline_node: entry.content) {
            std::visit([this, &iter, &tag](const auto &inl) { insert_inline(inl, iter, tag); }, inline_node);
        }
        iter = m_buffer->insert(iter, "\n");

        if (!entry.children.empty())
            insert_list_entries(entry.children, iter, depth + 1);
    }
}

void MarkdownView::insert_node(const mdview::Table &t, Gtk::TextIter &iter) {
    const auto table_cell_tag = m_tag_table->lookup("table_cell");

    // UTF-8 box-drawing constants
    static constexpr const char *kTopLeft = "┌";
    static constexpr const char *kTopRight = "┐";
    static constexpr const char *kBottomLeft = "└";
    static constexpr const char *kBottomRight = "┘";
    static constexpr const char *kTJunction = "┬";
    static constexpr const char *kPlus = "┼";
    static constexpr const char *kBJunction = "┴";
    static constexpr const char *kHLine = "─";
    static constexpr const char *kVLine = "│";
    static constexpr const char *kSepLeft = "├";
    static constexpr const char *kSepRight = "┤";

    // Repeat a UTF-8 string n times
    auto repeat = [](const char *s, size_t n) -> std::string {
        std::string result;
        result.reserve(std::strlen(s) * n);
        for (size_t i = 0; i < n; ++i) {
            result += s;
        }
        return result;
    };

    // Extract plain text from inline nodes for width computation
    auto extract_text = [](const std::vector<mdview::InlineNode> &nodes) -> std::string {
        std::string result;
        for (const auto &node: nodes) {
            std::visit(
                    [&result](const auto &n) {
                        if constexpr (std::is_same_v<std::decay_t<decltype(n)>, mdview::TextRun>) {
                            result += n.text;
                        }
                    },
                    node);
        }
        return result;
    };

    // Display width via Pango: measures actual rendered width in monospace font.
    // This correctly handles emoji (🖥🗑⭐), symbols (☀♥✔), and CJK characters
    // that wcwidth miscounts for monospace rendering.
    auto pango_layout = create_pango_layout("");
    PangoFontDescription *raw = pango_font_description_new();
    pango_font_description_set_family(raw, "monospace");
    // Match the widget's resolved font size so measurement equals rendering;
    // a hardcoded size would skew every fit factor. The zoom tag scales glyph
    // advances too, so fold the zoom factor into the measured size.
    const auto ctx_desc = get_pango_context()->get_font_description();
    const double base_pt = ctx_desc.get_size() > 0 ? static_cast<double>(ctx_desc.get_size()) : 12.0 * PANGO_SCALE;
    pango_font_description_set_size(raw, static_cast<gint>(base_pt));
    Pango::FontDescription font_desc(raw, false);
    pango_layout->set_font_description(font_desc);

    // Get monospace character width (baseline)
    pango_layout->set_text("M");
    Pango::Rectangle baseline_lr = pango_layout->get_logical_extents();
    int char_width = baseline_lr.get_width();
    if (char_width <= 0)
        char_width = 12;
    if (g_getenv("MDVIEW_DEBUG_TABLE"))
        std::print(stderr, "[table] cw={} base_pt={} zoom={}\n", char_width, base_pt, static_cast<double>(m_zoom));

    // Split text into grid clusters: a base character plus its zero-width
    // extenders (combining marks, variation selectors, skin-tone modifiers),
    // ZWJ joiners (absorbing the joined base so emoji families stay one
    // cluster) and regional-indicator pairs (one flag glyph).
    auto is_extender = [](gunichar cp) {
        return g_unichar_ismark(cp) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x200D ||
               (cp >= 0x1F3FB && cp <= 0x1F3FF);
    };
    auto is_ri = [](gunichar cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; };
    struct ClusterInfo {
        Glib::ustring text;
        // false for ZWJ sequences (shaped as one ligature)
        bool correctable = true;
    };
    auto split_clusters = [&is_extender, &is_ri](const Glib::ustring &s) {
        std::vector<ClusterInfo> out;
        const char *p = s.c_str();
        const char *end = p + s.bytes();
        while (p < end) {
            const char *start = p;
            gunichar base = g_utf8_get_char(p);
            p = g_utf8_next_char(p);
            ClusterInfo info;
            if (is_ri(base) && p < end && is_ri(g_utf8_get_char(p)))
                p = g_utf8_next_char(p); // regional-indicator pair renders as one flag glyph
            while (p < end) {
                gunichar nxt = g_utf8_get_char(p);
                if (!is_extender(nxt))
                    break;
                if (nxt == 0x200D)
                    info.correctable = false; // ZWJ sequence: shaped as a single ligature
                p = g_utf8_next_char(p);
                if (nxt == 0x200D && p < end)
                    p = g_utf8_next_char(p); // absorb the joined base into this cluster
            }
            info.text = Glib::ustring(start, p);
            out.push_back(std::move(info));
        }
        return out;
    };

    struct ClusterMetrics {
        int px = 0;
        int font_h = 0; // laid-out line height of the cluster (Pango units)
        int asc = 0; // baseline above top
        int desc = 0; // baseline below bottom
        Glib::ustring family; // concrete family Pango resolves for this cluster
    };
    std::map<Glib::ustring, ClusterMetrics> metrics_cache;
    auto measure_cluster = [&](const Glib::ustring &cluster) -> const ClusterMetrics & {
        auto cached = metrics_cache.find(cluster);
        if (cached != metrics_cache.end())
            return cached->second;
        pango_layout->set_text(cluster);
        ClusterMetrics m;
        Pango::Rectangle lr = pango_layout->get_logical_extents();
        m.px = lr.get_width();
        // Layout (not font-metric) height: this is exactly what stretches a
        // row. pango_font_get_metrics lies for bitmap strikes (Noto Color
        // Emoji reports a near-zero height there while the laid-out run is
        // far taller than monospace).
        m.font_h = lr.get_height();
        // Ascent/descent split around the baseline: a run may fit in total
        // height yet still overflow the row below the baseline (descent-heavy
        // fallback fonts), which needs a rise correction rather than scaling.
        {
            PangoLayoutIter *bit = pango_layout_get_iter(pango_layout->gobj());
            const int baseline = pango_layout_iter_get_baseline(bit);
            pango_layout_iter_free(bit);
            if (baseline > 0 && m.font_h > 0) {
                m.asc = baseline;
                m.desc = m.font_h - baseline;
            }
        }
        // Record the exact font Pango itemizes this cluster to, so rendering
        // can pin the same family: fallback order for an offscreen layout and
        // the text view may otherwise diverge (e.g. variation selectors pick
        // text vs emoji presentation fonts).
        PangoLayoutIter *lit = pango_layout_get_iter(pango_layout->gobj());
        do {
            const PangoLayoutRun *run = pango_layout_iter_get_run(lit);
            if (!run || !run->item || !run->item->analysis.font)
                continue;
            PangoFontDescription *desc = pango_font_describe(run->item->analysis.font);
            const char *fam = desc ? pango_font_description_get_family(desc) : nullptr;
            if (fam && *fam)
                m.family = fam; // copied before desc is freed
            if (desc)
                pango_font_description_free(desc);
            if (!m.family.empty())
                break;
        } while (pango_layout_iter_next_run(lit));
        pango_layout_iter_free(lit);
        return metrics_cache.emplace(cluster, m).first->second;
    };

    // Monospace line box: no table row may grow past this, or stacked box-
    // drawing characters on adjacent lines lose contact (visible gaps in the
    // vertical rules around rows containing tall emoji).
    const auto &mono_m = measure_cluster("M");
    const Glib::ustring mono_family = mono_m.family;
    const int cell_font_h = mono_m.font_h;
    const int cell_asc = mono_m.asc > 0 ? mono_m.asc : mono_m.font_h * 3 / 4;
    const int cell_desc = mono_m.desc > 0 ? mono_m.font_h - mono_m.asc : mono_m.font_h - cell_asc;

    // Grid columns a cluster occupies: its measured pixel width rounded to the
    // nearest whole number of monospace cells (minimum 1). The residual is
    // corrected by uniformly scaling the run, so round (not ceil) is exact.
    auto cluster_cols = [&](const Glib::ustring &cluster) -> size_t {
        const auto &m = measure_cluster(cluster);
        if (m.px <= 0)
            return 0;
        if (!m.family.empty() && m.family == mono_family) {
            // True monospace text sits exactly one cell per character. Measuring
            // and rounding instead lets AA/kerning noise flip a cluster a whole
            // cell, desynchronising the padded content rows from the rules.
            return std::max<size_t>(1, g_utf8_strlen(cluster.c_str(), cluster.bytes()));
        }
        return std::max<size_t>(1, static_cast<size_t>((m.px + char_width / 2) / char_width));
    };

    auto display_width = [&](const std::string &s) -> size_t {
        size_t total = 0;
        for (const auto &cl: split_clusters(Glib::ustring(s))) {
            total += cluster_cols(cl.text);
        }
        return total;
    };

    // Compute max display width per column
    std::vector<size_t> col_widths(t.alignments.size(), 0);
    for (size_t c = 0; c < t.header.size(); ++c) {
        col_widths[c] = std::max(col_widths[c], display_width(extract_text(t.header[c].content)));
    }
    for (const auto &row: t.rows) {
        for (size_t c = 0; c < col_widths.size() && c < row.size(); ++c) {
            col_widths[c] = std::max(col_widths[c], display_width(extract_text(row[c].content)));
        }
    }
    for (auto &w: col_widths) {
        if (w == 0)
            w = 1;
    }

    // GtkTextView walks every mark on every buffer edit; a large document has
    // tens of thousands (heading anchors, image anchors), so table rendering
    // must touch the buffer as few times as possible. Plain cell text is
    // accumulated here and flushed in one insert; tagged runs flush first.
    std::string pending;
    auto flush = [&] {
        if (pending.empty())
            return;
        if (table_cell_tag)
            iter = m_buffer->insert_with_tags(iter, pending, {table_cell_tag});
        else
            iter = m_buffer->insert(iter, pending);
        pending.clear();
    };
    auto write = [&](const Glib::ustring &s) { pending += s.raw(); };

    // Write cell content so every cluster occupies an exact whole number of
    // monospace cells: emoji and symbols render at fractional multiples of the
    // monospace advance, so uniformly scale the run (advance and glyph shrink
    // or grow together) until its width equals cols * char_width exactly.
    // The tag also pins the concrete font family that was measured, so the two
    // can never resolve different fonts for the same codepoint. Runs whose
    // laid-out height exceeds the monospace line height are additionally
    // capped by scale (shrinking the glyph keeps the row compact); the width
    // freed by that extra shrink is padded with a fractionally scaled space.
    auto space_scale_tag = [&](int milli) -> Glib::RefPtr<Gtk::TextTag> {
        milli = static_cast<int>(std::lround(milli));
        const Glib::ustring name = "mdview-spfix#" + std::to_string(milli);
        if (const auto existing = m_tag_table->lookup(name))
            return existing;
        const auto tag = Gtk::TextTag::create(name);
        tag->property_scale() = milli / 1000.0;
        m_tag_table->add(tag);
        m_grid_tags.push_back({.tag = tag, .scale_milli = milli});
        return tag;
    };
    auto write_cell_text = [&](const std::string &text) {
        const bool dbg = g_getenv("MDVIEW_DEBUG_TABLE") != nullptr;
        for (const auto &cl: split_clusters(Glib::ustring(text))) {
            const auto &m = measure_cluster(cl.text);
            // Height budget: shrink any run whose laid-out height exceeds the
            // monospace line box until it fits (aspect preserved), then shift
            // it up so its descent stops overflowing below the baseline. A run
            // scaled to at most cell_font_h can always be positioned inside
            // the mono ascent+descent box by such a rise. Never scale above
            // 1.0: upscaling would stretch the row. Leftover width after any
            // shrink is padded with a fractionally scaled space so the cluster
            // still spans exactly cols cells.
            double fit = 0.0;
            if (m.px > 0 && cl.correctable)
                fit = static_cast<double>(cluster_cols(cl.text) * static_cast<size_t>(char_width)) / m.px;
            double cap = 1.0;
            if (m.font_h > 0 && cell_font_h > 0 && m.font_h > cell_font_h)
                cap = static_cast<double>(cell_font_h) / m.font_h;
            if (fit <= 0.0 || m.family.empty()) {
                // Uncorrectable width (ZWJ ligatures): still apply the height
                // cap so the row does not stretch.
                if (cap < 1.0 && !m.family.empty()) {
                    const int rise = m.desc > 0 ? std::max(0, static_cast<int>(cap * m.desc) - cell_desc) : 0;
                    flush();
                    std::vector<Glib::RefPtr<Gtk::TextTag>> tags{
                            grid_fix_tag(m.family, static_cast<int>(std::lround(cap * 1000.0)), rise)};
                    if (table_cell_tag)
                        tags.insert(tags.begin(), table_cell_tag);
                    iter = m_buffer->insert_with_tags(iter, cl.text, tags);
                } else {
                    write(cl.text);
                }
                continue;
            }
            fit = std::min(fit, cap);
            const int rise = m.desc > 0 ? std::max(0, static_cast<int>(fit * m.desc) - cell_desc) : 0;
            // Plain monospace text (the overwhelming majority of cells): the
            // cell tag alone renders exactly right; creating a fix tag would
            // bloat the tag table quadratically on large documents.
            if (std::abs(fit - 1.0) < 0.0005 && rise == 0 && (m.family.empty() || m.family == mono_family)) {
                write(cl.text);
                continue;
            }
            if (dbg)
                std::print(stderr,
                           "[table] '{}' px={} cw={} cols={} fam='{}' h={} asc={} desc={} cell={}/{} fit={:.3f} "
                           "rise={}\n",
                           std::string_view(cl.text.raw()), m.px, char_width, cluster_cols(cl.text),
                           std::string_view(m.family.raw()), m.font_h, m.asc, m.desc, cell_asc, cell_desc, fit, rise);
            // The buffer-wide zoom tag also carries `scale`, and Pango
            // multiplies coexisting scale attributes rather than letting one
            // win, so this run's scale stays zoom-independent: zoom scales the
            // corrected width along with everything else.
            const int scale_milli = static_cast<int>(std::lround(fit * 1000.0));
            flush();
            std::vector<Glib::RefPtr<Gtk::TextTag>> tags{grid_fix_tag(m.family, scale_milli, rise)};
            if (table_cell_tag)
                tags.insert(tags.begin(), table_cell_tag);
            iter = m_buffer->insert_with_tags(iter, cl.text, tags);
            // Pad the width freed by the height cap with a scaled space so the
            // cluster still spans exactly cols cells. Space advance scales
            // linearly: rendered = char_width * scale, independent of zoom.
            const int target = static_cast<int>(cluster_cols(cl.text) * static_cast<size_t>(char_width));
            const int residual = target - static_cast<int>(std::lround(m.px * fit));
            if (residual >= 2 && char_width > 0) {
                const int sp_milli =
                        static_cast<int>(std::lround(static_cast<long long>(residual) * 1000 / char_width));
                flush();
                std::vector<Glib::RefPtr<Gtk::TextTag>> pad{space_scale_tag(sp_milli)};
                if (table_cell_tag)
                    pad.insert(pad.begin(), table_cell_tag);
                iter = m_buffer->insert_with_tags(iter, " ", pad);
            }
        }
    };

    // Box-drawing glyphs often resolve to a fallback font whose advance
    // differs slightly from the cell monospace; over a long rule that drift
    // detaches the corners from the content rows. Routing these runs through
    // write_cell_text applies the same per-cluster width correction cells get.
    auto write_grid = [&](const char *g) { write_cell_text(g); };

    // Top border
    write_grid(kTopLeft);
    for (size_t c = 0; c < col_widths.size(); ++c) {
        write_grid(repeat(kHLine, col_widths[c] + 2).c_str());
        write_grid(c + 1 < col_widths.size() ? kTJunction : kTopRight);
    }
    write("\n");

    // Header row
    for (size_t c = 0; c < t.header.size() && c < col_widths.size(); ++c) {
        write_grid(kVLine);
        std::string text = extract_text(t.header[c].content);
        char align = t.alignments.size() > c ? t.alignments[c] : 'l';
        size_t pad = col_widths[c] - display_width(text);
        if (align == 'r') {
            // Gutter on both sides like every other cell type; omitting the
            // trailing space makes content rows shorter than the rules.
            write(Glib::ustring(pad + 1, ' '));
            write_cell_text(text);
            write(" ");
        } else if (align == 'c') {
            size_t lp = pad / 2 + 1;
            write(Glib::ustring(lp, ' '));
            write_cell_text(text);
            write(Glib::ustring(pad - lp + 2, ' '));
        } else {
            write(" ");
            write_cell_text(text);
            write(Glib::ustring(pad + 1, ' '));
        }
    }
    write_grid(kVLine);
    write("\n");

    // Separator line
    write_grid(kSepLeft);
    for (size_t c = 0; c < col_widths.size(); ++c) {
        write_grid(repeat(kHLine, col_widths[c] + 2).c_str());
        write_grid(c + 1 < col_widths.size() ? kPlus : kSepRight);
    }
    write("\n");

    // Body rows
    for (const auto &row: t.rows) {
        for (size_t c = 0; c < col_widths.size(); ++c) {
            write_grid(kVLine);
            if (c >= row.size()) {
                write(Glib::ustring(std::string(col_widths[c] + 2, ' ')));
                continue;
            }
            std::string text = extract_text(row[c].content);
            char align = t.alignments.size() > c ? t.alignments[c] : 'l';
            size_t pad = col_widths[c] - display_width(text);
            if (align == 'r') {
                write(Glib::ustring(pad + 1, ' '));
                write_cell_text(text);
                write(" ");
            } else if (align == 'c') {
                size_t lp = pad / 2 + 1;
                write(Glib::ustring(lp, ' '));
                write_cell_text(text);
                write(Glib::ustring(pad - lp + 2, ' '));
            } else {
                write(" ");
                write_cell_text(text);
                write(Glib::ustring(pad + 1, ' '));
            }
        }
        write_grid(kVLine);
        write("\n");
    }

    // Bottom border
    write_grid(kBottomLeft);
    for (size_t c = 0; c < col_widths.size(); ++c) {
        write_grid(repeat(kHLine, col_widths[c] + 2).c_str());
        write_grid(c + 1 < col_widths.size() ? kBJunction : kBottomRight);
    }
    write("\n");
    flush();
}

void MarkdownView::insert_inline(const mdview::TextRun &tr, Gtk::TextIter &iter, Glib::RefPtr<Gtk::TextTag> tag) const {
    Glib::ustring text = tr.text;
    if (text.empty())
        return;

    // Collect all applicable tags to apply atomically via insert_with_tags
    std::vector<Glib::RefPtr<Gtk::TextTag>> tags;
    if (tag)
        tags.push_back(tag);
    if (tr.code) {
        if (auto code_tag = m_tag_table->lookup("inline_code"))
            tags.push_back(code_tag);
    }
    if (tr.bold) {
        if (auto bold_tag = m_tag_table->lookup("bold"))
            tags.push_back(bold_tag);
    }
    if (tr.italic) {
        if (auto italic_tag = m_tag_table->lookup("italic"))
            tags.push_back(italic_tag);
    }
    if (tr.strikethrough) {
        if (auto strike_tag = m_tag_table->lookup("strikethrough"))
            tags.push_back(strike_tag);
    }

    if (tags.empty()) {
        iter = m_buffer->insert(iter, text);
    } else {
        iter = m_buffer->insert_with_tags(iter, text, tags);
    }
}

Glib::RefPtr<Gtk::TextTag> MarkdownView::get_link_tag(const std::string &url) {
    if (const auto it = m_link_tags.find(url); it != m_link_tags.end())
        return it->second;

    const auto tag = Gtk::TextTag::create(Glib::ustring(kUrlTagPrefix) + std::to_string(m_link_tag_counter++));
    tag->property_underline() = Pango::Underline::SINGLE;
    tag->property_foreground_set() = true;
    tag->property_foreground() = m_dark_mode ? "#58a6ff" : "#0b57d0";
    g_object_set_data_full(G_OBJECT(tag->gobj()), kUrlDataKey, g_strdup(url.c_str()), g_free);
    m_tag_table->add(tag);
    m_link_tags[url] = tag;
    return tag;
}

Glib::ustring MarkdownView::url_at(const Gtk::TextConstIter &iter) {
    for (const auto &tag: iter.get_tags()) {
        const Glib::ustring name = tag->property_name();
        if (std::string_view(name.raw()).starts_with(kUrlTagPrefix)) {
            auto *raw = const_cast<GtkTextTag *>(tag->gobj());
            if (const void *url = g_object_get_data(G_OBJECT(raw), kUrlDataKey))
                return Glib::ustring(static_cast<const char *>(url));
        }
    }
    return {};
}

void MarkdownView::insert_inline(const mdview::Link &l, Gtk::TextIter &iter, const Glib::RefPtr<Gtk::TextTag> &tag) {
    const Glib::ustring text = l.text;
    if (text.empty())
        return;

    std::vector<Glib::RefPtr<Gtk::TextTag>> tags;
    if (tag)
        tags.push_back(tag);
    if (!l.url.empty())
        tags.push_back(get_link_tag(l.url));

    if (tags.empty()) {
        iter = m_buffer->insert(iter, text);
    } else {
        iter = m_buffer->insert_with_tags(iter, text, tags);
    }
}

// Resolve and decode an image URL (relative path, file://, http(s) via libsoup,
// or base64 data: URIs). Returns nullptr on any failure; the
// caller falls back to alt text. Decoding caps huge images at 4096px per side.
Glib::RefPtr<Gdk::Pixbuf> MarkdownView::load_image(const std::string &url) {
    if (url.empty())
        return {};
    if (const auto cached = m_image_cache.find(url); cached != m_image_cache.end())
        return cached->second;

    Glib::RefPtr<Gdk::Pixbuf> result;
    Glib::RefPtr<Gdk::PixbufLoader> loader;
    try {
        constexpr guint64 kMaxEncodedBytes = 64ull * 1024 * 1024;
        std::vector<uint8_t> data;
        if (url.starts_with("data:")) {
            const auto comma = url.find(',');
            // Only base64-encoded data URIs are supported (not URL-escaped ones).
            const auto b64 = url.find(";base64", 0);
            if (comma == std::string::npos || comma + 1 >= url.size() || b64 == std::string::npos || b64 > comma)
                throw std::invalid_argument("unsupported data URI");
            gsize len = 0;
            auto *raw = g_base64_decode(url.c_str() + comma + 1, &len);
            data.assign(raw, raw + len);
            g_free(raw);
        } else if (url.starts_with("http://") || url.starts_with("https://")) {
            data = fetch_http(url, kMaxEncodedBytes);
        } else {
            Glib::RefPtr<Gio::File> file;
            if (has_uri_scheme(url)) {
                file = Gio::File::create_for_uri(url);
            } else if (Glib::path_is_absolute(url)) {
                file = Gio::File::create_for_path(url);
            } else if (!m_current_path.empty()) {
                const auto dir = Gio::File::create_for_path(Glib::path_get_dirname(std::string(m_current_path)));
                file = dir->resolve_relative_path(url);
            } else {
                file = Gio::File::create_for_path(url);
            }
            // Blocking devices (FIFOs, stale mounts) would freeze the UI thread
            // indefinitely inside read(); accept plain files only.
            if (!has_uri_scheme(url) && file->query_file_type() != Gio::FileType::REGULAR)
                throw std::runtime_error("image source is not a regular file");
            const auto stream = file->read();
            data = read_capped(stream, kMaxEncodedBytes);
        }

        loader = Gdk::PixbufLoader::create();
        // Decode oversized images (or decompression bombs claiming huge
        // dimensions) directly at reduced resolution; gdk-pixbuf's full-size
        // allocation of a bomb would abort the process before any cap could run.
        constexpr int kMaxSide = 4096;
        if (const auto [w, h] = sniff_image_size(data.data(), data.size()); std::max(w, h) > kMaxSide) {
            const double d = static_cast<double>(kMaxSide) / std::max(w, h);
            loader->set_size(std::max(1, static_cast<int>(w * d)), std::max(1, static_cast<int>(h * d)));
        }
        loader->write(data.data(), data.size());
        loader->close();
        result = loader->get_pixbuf();
        // Unknown formats are not sniffable; cap their decode after the fact.
        if (result && std::max(result->get_width(), result->get_height()) > kMaxSide) {
            const double d = static_cast<double>(kMaxSide) / std::max(result->get_width(), result->get_height());
            result = result->scale_simple(std::max(1, static_cast<int>(result->get_width() * d)),
                                          std::max(1, static_cast<int>(result->get_height() * d)),
                                          Gdk::InterpType::BILINEAR);
        }
    } catch (const std::exception &err) {
        g_message("mdview: image '%s' not rendered: %s", url.c_str(), err.what());
        result.reset();
        if (loader) {
            try {
                loader->close(); // gdk-pixbuf requires close() even after failure
            } catch (const std::exception &) {
            }
        }
    }
    if (result) {
        const size_t bytes =
                static_cast<size_t>(result->get_rowstride()) * static_cast<size_t>(std::max(1, result->get_height()));
        m_image_cache.emplace(url, result);
        m_image_cache_order.push_back(url);
        m_image_cache_bytes += bytes;
        while (m_image_cache_bytes > kImageCacheBudget && !m_image_cache_order.empty()) {
            const auto victim = m_image_cache_order.front();
            m_image_cache_order.pop_front();
            if (auto it = m_image_cache.find(victim); it != m_image_cache.end()) {
                m_image_cache_bytes -= static_cast<size_t>(it->second->get_rowstride()) *
                                       static_cast<size_t>(std::max(1, it->second->get_height()));
                m_image_cache.erase(it);
            }
        }
    }
    return result;
}

// Remote images are privacy-sensitive (the request leaks IP/user-agent), so
// they are never fetched automatically; a click-to-load placeholder is shown.
bool MarkdownView::is_remote_url(const std::string &url) {
    return url.starts_with("http://") || url.starts_with("https://") ||
           g_ascii_strncasecmp(url.c_str(), "HTTP://", 7) == 0 || g_ascii_strncasecmp(url.c_str(), "HTTPS://", 8) == 0;
}

// SVG additionally defers even from disk: librsvg resolves external hrefs and
// XIncludes inside the picture, so an attacker-supplied .svg could phone home
// or pull second-stage content without the consent the click-to-load promise
// exists to obtain.
bool MarkdownView::needs_click_to_load(const std::string &url) {
    if (is_remote_url(url))
        return true;
    const std::string path = url.substr(0, url.find_first_of("?#"));
    const auto dot = path.rfind('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = path.substr(dot + 1);
    for (auto &c: ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "svg" || ext == "svgz";
}

namespace {
    // Placeholder box size for remote images awaiting a click (no network was
    // touched yet, so the natural size is unknown).
    constexpr int kPlaceholderW = 320;
    constexpr int kPlaceholderH = 180;

    // Host part of an http(s) URL for the placeholder caption ("pics.example.com").
    std::string url_host(const std::string &url) {
        const auto sep = url.find("//");
        if (sep == std::string::npos)
            return url;
        const auto host_end = url.find_first_of("/?#", sep + 2);
        return url.substr(sep + 2, host_end == std::string::npos ? std::string::npos : host_end - sep - 2);
    }
} // namespace

// Images never upscale beyond their natural size at zoom 1; they shrink to fit
// the view width, then follow the text zoom like everything else.
double MarkdownView::image_scale_for(int natural_width) const {
    if (natural_width <= 0)
        return 1.0;
    int avail = get_allocated_width() - m_measure_margin - m_measure_margin_right - 8;
    if (avail < 100)
        avail = 1024; // not allocated yet; assume a typical window
    return std::min(1.0, static_cast<double>(avail) / natural_width) * static_cast<double>(m_zoom);
}

void MarkdownView::rescale_images() {
    for (auto &[tag, w, h]: m_imgline_tags)
        apply_image_line_height(*tag, std::max(1, static_cast<int>(std::lround(h * image_scale_for(w)))));
}

// On-screen rect of an image placement in buffer coordinates (hit-testing and
// painting share this so they can never disagree). Pending placeholders use a
// fixed natural size; loading swaps in the real one.
bool MarkdownView::image_rect_buffer(const ImagePlacement &img, Gdk::Rectangle &out) const {
    const auto iter = m_buffer->get_iter_at_offset(img.offset);
    if (!iter)
        return false;
    int top = 0;
    int bottom = 0;
    get_line_yrange(iter, top, bottom);
    Gdk::Rectangle lr;
    get_iter_location(m_buffer->get_iter_at_line(iter.get_line()), lr);
    const int nat_w = img.pixbuf ? img.pixbuf->get_width() : kPlaceholderW;
    const int nat_h = img.pixbuf ? img.pixbuf->get_height() : kPlaceholderH;
    const double s = image_scale_for(nat_w);
    out.set_x(lr.get_x());
    out.set_y(top);
    out.set_width(std::clamp(static_cast<int>(std::lround(nat_w * s)), 1, 16384));
    out.set_height(std::clamp(static_cast<int>(std::lround(nat_h * s)), 1, 16384));
    return true;
}

// Index into m_images of a not-yet-loaded remote image at widget coords, or -1.
int MarkdownView::pending_image_at_point(double x, double y) const {
    int bx = 0;
    int by = 0;
    window_to_buffer_coords(Gtk::TextWindowType::TEXT, static_cast<int>(x), static_cast<int>(y), bx, by);
    for (size_t i = 0; i < m_images.size(); ++i) {
        if (m_images[i].pixbuf)
            continue;
        Gdk::Rectangle r;
        if (image_rect_buffer(m_images[i], r) && bx >= r.get_x() && bx < r.get_x() + r.get_width() && by >= r.get_y() &&
            by < r.get_y() + r.get_height())
            return static_cast<int>(i);
    }
    return -1;
}

namespace {
    constexpr int kCopyIconSize = 16;
    constexpr int kCopyIconPad = 6;
} // namespace

bool MarkdownView::copy_icon_rect(int block_index, Gdk::Rectangle &out) const {
    if (block_index < 0 || static_cast<size_t>(block_index) >= m_code_blocks.size())
        return false;
    const auto it = m_buffer->get_iter_at_offset(m_code_blocks[static_cast<size_t>(block_index)].first);
    // get_iter_location reflects the real laid-out position (paragraph
    // spacing included); get_line_yrange does not.
    Gdk::Rectangle line;
    get_iter_location(m_buffer->get_iter_at_line(it.get_line() - 1), line); // language/spacer line above the code
    out = Gdk::Rectangle(m_measure_margin + kCopyIconPad, line.get_y() + (line.get_height() - kCopyIconSize) / 2,
                         kCopyIconSize, kCopyIconSize);
    return true;
}

int MarkdownView::copy_icon_at_point(double x, double y) const {
    int bx = 0;
    int by = 0;
    window_to_buffer_coords(Gtk::TextWindowType::TEXT, static_cast<int>(x), static_cast<int>(y), bx, by);
    Gdk::Rectangle r;
    for (size_t i = 0; i < m_code_blocks.size(); ++i) {
        if (copy_icon_rect(static_cast<int>(i), r) && bx >= r.get_x() - 4 && bx < r.get_x() + r.get_width() + 4 &&
            by >= r.get_y() - 4 && by < r.get_y() + r.get_height() + 4)
            return static_cast<int>(i);
    }
    return -1;
}

// User clicked a placeholder: fetch over the network now. Synchronous on
// purpose — it is user-initiated, and load_image already caps bytes/size.
void MarkdownView::load_remote_image(size_t index) {
    if (index >= m_images.size())
        return;
    auto &pl = m_images[index];
    if (pl.pixbuf || pl.url.empty())
        return;
    pl.pixbuf = load_image(pl.url);
    pl.failed = !pl.pixbuf;
    if (pl.pixbuf && index < m_imgline_tags.size()) {
        auto &line = m_imgline_tags[index];
        line.nat_w = pl.pixbuf->get_width();
        line.nat_h = pl.pixbuf->get_height();
        apply_image_line_height(*line.tag,
                                std::max(1, static_cast<int>(std::lround(line.nat_h * image_scale_for(line.nat_w)))));
    }
    queue_resize();
    queue_draw();
}

// Paint each embedded image over its reserved line. Positions come from the
// anchor marks every frame, so scrolling, zoom and resize track automatically.
// Buffer-offset range currently scrolled into view; used to cull per-frame
// painting work to what can actually be seen (huge documents stall otherwise).
bool MarkdownView::visible_offset_range(int &top, int &bottom) const {
    const auto adj = get_vadjustment();
    if (!adj || get_height() <= 0)
        return false;
    Gtk::TextConstIter it;
    int line_top = 0;
    get_line_at_y(it, static_cast<int>(adj->get_value()), line_top);
    top = it.get_offset();
    get_line_at_y(it, static_cast<int>(adj->get_value()) + get_height(), line_top);
    bottom = it.get_offset();
    return true;
}

void MarkdownView::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot> &snapshot) {
    Gtk::TextView::snapshot_vfunc(snapshot);
    int vis_top = 0;
    int vis_bottom = 0;
    const bool culled = visible_offset_range(vis_top, vis_bottom);
    const int slack = 4096; // lines around the fold keep scrolling smooth
    size_t first_img = 0;
    size_t last_img = m_images.size();
    if (culled && !m_images.empty()) {
        auto mark_offset = [this](size_t i) { return m_images[i].offset; };
        size_t lo = 0;
        size_t hi = m_images.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (mark_offset(mid) < vis_top - slack)
                lo = mid + 1;
            else
                hi = mid;
        }
        first_img = lo;
        lo = first_img;
        hi = m_images.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (mark_offset(mid) > vis_bottom + slack)
                hi = mid;
            else
                lo = mid + 1;
        }
        last_img = lo;
    }
    for (size_t i = first_img; i < last_img; ++i) {
        const auto &img = m_images[i];
        Gdk::Rectangle r;
        if (!image_rect_buffer(img, r))
            continue;
        int wx = 0;
        int wy = 0;
        buffer_to_window_coords(Gtk::TextWindowType::TEXT, r.get_x(), r.get_y(), wx, wy);
        if (!img.pixbuf) {
            draw_placeholder(snapshot, wx, wy, r.get_width(), r.get_height(), img.failed, img.url);
            continue;
        }
        if (!img.texture)
            img.texture = Gdk::Texture::create_for_pixbuf(img.pixbuf);
        if (img.texture)
            snapshot->append_texture(img.texture, Gdk::Graphene::Rect(wx, wy, r.get_width(), r.get_height()));
    }
    paint_quote_bars(snapshot);
    paint_copy_icons(snapshot);
}

// Small copy glyph at the top-left of every code block, in front of the
// language label. Painted (not a widget) so buffer layout is untouched.
// Inner padding between a quote bar and its text column. Layout reserves
// exactly this much left margin so the painted bar lands on the document
// column (m_measure_margin), aligning the block with headings and code.
int MarkdownView::quote_pad() { return std::max(4, static_cast<int>(std::lround((10 + ink_overhang_px()) * m_zoom))); }

// Worst-case horizontal ink overhang beyond a layout's logical origin for
// the widget's current font (round glyphs like 'w'/'m' bleed left of the
// advance). Measured, not guessed: theme fonts and text scaling vary.
int MarkdownView::ink_overhang_px() {
    const Glib::ustring key = get_pango_context()->get_font_description().to_string();
    if (key == m_overhang_key)
        return m_overhang_px;
    const auto layout = create_pango_layout("mwq");
    Pango::Rectangle ink;
    Pango::Rectangle logical;
    layout->get_pixel_extents(ink, logical);
    m_overhang_key = key;
    m_overhang_px = std::max(0, logical.get_x() - ink.get_x());
    return m_overhang_px;
}

// GitHub-style vertical rule in the gutter of each block-quote run. Lines are
// recorded per logical line with their quote depth in LineInfo::info; maximal
// runs of equal depth become one rect spanning wrapped lines via line yranges.
void MarkdownView::paint_quote_bars(const Glib::RefPtr<Gtk::Snapshot> &snapshot) {
    if (m_line_info.empty())
        return;
    const Gdk::RGBA bar = m_dark_mode ? Gdk::RGBA("#52525a") : Gdk::RGBA("#d0d7de");
    int vis_top = 0;
    int vis_bottom = 0;
    const bool culled = visible_offset_range(vis_top, vis_bottom);

    int run_depth = -1;
    int run_first_line = 0;
    int prev_line = -2;
    auto flush_run = [&](int last_line) {
        if (run_depth < 0)
            return;
        int y0 = 0;
        int h0 = 0;
        int y1 = 0;
        int h1 = 0;
        get_line_yrange(m_buffer->get_iter_at_line(run_first_line), y0, h0);
        get_line_yrange(m_buffer->get_iter_at_line(last_line), y1, h1);
        // Anchor to laid-out line origins, not to tag-margin arithmetic:
        // wrapped continuations start at the tag's left_margin (without the
        // first-line indent), so anything computed from margins alone can
        // land on top of them. Clearance adds the font's worst-case ink
        // overhang past the layout origin, measured live so theme font and
        // text scaling cannot push glyphs into the bar.
        int origin_x = 0;
        bool have_x = false;
        for (int line = run_first_line; line <= last_line; ++line) {
            Gdk::Rectangle r;
            get_iter_location(m_buffer->get_iter_at_line(line), r);
            if (!have_x || r.get_x() < origin_x) {
                origin_x = r.get_x();
                have_x = true;
            }
        }
        if (!have_x)
            return;
        int wx = 0;
        int wy = 0;
        // Gap and bar thickness track zoom so the rule stays proportional
        // to the text. Tags cannot pad text inside their paragraph
        // background, so fake it: paint a filler in the quote's background
        // colour from the bar to the text column, making the gap read as
        // inner padding of the quoted block.
        const int pad = quote_pad();
        const int bar_w = std::max(2, static_cast<int>(std::lround(3.0 * m_zoom))); // integral: no AA seam vs filler
        buffer_to_window_coords(Gtk::TextWindowType::TEXT, origin_x - pad, y0, wx, wy);
        const Gdk::RGBA tint = m_dark_mode ? Gdk::RGBA("#2a2a2a") : Gdk::RGBA("#f0f0f0");
        const double h = (y1 + h1) - y0;
        snapshot->append_color(tint, Gdk::Graphene::Rect(wx, wy, pad, h));
        snapshot->append_color(bar, Gdk::Graphene::Rect(wx, wy, bar_w, h));
        run_depth = -1;
    };

    for (const auto &[offset, info]: m_line_info) {
        if (info.kind != LineKind::BlockQuote)
            continue;
        if (culled && (offset < vis_top - 4096 || offset > vis_bottom + 4096))
            continue;
        const int depth = info.info.empty() ? 0 : std::atoi(info.info.c_str());
        const int line = m_buffer->get_iter_at_offset(offset).get_line();
        if (depth != run_depth || line != prev_line + 1) {
            flush_run(prev_line);
            run_depth = depth;
            run_first_line = line;
        }
        prev_line = line;
    }
    flush_run(prev_line);
}

void MarkdownView::paint_copy_icons(const Glib::RefPtr<Gtk::Snapshot> &snapshot) {
    if (m_code_blocks.empty())
        return;
    const Gdk::RGBA fg = m_dark_mode ? Gdk::RGBA("#b8b8bc") : Gdk::RGBA("#5a5a60");
    int vis_top = 0;
    int vis_bottom = 0;
    size_t first = 0;
    size_t last = m_code_blocks.size();
    if (visible_offset_range(vis_top, vis_bottom)) {
        first = static_cast<size_t>(std::lower_bound(m_code_blocks.begin(), m_code_blocks.end(), vis_top - 4096,
                                                     [](const std::pair<int, int> &b, int v) { return b.second < v; }) -
                                    m_code_blocks.begin());
        last = static_cast<size_t>(std::upper_bound(m_code_blocks.begin(), m_code_blocks.end(), vis_bottom + 4096,
                                                    [](int v, const std::pair<int, int> &b) { return v < b.first; }) -
                                   m_code_blocks.begin());
    }
    for (size_t i = first; i < last; ++i) {
        Gdk::Rectangle r;
        if (!copy_icon_rect(static_cast<int>(i), r))
            continue;
        int wx = 0;
        int wy = 0;
        buffer_to_window_coords(Gtk::TextWindowType::TEXT, r.get_x(), r.get_y(), wx, wy);
        // Two overlapping document glyphs, built from flat rects (append_color
        // is the only primitive this renderer reliably rasterizes).
        const auto rect = [&](double x, double y, double w, double h) {
            snapshot->append_color(fg, Gdk::Graphene::Rect(wx + x, wy + y, w, h));
        };
        const Gdk::RGBA paper = m_dark_mode ? Gdk::RGBA("#3a3a3d") : Gdk::RGBA("#f2f2f4");
        constexpr double t = 1.4; // border thickness
        // back sheet: top/right borders + hint of left/bottom outside front overlap
        rect(6.0, 0.5, 9.5, t);
        rect(6.0, 0.5, t, 8.0);
        rect(14.1, 0.5, t, 12.0);
        rect(6.0, 11.1, 9.5, t);
        // front sheet: paper fill then its border ring
        snapshot->append_color(paper, Gdk::Graphene::Rect(wx + 0.5, wy + 3.5, 10.0, 12.0));
        rect(0.5, 3.5, 10.0, t);
        rect(0.5, 14.1, 10.0, t);
        rect(0.5, 3.5, t, 12.0);
        rect(9.1, 3.5, t, 12.0);
    }
}

// Dashed-looking gray box with a caption inviting the reader to fetch the
// remote image (or signalling that the last fetch failed).
void MarkdownView::draw_placeholder(const Glib::RefPtr<Gtk::Snapshot> &snapshot, int x, int y, int w, int h,
                                    bool failed, const std::string &url) {
    const Gdk::RGBA bg = m_dark_mode ? Gdk::RGBA("#3a3a3d") : Gdk::RGBA("#f2f2f4");
    const Gdk::RGBA border = m_dark_mode ? Gdk::RGBA("#6a6a70") : Gdk::RGBA("#b8b8c0");
    const Gdk::RGBA fg = m_dark_mode ? Gdk::RGBA("#d0d0d4") : Gdk::RGBA("#55555c");

    snapshot->save();
    snapshot->translate({static_cast<float>(x), static_cast<float>(y)});
    snapshot->append_color(bg, Gdk::Graphene::Rect(0, 0, w, h));
    snapshot->append_color(border, Gdk::Graphene::Rect(0, 0, w, 1));
    snapshot->append_color(border, Gdk::Graphene::Rect(0, h - 1, w, 1));
    snapshot->append_color(border, Gdk::Graphene::Rect(0, 0, 1, h));
    snapshot->append_color(border, Gdk::Graphene::Rect(w - 1, 0, 1, h));

    const auto layout = create_pango_layout(
            Glib::ustring(failed ? "Failed to load — click to retry" : "Click to load remote image"));
    const auto host = create_pango_layout(url_host(url));
    // Buffer text scales via the zoom tag; free-standing layouts must be
    // sized by hand or the caption stays small while its box grows.
    auto desc = get_pango_context()->get_font_description();
    if (desc.get_size() > 0)
        desc.set_size(static_cast<gint>(desc.get_size() * m_zoom));
    layout->set_font_description(desc);
    host->set_font_description(desc);
    int lw = 0;
    int lh = 0;
    layout->get_pixel_size(lw, lh);
    int hw = 0;
    int hh = 0;
    host->get_pixel_size(hw, hh);
    const int tx = std::max(4, (w - std::max(lw, hw)) / 2);
    const int ty = std::max(4, (h - lh - hh - 4) / 2);
    snapshot->save();
    snapshot->translate({static_cast<float>(tx), static_cast<float>(ty)});
    snapshot->append_layout(layout, fg);
    snapshot->translate({0.0f, static_cast<float>(lh + 4)});
    snapshot->append_layout(host, border);
    snapshot->restore();
    snapshot->restore();
}

// Natural text line height in px (unscaled; zoom scales numerator and
// denominator alike, so the resulting factor is zoom-independent).
int MarkdownView::base_line_height() {
    const auto layout = create_pango_layout("Mgpq");
    int w = 0;
    int h = 0;
    layout->get_pixel_size(w, h);
    return std::max(1, h);
}

// Tag that stretches an image's line to the image's rendered height. The
// line-height factor only accepts modest values, so anything beyond a few
// lines' worth is added as fixed spacing below the line instead. Tags are
// unique per image (id) because placeholders mutate their height on load.
Glib::RefPtr<Gtk::TextTag> MarkdownView::image_line_tag(unsigned id, int nat_w, int nat_h) {
    const auto tag = Gtk::TextTag::create("mdview-imgline#" + std::to_string(id));
    apply_image_line_height(*tag, std::max(1, static_cast<int>(std::lround(nat_h * image_scale_for(nat_w)))));
    m_tag_table->add(tag);
    m_imgline_tags.push_back({.tag = tag, .nat_w = nat_w, .nat_h = nat_h});
    return tag;
}

void MarkdownView::apply_image_line_height(Gtk::TextTag &tag, int rendered_height_px) {
    // line-height is a factor of the line's ACTUAL metrics, and the marker
    // line's glyphs carry the buffer-wide zoom tag: the effective base grows
    // with zoom or the reservation drifts from the painted texture (gap when
    // zooming in, overlap when zooming out).
    const double base_h = std::max(1.0, static_cast<double>(base_line_height()) * static_cast<double>(m_zoom));
    const double factor = std::min(static_cast<double>(rendered_height_px) / base_h, 8.0);
    const int extra = std::max(0, rendered_height_px - static_cast<int>(std::lround(base_h * factor)));
    tag.property_line_height() = factor;
    tag.property_pixels_below_lines() = extra;
}

void MarkdownView::insert_inline(const mdview::Image &img, Gtk::TextIter &iter, const Glib::RefPtr<Gtk::TextTag> &tag) {
    // Remote images are never fetched automatically (privacy); they render as a
    // click-to-load placeholder. Everything else loads eagerly at layout time.
    const bool remote = needs_click_to_load(img.url);
    Glib::RefPtr<Gdk::Pixbuf> pixbuf;
    if (remote) {
        // A previously click-loaded image survives live reloads via the cache.
        if (const auto cached = m_image_cache.find(img.url); cached != m_image_cache.end())
            pixbuf = cached->second;
    } else {
        pixbuf = load_image(img.url);
    }
    if (!remote && !pixbuf) {
        // Placeholder: italic "[alt]" so the reader knows something is missing.
        std::vector<Glib::RefPtr<Gtk::TextTag>> tags;
        if (tag)
            tags.push_back(tag);
        if (const auto it_tag = m_tag_table->lookup("italic"))
            tags.push_back(it_tag);
        const Glib::ustring label = "[" + Glib::ustring(img.alt.empty() ? img.url : img.alt) + "]";
        if (tags.empty()) {
            iter = m_buffer->insert(iter, label);
        } else {
            iter = m_buffer->insert_with_tags(iter, label, tags);
        }
        return;
    }

    // Images get their own line: a lone object-replacement character whose
    // line-height tag stretches the line to the image's rendered height. The
    // picture itself is painted in snapshot_vfunc at the anchor mark, since
    // TextView child-anchor widgets are unreliable for sized inline content.
    // Every step works from offsets: any buffer mutation invalidates iterators.
    int off = iter.get_offset();
    if (!iter.starts_line()) {
        m_buffer->insert(m_buffer->get_iter_at_offset(off), "\n");
        ++off;
    }

    const unsigned id = m_image_counter++;
    std::vector<Glib::RefPtr<Gtk::TextTag>> tags;
    if (tag)
        tags.push_back(tag);
    tags.push_back(image_line_tag(id, pixbuf ? pixbuf->get_width() : kPlaceholderW,
                                  pixbuf ? pixbuf->get_height() : kPlaceholderH));
    // Zero-width space: carries the line-height stretch but renders as nothing,
    // so tiny images show no leftover glyph box behind them.
    m_buffer->insert_with_tags(m_buffer->get_iter_at_offset(off), "\u200B", tags);

    // Offset, not a mark: buffer content is immutable while viewed, and each
    // extra GtkTextMark taxes every later edit (see heading anchors).
    m_images.push_back(
            {.offset = off, .url = img.url, .alt = img.alt, .pixbuf = pixbuf, .texture = {}, .failed = false});
    ++off; // past the zero-width marker

    m_buffer->insert(m_buffer->get_iter_at_offset(off), "\n");
    iter = m_buffer->get_iter_at_offset(off + 1);
}

void MarkdownView::setup_color_detection() {
    auto settings = Gtk::Settings::get_default();
    if (settings) {
        Glib::ustring theme;
        settings->get_property("gtk-theme-name", theme);
        m_dark_mode = theme.find("dark") != Glib::ustring::npos || theme.find("Dark") != Glib::ustring::npos;

        // Update colors when theme changes (connection auto-disconnects on widget destroy)
        auto settings_ref = settings;
        m_connections.emplace_back(settings_ref->property_gtk_theme_name().signal_changed().connect([this,
                                                                                                     settings_ref]() {
            Glib::ustring theme;
            settings_ref->get_property("gtk-theme-name", theme);
            m_dark_mode = theme.find("dark") != Glib::ustring::npos || theme.find("Dark") != Glib::ustring::npos;
            update_colors();
        }));
    }

    // GTK_THEME swaps the painted theme without touching gtk-theme-name, so
    // checking only the setting misclassifies override sessions as light.
    if (const char *theme_override = g_getenv("GTK_THEME")) {
        const std::string_view theme(theme_override);
        m_dark_mode = theme.contains("dark") || theme.contains("Dark");
    }

    // Set initial code block background color
    update_code_backgrounds();
}

void MarkdownView::load_file(const Glib::ustring &path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        m_buffer->set_text("Error: Cannot open file\n" + path);
        m_has_content = false;
        return;
    }

    std::streamsize file_size = file.tellg();
    if (file_size > static_cast<std::streamsize>(mdview::MAX_INPUT_SIZE)) {
        m_buffer->set_text("Error: File too large (max 2 MB)\n" + path);
        m_has_content = false;
        return;
    }

    file.seekg(0);
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    const auto t0 = std::chrono::steady_clock::now();
    auto result = mdview::parse_markdown(content);
    if (g_getenv("MDVIEW_DEBUG_PERF")) {
        const long long ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[perf] parse %lldms\n", ms);
    }
    if (!result) {
        m_buffer->set_text("Error: Failed to parse markdown document.");
        m_has_content = false;
        return;
    }
    m_current_path = path;
    load(std::move(*result));
    watch_current_file();
    m_signal_loaded.emit();
}

// Watch the loaded file so external edits (editor, git checkout) refresh the
// view. CHANGED/CHANGES_DONE_HINT are debounced briefly because many editors
// write via temp-file rename bursts.
void MarkdownView::watch_current_file() {
    m_file_monitor.reset();
    if (m_current_path.empty())
        return;
    try {
        m_file_monitor = Gio::File::create_for_path(m_current_path)->monitor_file();
    } catch (const Gio::Error &) {
        return; // no monitor backend: live reload is a nicety, not a requirement
    }
    m_file_monitor->signal_changed().connect(
            [this](const Glib::RefPtr<Gio::File> &, const Glib::RefPtr<Gio::File> &, Gio::FileMonitor::Event event) {
                if (event != Gio::FileMonitor::Event::CHANGED && event != Gio::FileMonitor::Event::CHANGES_DONE_HINT)
                    return;
                if (m_reload_timer)
                    return; // one reload per burst
                m_reload_timer = Glib::TimeoutSource::create(200);
                m_reload_timer->connect([this] {
                    m_reload_timer.reset();
                    reload_preserving_view();
                    return false;
                });
                m_reload_timer->attach();
            });
}

// Re-parse the current file but keep the reader's place: scroll offset and
// navigation history survive, TOC/search rebuild via signal_loaded.
void MarkdownView::reload_preserving_view() {
    if (m_current_path.empty())
        return;
    if (!Gio::File::create_for_path(m_current_path)->query_exists())
        return; // mid-save replacement resolves on the next hint
    const double scroll = get_vadjustment() ? get_vadjustment()->get_value() : 0.0;
    auto back = std::move(m_nav_back);
    auto fwd = std::move(m_nav_fwd);
    load_file(m_current_path);
    m_nav_back = std::move(back);
    m_nav_fwd = std::move(fwd);
    update_nav_actions();
    Glib::signal_idle().connect_once([this, scroll] {
        if (const auto adj = get_vadjustment())
            adj->set_value(scroll);
    });
}

void MarkdownView::update_colors() {
    update_code_backgrounds();
    update_search_colors();
    update_link_colors();
}

void MarkdownView::update_link_colors() const {
    const Glib::ustring fg = m_dark_mode ? "#58a6ff" : "#0b57d0";
    for (const auto &[url, tag]: m_link_tags) {
        tag->property_foreground() = fg;
    }
}

void MarkdownView::update_code_backgrounds() const {
    Glib::ustring bg_color;
    if (m_dark_mode) {
        bg_color = "#2a2a2a";
    } else {
        bg_color = "#f0f0f0";
    }

    if (const auto inline_code_tag = m_tag_table->lookup("inline_code")) {
        inline_code_tag->property_background_set() = false;
    }

    if (const auto code_block_tag = m_tag_table->lookup("code_block")) {
        code_block_tag->property_background() = bg_color;
        code_block_tag->property_paragraph_background() = bg_color;
    }

    if (const auto code_block_padding_top_tag = m_tag_table->lookup("code_block_padding_top")) {
        code_block_padding_top_tag->property_background() = bg_color;
        code_block_padding_top_tag->property_paragraph_background() = bg_color;
    }

    if (const auto code_block_padding_bottom_tag = m_tag_table->lookup("code_block_padding_bottom")) {
        code_block_padding_bottom_tag->property_background() = bg_color;
        code_block_padding_bottom_tag->property_paragraph_background() = bg_color;
    }

    if (const auto code_block_language_tag = m_tag_table->lookup("code_block_language")) {
        code_block_language_tag->property_background() = bg_color;
        code_block_language_tag->property_paragraph_background() = bg_color;
    }

    m_tag_table->foreach ([&](const Glib::RefPtr<Gtk::TextTag> &tag) {
        const Glib::ustring name = tag->property_name();
        if (std::string_view(name.raw()).starts_with("blockquote")) {
            tag->property_background() = bg_color;
            tag->property_paragraph_background() = bg_color;
        }
    });
}

void MarkdownView::setup_search_colors() {
    Gdk::RGBA selected_bg, selected_fg;
    auto style_context = get_style_context();

    if (style_context->lookup_color("theme_selected_bg_color", selected_bg) &&
        style_context->lookup_color("theme_selected_fg_color", selected_fg)) {

        auto highlight_tag = m_tag_table->lookup("search_highlight");
        auto current_tag = m_tag_table->lookup("search_highlight_current");

        if (highlight_tag && current_tag) {
            auto to_hex = [](double component) -> Glib::ustring {
                int value = static_cast<int>(std::round(component * 255.0));
                std::ostringstream oss;
                oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << value;
                return Glib::ustring(oss.str());
            };

            Glib::ustring bg_hex =
                    Glib::ustring::format("#", to_hex(selected_bg.get_red()), to_hex(selected_bg.get_green()),
                                          to_hex(selected_bg.get_blue()));

            Glib::ustring fg_hex =
                    Glib::ustring::format("#", to_hex(selected_fg.get_red()), to_hex(selected_fg.get_green()),
                                          to_hex(selected_fg.get_blue()));

            current_tag->property_background() = bg_hex;
            current_tag->property_foreground() = fg_hex;

            Gdk::RGBA dimmed_bg;
            dimmed_bg.set_rgba(selected_bg.get_red() * 0.6, selected_bg.get_green() * 0.6, selected_bg.get_blue() * 0.6,
                               selected_bg.get_alpha());

            Glib::ustring dimmed_hex = Glib::ustring::format(
                    "#", to_hex(dimmed_bg.get_red()), to_hex(dimmed_bg.get_green()), to_hex(dimmed_bg.get_blue()));

            highlight_tag->property_background() = dimmed_hex;
            highlight_tag->property_foreground() = fg_hex;
        }
    }
}

void MarkdownView::update_search_colors() { setup_search_colors(); }

void MarkdownView::update_list_indent() {
    const int extra = static_cast<int>(m_base_indent * m_zoom);
    if (const auto bullet = m_tag_table->lookup("bullet")) {
        bullet->property_left_margin() = m_measure_margin + extra;
        bullet->property_indent() = extra;
    }
    for (int depth = 1;; ++depth) {
        const auto bullet = m_tag_table->lookup("bullet-d" + std::to_string(depth));
        if (!bullet)
            break;
        bullet->property_left_margin() = m_measure_margin + extra * (depth + 1);
        bullet->property_indent() = extra;
    }
    for (int depth = 0;; ++depth) {
        const auto quote = m_tag_table->lookup("blockquote-d" + std::to_string(depth));
        if (!quote)
            break;
        quote->property_left_margin() = m_measure_margin + quote_pad() + extra * depth;
    }
}

// ---------------------------------------------------------------------------
// Link following
// ---------------------------------------------------------------------------

void MarkdownView::launch_uri(const std::string &uri) {
    // Content-authored URIs must not smuggle control characters into desktop
    // handlers (terminal titles, shell quoting quirks).
    std::string clean;
    clean.reserve(uri.size());
    for (const char c: uri)
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F)
            clean += c;
    auto launcher = Gtk::UriLauncher::create(clean);
    launcher->launch([launcher, uri](const Glib::RefPtr<Gio::AsyncResult> &result) {
        try {
            launcher->launch_finish(result);
        } catch (const Glib::Error &err) {
            g_warning("mdview: failed to open %s: %s", uri.c_str(), err.what());
        }
    });
}

// Scrolls like an internal link but addressed by the anchor mark directly
// (table-of-contents popover); pushes history so Back returns here.
void MarkdownView::jump_to_anchor(const Glib::ustring &anchor) {
    const auto entry = m_anchor_offsets.find(anchor);
    if (entry == m_anchor_offsets.end())
        return;
    if (const auto adj = get_vadjustment())
        m_nav_back.push_back(adj->get_value());
    m_nav_fwd.clear();
    Gtk::TextIter target = m_buffer->get_iter_at_offset(entry->second);
    scroll_to(target, 0.0, 0.5, 0.15);
    update_nav_actions();
}

int MarkdownView::visible_toc_index() const {
    if (m_toc.empty())
        return -1;
    const auto adj = get_vadjustment();
    int bx = 0;
    int by = 0;
    window_to_buffer_coords(Gtk::TextWindowType::TEXT, 0, static_cast<int>(adj ? adj->get_value() : 0) + 2, bx, by);
    Gtk::TextConstIter top;
    get_iter_at_location(top, bx, by);
    int best = -1;
    for (size_t i = 0; i < m_toc.size(); ++i) {
        const auto entry = m_anchor_offsets.find(m_toc[i].anchor);
        if (entry == m_anchor_offsets.end())
            continue;
        if (m_buffer->get_iter_at_offset(entry->second).get_line() > top.get_line())
            break;
        best = static_cast<int>(i);
    }
    return best;
}

void MarkdownView::follow_link(const Glib::ustring &url) {
    const std::string u(url);
    if (u.empty())
        return;

    // Internal anchor: scroll to the matching heading, remembering where we were
    if (u[0] == '#') {
        const auto entry = m_anchor_offsets.find(Glib::ustring(kAnchorPrefix) + slugify(u.substr(1)));
        if (entry != m_anchor_offsets.end()) {
            if (const auto adj = get_vadjustment())
                m_nav_back.push_back(adj->get_value());
            m_nav_fwd.clear();
            Gtk::TextIter target = m_buffer->get_iter_at_offset(entry->second);
            scroll_to(target, 0.0, 0.5, 0.15);
            update_nav_actions();
        }
        return;
    }

    // Relative link: open sibling markdown files in a new window, other files via URI
    if (!has_uri_scheme(u) && !m_current_path.empty()) {
        std::string rel = u;
        if (const auto hash = rel.find('#'); hash != std::string::npos)
            rel.erase(hash);
        if (!rel.empty()) {
            const auto dir = Gio::File::create_for_path(Glib::path_get_dirname(std::string(m_current_path)));
            const auto target = dir->resolve_relative_path(rel);
            if (target->query_exists()) {
                const std::string p = target->get_path();
                if (!p.empty()) {
                    std::string ext;
                    if (const auto dot = p.rfind('.'); dot != std::string::npos)
                        ext = to_lower_ascii(p.substr(dot + 1));
                    if (ext == "md" || ext == "markdown" || ext == "mdown" || ext == "mkd") {
                        m_signal_open_file.emit(p);
                        return;
                    }
                    launch_uri(Gio::File::create_for_path(p)->get_uri());
                    return;
                }
            }
        }
    }

    launch_uri(u);
}

void MarkdownView::navigate_back() {
    const auto adj = get_vadjustment();
    if (!adj || m_nav_back.empty())
        return;
    m_nav_fwd.push_back(adj->get_value());
    adj->set_value(m_nav_back.back());
    m_nav_back.pop_back();
    update_nav_actions();
}

void MarkdownView::navigate_forward() {
    const auto adj = get_vadjustment();
    if (!adj || m_nav_fwd.empty())
        return;
    m_nav_back.push_back(adj->get_value());
    adj->set_value(m_nav_fwd.back());
    m_nav_fwd.pop_back();
    update_nav_actions();
}

void MarkdownView::update_nav_actions() {
    if (m_act_back)
        m_act_back->set_enabled(!m_nav_back.empty());
    if (m_act_fwd)
        m_act_fwd->set_enabled(!m_nav_fwd.empty());
    m_signal_nav_state.emit(!m_nav_back.empty(), !m_nav_fwd.empty());
}

// ---------------------------------------------------------------------------
// Interaction: gestures, hover, context menu
// ---------------------------------------------------------------------------

Gtk::TextConstIter MarkdownView::iter_at_xy(double x, double y) const {
    int bx = 0;
    int by = 0;
    window_to_buffer_coords(Gtk::TextWindowType::TEXT, static_cast<int>(x), static_cast<int>(y), bx, by);
    Gtk::TextConstIter iter;
    get_iter_at_location(iter, bx, by);
    return iter;
}

void MarkdownView::on_primary_pressed(int n_press, double x, double y) {
    if (n_press == 1) {
        m_press_x = x;
        m_press_y = y;
        return;
    }
    if (n_press == 3) {
        // Triple-click inside a code block selects the whole block
        int start = 0;
        int end = 0;
        if (code_block_bounds(iter_at_xy(x, y), start, end)) {
            set_selection(start, end);
            m_click_gesture->set_state(Gtk::EventSequenceState::CLAIMED);
        }
    }
}

void MarkdownView::on_primary_released(int n_press, double x, double y) {
    if (n_press != 1)
        return;
    if (std::hypot(x - m_press_x, y - m_press_y) > 5.0)
        return; // was a drag, not a click

    // A click on a not-yet-fetched remote image loads it instead of placing
    // the cursor or following a link.
    if (const int idx = pending_image_at_point(x, y); idx >= 0) {
        load_remote_image(static_cast<size_t>(idx));
        m_click_gesture->set_state(Gtk::EventSequenceState::CLAIMED);
        return;
    }

    if (const int block = copy_icon_at_point(x, y); block >= 0) {
        const auto [start, end] = m_code_blocks[static_cast<size_t>(block)];
        copy_code_block_range(start, end);
        m_click_gesture->set_state(Gtk::EventSequenceState::CLAIMED);
        return;
    }

    const auto iter = iter_at_xy(x, y);
    Glib::ustring url = url_at(iter);
    if (url.empty()) {
        // Clicking the very last pixel of a link lands past its end
        Gtk::TextConstIter prev = iter;
        if (prev.backward_char())
            url = url_at(prev);
    }
    if (!url.empty())
        follow_link(url);
}

void MarkdownView::on_hover_motion(double x, double y) {
    const bool over_link =
            !url_at(iter_at_xy(x, y)).empty() || pending_image_at_point(x, y) >= 0 || copy_icon_at_point(x, y) >= 0;
    if (over_link != m_pointer_over_link) {
        m_pointer_over_link = over_link;
        set_cursor(over_link ? "pointer" : "");
    }
}

void MarkdownView::on_hover_leave() {
    if (m_pointer_over_link) {
        m_pointer_over_link = false;
        set_cursor("");
    }
}

void MarkdownView::setup_context_menu() {
    m_ctx_actions = Gio::SimpleActionGroup::create();

    const auto add_action = [this](const Glib::ustring &name, sigc::slot<void()> slot) {
        const auto action = Gio::SimpleAction::create(name);
        action->signal_activate().connect([slot](const Glib::VariantBase &) { slot(); });
        // g_action_map_add_action() takes ownership of the reference returned by gobj_copy()
        g_action_map_add_action(G_ACTION_MAP(m_ctx_actions->gobj()), G_ACTION(action->gobj_copy()));
        return action;
    };

    m_act_copy = add_action("copy", sigc::mem_fun(*this, &MarkdownView::copy_selection_text));
    m_act_copy_md = add_action("copy_markdown", sigc::mem_fun(*this, &MarkdownView::copy_selection_markdown));
    add_action("select_all", sigc::mem_fun(*this, &MarkdownView::select_all_buffer));
    m_act_back = add_action("back", sigc::mem_fun(*this, &MarkdownView::navigate_back));
    m_act_fwd = add_action("forward", sigc::mem_fun(*this, &MarkdownView::navigate_forward));
    insert_action_group("ctx", m_ctx_actions);

    const auto menu = Gio::Menu::create();
    const auto nav_section = Gio::Menu::create();
    nav_section->append("Back", "ctx.back");
    nav_section->append("Forward", "ctx.forward");
    const auto selection_section = Gio::Menu::create();
    selection_section->append("Copy", "ctx.copy");
    selection_section->append("Copy as Markdown", "ctx.copy_markdown");
    const auto all_section = Gio::Menu::create();
    all_section->append("Select All", "ctx.select_all");
    menu->append_section(nav_section);
    menu->append_section(selection_section);
    menu->append_section(all_section);

    m_ctx_menu = Gtk::manage(new Gtk::PopoverMenu(menu));
    m_ctx_menu->set_parent(*this);
    update_nav_actions();
}

void MarkdownView::setup_gestures() {
    m_click_gesture = Gtk::GestureClick::create();
    m_click_gesture->set_button(1 /* primary */);
    // CAPTURE so we see presses/releases even when the TextView's internal gesture claims them
    m_click_gesture->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    m_click_gesture->signal_pressed().connect(sigc::mem_fun(*this, &MarkdownView::on_primary_pressed));
    m_click_gesture->signal_released().connect(sigc::mem_fun(*this, &MarkdownView::on_primary_released));
    add_controller(m_click_gesture);

    m_ctx_gesture = Gtk::GestureClick::create();
    m_ctx_gesture->set_button(3 /* secondary */);
    m_ctx_gesture->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    m_ctx_gesture->signal_pressed().connect([this](int /*n_press*/, double x, double y) {
        m_ctx_iter = iter_at_xy(x, y);
        show_context_menu(x, y);
        m_ctx_gesture->set_state(Gtk::EventSequenceState::CLAIMED);
    });
    add_controller(m_ctx_gesture);

    m_motion_controller = Gtk::EventControllerMotion::create();
    m_motion_controller->signal_motion().connect(sigc::mem_fun(*this, &MarkdownView::on_hover_motion));
    m_motion_controller->signal_leave().connect(sigc::mem_fun(*this, &MarkdownView::on_hover_leave));
    add_controller(m_motion_controller);

    // Mouse back/forward buttons navigate the anchor-jump history
    for (unsigned button: {8u, 9u}) {
        auto gesture = Gtk::GestureClick::create();
        gesture->set_button(static_cast<int>(button));
        gesture->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        gesture->signal_pressed().connect([this, button](int, double, double) {
            if (button == 8)
                navigate_back();
            else
                navigate_forward();
        });
        add_controller(gesture);
    }
}

void MarkdownView::show_context_menu(double x, double y) {
    const bool has_sel = m_buffer->get_has_selection();
    m_act_copy->set_enabled(has_sel);
    m_act_copy_md->set_enabled(has_sel);
    m_ctx_menu->set_pointing_to({static_cast<int>(x), static_cast<int>(y), 1, 1});
    m_ctx_menu->popup();
}

// ---------------------------------------------------------------------------
// Copy helpers
// ---------------------------------------------------------------------------

Glib::RefPtr<Gdk::Clipboard> MarkdownView::clip() const { return Gdk::Display::get_default()->get_clipboard(); }

bool MarkdownView::selection_bounds(Gtk::TextConstIter &start, Gtk::TextConstIter &end) const {
    if (!m_buffer->get_has_selection())
        return false;
    start = m_buffer->get_iter_at_mark(m_buffer->get_insert());
    end = m_buffer->get_iter_at_mark(m_buffer->get_selection_bound());
    if (start.get_offset() > end.get_offset())
        std::swap(start, end);
    return start.get_offset() != end.get_offset();
}

void MarkdownView::set_selection(int start, int end) {
    const Gtk::TextIter start_iter = m_buffer->get_iter_at_offset(start);
    const Gtk::TextIter end_iter = m_buffer->get_iter_at_offset(end);
    m_buffer->move_mark(m_buffer->get_selection_bound(), start_iter);
    m_buffer->move_mark(m_buffer->get_insert(), end_iter);
}

void MarkdownView::copy_selection_text() {
    Gtk::TextConstIter start;
    Gtk::TextConstIter end;
    if (!selection_bounds(start, end))
        return;
    clip()->set_text(start.get_slice(end));
    m_signal_copied.emit("Selection copied");
}

void MarkdownView::copy_selection_markdown() {
    Gtk::TextConstIter start;
    Gtk::TextConstIter end;
    if (!selection_bounds(start, end))
        return;
    clip()->set_text(build_markdown(start, end));
    m_signal_copied.emit("Copied as Markdown");
}

void MarkdownView::select_all_buffer() { set_selection(0, m_buffer->end().get_offset()); }

bool MarkdownView::code_block_at_point(double x, double y, int &start, int &end) const {
    return code_block_bounds(iter_at_xy(x, y), start, end);
}

void MarkdownView::copy_code_block_range(int start, int end) {
    clip()->set_text(extract_code_text(start, end));
    m_signal_copied.emit("Code copied");
}

bool MarkdownView::code_block_bounds(const Gtk::TextConstIter &iter, int &start, int &end) const {
    Gtk::TextConstIter ls = iter;
    ls.set_line_offset(0);

    const auto is_code = [](const LineInfo &li) {
        return li.kind == LineKind::CodeLine || li.kind == LineKind::CodeLang;
    };

    auto it = m_line_info.find(ls.get_offset());
    if (it == m_line_info.end() || !is_code(it->second))
        return false;

    auto first = it;
    while (first != m_line_info.begin()) {
        // flat_map iterators have input-category traits in libstdc++, so
        // std::prev/std::next assert or degrade; step a copy manually.
        // flat_map iterators have input-category traits in libstdc++, so
        // std::prev/std::next assert or degrade; step a copy manually.
        auto prev_it = first;
        --prev_it;
        if (!is_code(prev_it->second))
            break;
        first = prev_it;
    }
    auto last = it;
    auto next_it = it;
    ++next_it;
    while (next_it != m_line_info.end() && is_code(next_it->second)) {
        last = next_it;
        ++next_it;
    }

    // Trim trailing padding-artifact lines (rendered as truly empty lines)
    const auto line_is_empty = [this](int offset) {
        const Gtk::TextIter s = m_buffer->get_iter_at_offset(offset);
        Gtk::TextIter e = s;
        if (!e.ends_line())
            e.forward_to_line_end();
        return s.get_slice(e).empty();
    };
    while (last != first && line_is_empty(last->first))
        --last;

    start = first->first;
    Gtk::TextIter e = m_buffer->get_iter_at_offset(last->first);
    if (!e.ends_line())
        e.forward_to_line_end();
    end = e.get_offset() + 1;
    return true;
}

const MarkdownView::ImagePlacement *MarkdownView::image_at_anchor(const Gtk::TextIter &iter) const {
    for (const auto &img: m_images) {
        if (iter.get_offset() == img.offset)
            return &img;
    }
    return nullptr;
}

Glib::ustring MarkdownView::extract_code_text(int start, int end) const {
    std::vector<std::string> lines;
    Gtk::TextIter it = m_buffer->get_iter_at_offset(start);
    while (it.get_offset() < end) {
        const int lo = it.get_offset();
        Gtk::TextIter le = it;
        if (!le.ends_line())
            le.forward_to_line_end();
        if (line_info_at(lo).kind != LineKind::CodeLang) {
            std::string raw(it.get_slice(le));
            if (raw.starts_with("  "))
                raw.erase(0, 2);
            lines.push_back(raw);
        }
        if (!it.forward_line())
            break;
    }
    Glib::ustring out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0)
            out += "\n";
        out += Glib::ustring(lines[i]);
    }
    return out;
}

Glib::ustring MarkdownView::build_inline_markdown(int start_offset, int end_offset) const {
    struct Style {
        bool bold = false;
        bool italic = false;
        bool strike = false;
        bool code = false;
        std::string url;
        bool operator==(const Style &) const = default;
    };

    const auto style_at = [](const Gtk::TextIter &it) {
        Style st;
        for (const auto &tag: it.get_tags()) {
            const Glib::ustring name = tag->property_name();
            if (name == "bold")
                st.bold = true;
            else if (name == "italic")
                st.italic = true;
            else if (name == "strikethrough")
                st.strike = true;
            else if (name == "inline_code")
                st.code = true;
            else if (std::string_view(name.raw()).starts_with(kUrlTagPrefix)) {
                auto *raw = const_cast<GtkTextTag *>(tag->gobj());
                if (const void *url = g_object_get_data(G_OBJECT(raw), kUrlDataKey))
                    st.url = static_cast<const char *>(url);
            }
        }
        return st;
    };

    const auto emit_run = [](const Glib::ustring &text, const Style &st) -> Glib::ustring {
        if (text.empty())
            return {};
        size_t lead = 0;
        size_t trail = text.size();
        while (lead < trail && std::isspace(static_cast<unsigned char>(text[lead])))
            ++lead;
        while (trail > lead && std::isspace(static_cast<unsigned char>(text[trail - 1])))
            --trail;
        if (lead == trail)
            return text;

        std::string pre;
        std::string post;
        if (st.code) {
            pre = "`";
            post = "`";
        } else {
            if (!st.url.empty()) {
                pre += "[";
                post = "](" + st.url + ")" + post;
            }
            if (st.bold) {
                pre += "**";
                post = "**" + post;
            }
            if (st.italic) {
                pre += "*";
                post = "*" + post;
            }
            if (st.strike) {
                pre += "~~";
                post = "~~" + post;
            }
        }
        return Glib::ustring(text.substr(0, lead)) + pre + text.substr(lead, trail - lead) + post + text.substr(trail);
    };

    Glib::ustring out;
    Gtk::TextIter it = m_buffer->get_iter_at_offset(start_offset);
    while (it.get_offset() < end_offset) {
        // Image anchors hold a zero-width marker; rebuild the ![alt](url)
        // source from the placement instead of emitting the invisible char.
        if (const auto img = image_at_anchor(it); img) {
            out += "![" + Glib::ustring(img->alt) + "](" + Glib::ustring(img->url) + ")";
            it.forward_char();
            continue;
        }
        const Style st = style_at(it);
        Gtk::TextIter run_end = it;
        do {
            run_end.forward_char();
        } while (run_end.get_offset() < end_offset && style_at(run_end) == st);
        out += emit_run(it.get_slice(run_end), st);
        it = run_end;
    }
    return out;
}

Glib::ustring MarkdownView::build_markdown(const Gtk::TextConstIter &sel_start,
                                           const Gtk::TextConstIter &sel_end) const {
    const auto slice = [this](int a, int b) -> Glib::ustring {
        if (b <= a)
            return {};
        return m_buffer->get_iter_at_offset(a).get_slice(m_buffer->get_iter_at_offset(b));
    };

    Glib::ustring out;
    bool in_code = false;
    std::string lang;
    std::vector<std::string> table_lines;

    const auto flush_code = [&]() {
        if (in_code) {
            out += "```\n";
            in_code = false;
            lang.clear();
        }
    };

    const auto flush_table = [&]() {
        if (table_lines.empty())
            return;
        bool header_done = false;
        size_t cols = 0;
        for (const auto &raw: table_lines) {
            bool border = true;
            bool pipe = false;
            for (gunichar ch: Glib::ustring(raw)) {
                if (ch == 0x2502)
                    pipe = true;
                else if (!is_box_char(ch) && ch != ' ')
                    border = false;
            }
            if (border || !pipe)
                continue;

            std::vector<std::string> cells;
            std::string cur;
            for (gunichar ch: Glib::ustring(raw)) {
                if (ch == 0x2502) {
                    cells.push_back(cur);
                    cur.clear();
                } else {
                    char buf[8];
                    const int n = g_unichar_to_utf8(ch, buf);
                    cur.append(buf, n);
                }
            }
            cells.push_back(cur);
            if (cells.size() >= 2) {
                cells.erase(cells.begin());
                cells.pop_back();
            }
            cols = cells.size();

            Glib::ustring row = "|";
            for (const auto &cell: cells)
                row += " " + Glib::ustring(trim_ws(cell)) + " |";
            out += row + "\n";

            if (!header_done) {
                Glib::ustring sep = "|";
                for (size_t c = 0; c < cols; ++c)
                    sep += " --- |";
                out += sep + "\n";
                header_done = true;
            }
        }
        table_lines.clear();
    };

    // Recover the fence language even when the selection starts mid-block
    const auto find_lang = [this](int line_start) {
        auto it = m_line_info.lower_bound(line_start);
        while (it != m_line_info.begin()) {
            --it;
            if (it->second.kind == LineKind::CodeLang)
                return it->second.info;
            if (it->second.kind != LineKind::CodeLine)
                break;
        }
        return std::string();
    };

    Gtk::TextConstIter cur = sel_start;
    while (true) {
        Gtk::TextConstIter lstart = cur;
        lstart.set_line_offset(0);
        const int lo = lstart.get_offset();
        if (lo >= sel_end.get_offset())
            break;

        Gtk::TextConstIter lend = lstart;
        if (!lend.ends_line())
            lend.forward_to_line_end();
        const int le = lend.get_offset();
        const int a = std::max(sel_start.get_offset(), lo);
        const int b = std::min(sel_end.get_offset(), le);

        const LineInfo info = line_info_at(lo);
        switch (info.kind) {
            case LineKind::CodeLang:
                lang = info.info;
                break;
            case LineKind::CodeLine: {
                const std::string raw(slice(lo, le));
                if (raw.empty())
                    break; // padding artifact
                if (lang.empty())
                    lang = find_lang(lo);
                if (!in_code) {
                    out += "```" + Glib::ustring(lang) + "\n";
                    in_code = true;
                }
                std::string body = raw;
                if (body.starts_with("  "))
                    body.erase(0, 2);
                out += Glib::ustring(body) + "\n";
                break;
            }
            case LineKind::HRule:
                flush_code();
                flush_table();
                out += "---\n";
                break;
            case LineKind::TableLine:
                flush_code();
                table_lines.push_back(std::string(slice(lo, le)));
                break;
            default: {
                flush_code();
                flush_table();
                if (a >= b)
                    break;
                Glib::ustring body = build_inline_markdown(a, b);
                std::string prefix;
                if (info.kind == LineKind::Heading) {
                    int level = 1;
                    if (!info.info.empty() &&
                        std::from_chars(info.info.data(), info.info.data() + info.info.size(), level).ec != std::errc{})
                        level = 1;
                    prefix = std::string(static_cast<size_t>(std::clamp(level, 1, 4)), '#') + " ";
                } else if (info.kind == LineKind::ListItem) {
                    // info.info carries "<kind><depth><x|->" from record_list_lines.
                    char kind = 'u';
                    int depth = 0;
                    bool checked = false;
                    if (info.info.size() >= 3) {
                        kind = info.info[0];
                        std::from_chars(info.info.data() + 1, info.info.data() + info.info.size() - 1, depth);
                        checked = info.info.back() == 'x';
                    }
                    prefix = std::string(static_cast<size_t>(2 * depth), ' ');
                    prefix += kind == 'o' ? "1. " : "- ";
                    if (kind == 't')
                        prefix += checked ? "[x] " : "[ ] ";
                    // Strip the rendered marker by CHARACTERS: ustring::substr counts
                    // chars, not bytes, and bullet/checkbox glyphs are one char each.
                    const std::string_view text = body.raw();
                    size_t cut = 0;
                    if (text.starts_with("\u2022 ") || text.starts_with("\u2611 ") || text.starts_with("\u2610 "))
                        cut = 2;
                    else {
                        size_t digits = 0;
                        while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9')
                            ++digits;
                        if (digits > 0 && digits + 1 < text.size() && text[digits] == '.' && text[digits + 1] == ' ')
                            cut = digits + 2; // ASCII prefix: byte offset == char offset
                    }
                    if (cut > 0)
                        body = body.substr(cut);
                } else if (info.kind == LineKind::BlockQuote) {
                    int depth = 0;
                    if (!info.info.empty())
                        std::from_chars(info.info.data(), info.info.data() + info.info.size(), depth);
                    prefix = std::string(static_cast<size_t>(depth + 1), '>') + " ";
                }
                out += Glib::ustring(prefix) + body + "\n";
                break;
            }
        }

        if (!cur.forward_line())
            break;
    }

    flush_code();
    flush_table();
    return out;
}

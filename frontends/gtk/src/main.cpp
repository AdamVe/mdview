#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <graphene.h>
#include <print>
#include "app.hpp"
#include "markdown_view.hpp"

namespace {
    void pump_main_context(int max_iterations = 20) {
        for (int i = 0; i < max_iterations && g_main_context_pending(nullptr); ++i)
            g_main_context_iteration(nullptr, false);
    }

    // Headless mode: render the document into a PNG at full height instead of
    // opening a window (for tests and CI). Usage:
    //   mdview --render-png OUT.png [--width N] [--zoom F] FILE.md
    int render_png_mode(const Glib::ustring &doc_path, const Glib::ustring &out_path, int width, float zoom) {
        setenv("GSK_RENDERER", "cairo", 1);
        const auto app = Gtk::Application::create("com.adamve.mdview.render", Gio::Application::Flags::NON_UNIQUE);
        bool wrote = false;

        app->signal_activate().connect([&] {
            Gtk::Window win;
            auto *view = Gtk::manage(new MarkdownView());
            win.set_child(*view);
            win.set_default_size(width, 800);
            view->load_file(doc_path);
            if (zoom != 1.0f)
                view->set_zoom(zoom);
            win.show(); // visibility flag drives snapshot; map stays pending till we return
            GtkWidget *w = GTK_WIDGET(win.gobj());
            pump_main_context();

            // Child-anchor widgets (embedded images) only join the text layout
            // after their own size negotiation, so iterate measure/allocate
            // until the full document height converges.
            int height = 800;
            for (int pass = 0; pass < 4; ++pass) {
                gint min_h = 0;
                gint nat_h = 0;
                gtk_widget_measure(w, GTK_ORIENTATION_VERTICAL, width, &min_h, &nat_h, nullptr, nullptr);
                const int next = std::clamp(static_cast<int>(nat_h), 1, 20000);
                const GtkAllocation allocation{0, 0, width, next};
                gtk_widget_size_allocate(w, const_cast<GtkAllocation *>(&allocation), -1);
                pump_main_context();
                if (next == height)
                    break;
                height = next;
            }

            if (g_getenv("MDVIEW_DEBUG_TAGS")) {
                GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view->gobj()));
                for (int line: {2, 3}) { // border row and header row of table 1
                    GtkTextIter it;
                    gtk_text_buffer_get_iter_at_line_offset(buf, &it, line, 2);
                    GSList *tags = gtk_text_iter_get_tags(&it);
                    for (GSList *l = tags; l; l = l->next) {
                        GObject *t = G_OBJECT(l->data);
                        gdouble sc = 0;
                        gchar *fam = nullptr;
                        gint rise = 0;
                        g_object_get(t, "scale", &sc, "family", &fam, "rise", &rise, nullptr);
                        gboolean has_scale = FALSE, has_lh = FALSE;
                        g_object_get(t, "scale-set", &has_scale, "line-height-set", &has_lh, nullptr);
                        std::print(stderr, "[tags] line={} tag={:p} scale={}(\{}) fam={} rise={} lhset={}\n", line,
                                   static_cast<const void *>(t), sc, static_cast<int>(has_scale), fam ? fam : "-", rise,
                                   static_cast<int>(has_lh));
                        g_free(fam);
                    }
                    g_slist_free(tags);
                }
            }

            if (g_getenv("MDVIEW_DEBUG_GRID")) {
                GtkTextView *tv = GTK_TEXT_VIEW(view->gobj());
                GtkTextBuffer *buf = gtk_text_view_get_buffer(tv);
                const int n_lines = gtk_text_buffer_get_line_count(buf);
                for (int line = 0; line < n_lines; ++line) {
                    GtkTextIter it;
                    gtk_text_buffer_get_iter_at_line_offset(buf, &it, line, 2);
                    gtk_text_iter_forward_to_line_end(&it);
                    if (gtk_text_iter_backward_char(&it)) {
                        GdkRectangle r{};
                        gtk_text_view_get_iter_location(tv, &it, &r);
                        gunichar ch = gtk_text_iter_get_char(&it);
                        std::print(stderr, "[grid] line={} x={} y={} last_cp={:04X}\n", line, r.x, r.y, ch);
                    }
                }
            }

            const auto *klass = GTK_WIDGET_GET_CLASS(w);
            GtkSnapshot *snapshot = gtk_snapshot_new();
            if (klass->snapshot)
                klass->snapshot(w, snapshot);
            GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);

            if (node) {
                if (GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(w))) {
                    graphene_rect_t viewport =
                            GRAPHENE_RECT_INIT(0, 0, static_cast<float>(width), static_cast<float>(height));
                    if (GdkTexture *texture = gsk_renderer_render_texture(renderer, node, &viewport)) {
                        wrote = gdk_texture_save_to_png(texture, out_path.c_str());
                        g_object_unref(texture);
                    }
                }
                gsk_render_node_unref(node);
            }
            // Teardown of a mapped-but-unrealized GTK stack can stall without a
            // session bus; everything needed is on disk already.
            std::_Exit(wrote ? 0 : 1);
        });

        if (!app->register_application())
            return 1;
        app->activate(); // synchronous local activation; no main loop needed
        return wrote ? 0 : 1; // reached only if activation never ran
    }
} // namespace

int main(int argc, char *argv[]) {
    Glib::ustring png_out;
    Glib::ustring doc_path;
    int width = 1280;
    float zoom = 1.0f;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--render-png") == 0 && i + 1 < argc) {
            png_out = argv[++i];
        } else if (std::strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            const std::string_view arg = argv[++i];
            if (std::from_chars(arg.data(), arg.data() + arg.size(), width).ec != std::errc{} || width < 16 ||
                width > 20000) {
                std::print(stderr, "mdview: invalid --width '{}' (expected 16..20000)\n", arg);
                return 2;
            }
        } else if (std::strcmp(argv[i], "--zoom") == 0 && i + 1 < argc) {
            const std::string_view arg = argv[++i];
            if (std::from_chars(arg.data(), arg.data() + arg.size(), zoom).ec != std::errc{} || !std::isfinite(zoom) ||
                zoom < 0.2f || zoom > 4.0f) {
                std::print(stderr, "mdview: invalid --zoom '{}' (expected finite 0.2..4.0)\n", arg);
                return 2;
            }
        } else if (argv[i][0] != '-') {
            doc_path = argv[i];
        }
    }

    if (!png_out.empty()) {
        if (doc_path.empty()) {
            std::print(stderr, "mdview: --render-png requires a markdown file\n");
            return 2;
        }
        return render_png_mode(doc_path, png_out, width > 0 ? width : 1280, zoom);
    }

    auto app = App::create();
    return app->run(argc, argv);
}

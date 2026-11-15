// PNG render regression tests: drive `mdview --render-png` as a subprocess
// and assert on the decoded output with GdkPixbuf (no Python, no image libs).
// Covers block layout sanity/determinism, table column alignment via the
// MDVIEW_DEBUG_GRID dump, and image/placeholder painting by pixel color.

#include <glibmm.h>
#include <gtest/gtest.h>
#include <gtkmm.h>
#include <gtkmm/init.h>

#include <algorithm>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace {

    std::string g_mdview;
    std::string g_fixtures;

    struct RenderResult {
        int exit_code = -1;
        std::string stderr_text;
        Glib::RefPtr<Gdk::Pixbuf> pixbuf;
    };

    RenderResult render_png(const std::string &fixture, int width = 900, const char *env_name = nullptr) {
        RenderResult out;
        const std::string png = Glib::build_filename(Glib::get_tmp_dir(), "mdview-render-test.png");
        // gtkmm's Subprocess::create has no env parameter; inherit and toggle.
        if (env_name)
            g_setenv(env_name, "1", TRUE);
        try {
            const auto proc = Gio::Subprocess::create({g_mdview, "--render-png", png, "--width", std::to_string(width),
                                                       Glib::build_filename(g_fixtures, fixture)},
                                                      Gio::Subprocess::Flags::STDERR_PIPE);
            proc->wait();
            if (env_name)
                g_unsetenv(env_name);
            out.exit_code = proc->get_exit_status();
            const auto pipe = proc->get_stderr_pipe();
            while (true) {
                const auto chunk = pipe->read_bytes(65536, nullptr);
                gsize size = 0;
                const auto *data = chunk ? static_cast<const char *>(chunk->get_data(size)) : nullptr;
                if (!data || size == 0)
                    break;
                out.stderr_text.append(data, size);
            }
            if (proc->get_successful() && Glib::file_test(png, Glib::FileTest::EXISTS))
                out.pixbuf = Gdk::Pixbuf::create_from_file(png);
        } catch (const Glib::Error &e) {
            if (env_name)
                g_unsetenv(env_name);
            ADD_FAILURE() << "spawn failed: " << e.what();
        }
        return out;
    }

    int count_matching(const Glib::RefPtr<Gdk::Pixbuf> &pb, int r, int g, int b, int tol = 2) {
        const int w = pb->get_width();
        const int h = pb->get_height();
        const int rowstride = pb->get_rowstride();
        const int channels = pb->get_n_channels();
        const guint8 *px = pb->get_pixels();
        int n = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const guint8 *p = px + static_cast<size_t>(y) * rowstride + static_cast<size_t>(x) * channels;
                if (std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - b) <= tol)
                    ++n;
            }
        }
        return n;
    }

    // Group consecutive box-drawing rows from [grid] dumps; return per-table
    // max spread of the last character's x coordinate.
    std::vector<int> grid_alignment_spreads(const std::string &dump) {
        int prev_line = -2;
        std::vector<int> spreads;
        std::vector<int> cur;
        std::istringstream stream(dump);
        std::string line_text;
        while (std::getline(stream, line_text)) {
            std::smatch m;
            static const std::regex re(R"(\[grid\] line=(\d+) x=(\d+) y=\d+ last_cp=(\w+))");
            if (!std::regex_search(line_text, m, re))
                continue;
            const int line = std::stoi(m[1]);
            const int x = std::stoi(m[2]);
            const bool is_row = m[3].str().starts_with("25"); // U+250x box drawing
            if (is_row && line == prev_line + 1) {
                cur.push_back(x);
            } else {
                if (!cur.empty())
                    spreads.push_back(*std::ranges::max_element(cur) - *std::ranges::min_element(cur));
                cur = is_row ? std::vector<int>{x} : std::vector<int>{};
            }
            prev_line = line;
        }
        if (!cur.empty())
            spreads.push_back(*std::ranges::max_element(cur) - *std::ranges::min_element(cur));
        return spreads;
    }

    TEST(RenderPng, BlocksRenderCleanlyAndDeterministically) {
        const auto first = render_png("blocks.md");
        ASSERT_EQ(first.exit_code, 0) << first.stderr_text;
        EXPECT_EQ(first.stderr_text.find("CRITICAL"), std::string::npos);
        ASSERT_TRUE(first.pixbuf);
        EXPECT_EQ(first.pixbuf->get_width(), 900);
        EXPECT_GT(first.pixbuf->get_height(), 400);

        const auto second = render_png("blocks.md");
        ASSERT_TRUE(second.pixbuf);
        ASSERT_EQ(first.pixbuf->get_height(), second.pixbuf->get_height());
        EXPECT_EQ(0, std::memcmp(first.pixbuf->get_pixels(), second.pixbuf->get_pixels(),
                                 static_cast<size_t>(first.pixbuf->get_height()) * first.pixbuf->get_rowstride()));
    }

    TEST(RenderPng, TablesArePixelAlignedPerTable) {
        const auto result = render_png("tables.md", 900, "MDVIEW_DEBUG_GRID");
        ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
        const auto spreads = grid_alignment_spreads(result.stderr_text);
        ASSERT_GE(spreads.size(), 2u) << "expected >=2 tables in grid dump";
        for (size_t i = 0; i < spreads.size(); ++i)
            EXPECT_LE(spreads[i], 2) << "table " << (i + 1) << " column misalignment";
    }

    TEST(RenderPng, ImagesAndPlaceholdersPaint) {
        const auto result = render_png("images.md");
        ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
        ASSERT_TRUE(result.pixbuf);
        // sample.png is 60x40 solid red = 2400 px at natural size
        EXPECT_GE(count_matching(result.pixbuf, 255, 0, 0), 2000);
        // remote image shows the placeholder fill (#f2f2f4), never a fetch attempt
        EXPECT_GE(count_matching(result.pixbuf, 242, 242, 244), 320 * 180 / 2);
    }

    TEST(RenderPng, SvgIsDeferredUntilClick) {
        const auto result = render_png("svg_defer.md");
        ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
        ASSERT_TRUE(result.pixbuf);
        // Local SVG must show the click-to-load placeholder (#f2f2f4), never a
        // librsvg decode: its internals reference an unreachable http href.
        EXPECT_GE(count_matching(result.pixbuf, 242, 242, 244), 320 * 180 / 2);
    }

    TEST(RenderPng, CliRejectsAbsurdWidth) {
        try {
            const auto proc =
                    Gio::Subprocess::create({g_mdview, "--render-png", "/tmp/opencode/mv/cli-width.png", "--width",
                                             "2000000000", Glib::build_filename(g_fixtures, "blocks.md")},
                                            Gio::Subprocess::Flags::STDERR_PIPE);
            proc->wait();
            EXPECT_EQ(proc->get_exit_status(), 2); // rejected before any allocation
        } catch (const Glib::Error &e) {
            ADD_FAILURE() << "spawn failed: " << e.what();
        }
    }

} // namespace

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s PATH_TO_MDVIEW FIXTURES_DIR\n", argv[0]);
        return 2;
    }
    g_mdview = argv[1];
    g_fixtures = argv[2];
    // An explicit test display beats whatever session environment leaked in
    // (a live desktop's Xwayland may be reachable-but-degraded for sandboxes).
    if (const char *d = g_getenv("MDVIEW_TEST_DISPLAY")) {
        g_setenv("DISPLAY", d, TRUE);
        g_unsetenv("WAYLAND_DISPLAY");
    }
    // --render-png renders through a real GdkDisplay/GskRenderer pair.
    if (g_getenv("DISPLAY") == nullptr && g_getenv("WAYLAND_DISPLAY") == nullptr) {
        std::printf("SKIP: no DISPLAY or WAYLAND_DISPLAY available\n");
        return 77; // meson treats exit code 77 as "skipped"
    }
    // Pin the backend when an X display exists: a leaked session environment
    // (Xwayland + foreign XAUTHORITY + Wayland compositor) otherwise makes
    // child renders silently degrade instead of failing loudly.
    if (g_getenv("DISPLAY") != nullptr)
        g_setenv("GDK_BACKEND", "x11", TRUE);
    Gio::init(); // glibmm wrap tables for subprocess streams
    gtk_init();
    Gtk::init_gtkmm_internals(); // gdkmm wrap tables (GdkPixbuf)
    return RUN_ALL_TESTS();
}

// Frontend integration tests for MarkdownView block mapping. These exercise
// the real parse -> load -> line-map pipeline, including the backward walk
// over the flat_map line info that regressed into a std::prev assertion
// (right-click inside a code block crashed).

#include <cstdlib>
#include <unistd.h>

#include <fstream>
#include <gtest/gtest.h>
#include <gtkmm.h>
#include <string>

#include "markdown_view.hpp"
#include "mdview/parser.hpp"

namespace {

    constexpr const char *kDoc = "# Title\n"
                                 "\n"
                                 "paragraph before\n"
                                 "\n"
                                 "```cpp\n"
                                 "int a;\n"
                                 "int b;\n"
                                 "```\n"
                                 "\n"
                                 "## Section Two\n"
                                 "\n"
                                 "trailing paragraph\n"
                                 "\n"
                                 "![Remote logo](https://example.com/logo.png)\n";

    // One realized view for the whole suite: widgets are expensive to spin up
    // headless, and per-test teardown of GTK stacks can stall without a session
    // bus. Leaked deliberately; main() exits hard after RUN_ALL_TESTS.
    MarkdownView *g_view = nullptr;

    class MarkdownViewSuite : public ::testing::Test {
    protected:
        static void SetUpTestSuite() {
            auto *window = new Gtk::ApplicationWindow(); // NOLINT(cppcoreguidelines-owning-memory): leaked by design
            g_view = new MarkdownView(); // NOLINT: leaked by design
            window->set_child(*g_view);
            window->show();
            while (g_main_context_pending(nullptr))
                g_main_context_iteration(nullptr, false);

            const auto doc = mdview::parse_markdown(kDoc);
            ASSERT_TRUE(doc.has_value());
            g_view->load(*doc);
            ASSERT_GT(g_view->get_buffer()->get_line_count(), 5);
        }

        // Line index of the first buffer line containing the given text.
        static int line_containing(const char *needle) {
            const auto buf = g_view->get_buffer();
            const int n = buf->get_line_count();
            for (int i = 0; i < n; ++i) {
                Gtk::TextIter s = buf->get_iter_at_line(i);
                Gtk::TextIter e = s;
                e.forward_to_line_end();
                if (std::string_view(Glib::ustring(s, e).raw()).find(needle) != std::string_view::npos)
                    return i;
            }
            return -1;
        }
    };

    TEST_F(MarkdownViewSuite, CodeLineYieldsFullBlockBounds) {
        const int mid = line_containing("int b"); // exercises the backward walk
        const int first = line_containing("int a");
        ASSERT_GT(mid, 0);
        ASSERT_GT(first, 0);

        int start = 0;
        int end = 0;
        ASSERT_TRUE(g_view->code_block_bounds(g_view->get_buffer()->get_iter_at_line(mid), start, end));
        EXPECT_LE(start, g_view->get_buffer()->get_iter_at_line(first).get_offset());
        EXPECT_GE(end, g_view->get_buffer()->get_iter_at_line(mid).get_offset() + 6);
    }

    TEST_F(MarkdownViewSuite, FirstCodeLineAlsoSpansForward) {
        const int first = line_containing("int a");
        const int last = line_containing("int b");
        ASSERT_GT(first, 0);

        int start = 0;
        int end = 0;
        ASSERT_TRUE(g_view->code_block_bounds(g_view->get_buffer()->get_iter_at_line(first), start, end));
        EXPECT_GE(end, g_view->get_buffer()->get_iter_at_line(last).get_offset() + 1);
    }

    TEST_F(MarkdownViewSuite, CopyAsMarkdownReconstructsImage) {
        // Remote images never load eagerly and their placeholder line holds
        // only the zero-width marker, so locate the image line by that char.
        const int img_line = line_containing("\u200B");
        ASSERT_GT(img_line, 0);
        // Remote images never load eagerly, so this also proves the copy works
        // straight from the parsed source, not from decoded pixels.
        const auto buf = g_view->get_buffer();
        Gtk::TextIter line_end = buf->get_iter_at_line(img_line);
        line_end.forward_to_line_end();
        const Glib::ustring md = g_view->build_markdown(buf->get_iter_at_line(img_line), line_end);
        // build_markdown reconstructs whole lines, so it terminates with \n.
        EXPECT_EQ(md, "![Remote logo](https://example.com/logo.png)\n");
    }

    TEST_F(MarkdownViewSuite, TocListsHeadingsInOrder) {
        const auto &entries = g_view->toc();
        ASSERT_EQ(entries.size(), 2u);
        EXPECT_EQ(entries[0].level, 1);
        EXPECT_EQ(entries[0].title, "Title");
        EXPECT_EQ(entries[1].level, 2);
        EXPECT_EQ(entries[1].title, "Section Two");
        EXPECT_FALSE(entries[1].anchor.empty());
    }

    TEST_F(MarkdownViewSuite, JumpToAnchorPushesHistory) {
        const auto &entries = g_view->toc();
        ASSERT_FALSE(entries.empty());
        ASSERT_FALSE(g_view->can_navigate_back());
        g_view->jump_to_anchor(entries.back().anchor);
        EXPECT_TRUE(g_view->can_navigate_back());
    }

    TEST_F(MarkdownViewSuite, CodeBlockAtPointFindsBlock) {
        auto buffer = g_view->get_buffer();
        int code_line = -1;
        for (int line = 0; line < buffer->get_line_count(); ++line) {
            const auto start = buffer->get_iter_at_line(line);
            auto stop = start;
            stop.forward_to_line_end();
            if (buffer->get_text(start, stop, false).find("int a;") != Glib::ustring::npos) {
                code_line = line;
                break;
            }
        }
        ASSERT_GE(code_line, 0);
        // Point mapping needs a real allocation; pump until the view is sized.
        for (int i = 0; i < 500 && g_view->get_width() < 10; ++i) {
            while (Glib::MainContext::get_default()->pending())
                Glib::MainContext::get_default()->iteration(false);
            g_usleep(4000);
        }
        if (g_view->get_width() < 10)
            GTEST_SKIP() << "view never allocated (no usable display?)";
        Gdk::Rectangle rect;
        g_view->get_iter_location(buffer->get_iter_at_line(code_line), rect);
        int start = -1;
        int end = -1;
        ASSERT_TRUE(g_view->code_block_at_point(rect.get_x() + 5, rect.get_y() + rect.get_height() / 2, start, end));
        EXPECT_LT(start, end);
    }

    // Live reload runs through Gio::FileMonitor (inotify); the sandboxed CI
    // display issues make pixel checks flaky there, but text content is stable.
    TEST_F(MarkdownViewSuite, ExternalEditReloadsBuffer) {
        char tmpl[] = "/tmp/mdview-watch-XXXXXX.md";
        const int fd = mkstemps(tmpl, 3);
        ASSERT_GE(fd, 0);
        close(fd);
        const std::string path = tmpl;
        auto write_file = [&path](const std::string &text) {
            std::ofstream out(path);
            out << text;
        };
        write_file("# One\n");
        g_view->load_file(path);
        auto buffer = g_view->get_buffer();
        ASSERT_TRUE(buffer->get_text().find("One") != Glib::ustring::npos);
        write_file("# Two\n");
        bool reloaded = false;
        for (int i = 0; i < 500 && !reloaded; ++i) {
            while (Glib::MainContext::get_default()->pending())
                Glib::MainContext::get_default()->iteration(false);
            g_usleep(10000);
            reloaded = buffer->get_text().find("Two") != Glib::ustring::npos;
        }
        unlink(path.c_str());
        EXPECT_TRUE(reloaded) << "file monitor did not trigger a reload within 5s";
        // Shared fixture: hand the standard document back to later tests.
        auto fresh = mdview::parse_markdown(kDoc);
        ASSERT_TRUE(static_cast<bool>(fresh));
        g_view->load(std::move(*fresh));
    }

    TEST_F(MarkdownViewSuite, ParagraphHasNoCodeBlock) {
        const int para = line_containing("paragraph before");
        ASSERT_GT(para, 0);
        int start = 0;
        int end = 0;
        EXPECT_FALSE(g_view->code_block_bounds(g_view->get_buffer()->get_iter_at_line(para), start, end));
    }

} // namespace

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    const auto app = Gtk::Application::create("com.adamve.mdview.unittest", Gio::Application::Flags::NON_UNIQUE);
    if (!app->register_application()) {
        std::printf("SKIP: no display backend available\n");
        return 77; // meson treats exit code 77 as "skipped"
    }
    // Widget teardown of a GTK stack outside a main loop can stall without a
    // session bus (same reason --render-png mode exits hard); results are out.
    std::_Exit(RUN_ALL_TESTS());
}

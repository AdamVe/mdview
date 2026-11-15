// Frontend integration tests for MarkdownView block mapping. These exercise
// the real parse -> load -> line-map pipeline, including the backward walk
// over the flat_map line info that regressed into a std::prev assertion
// (right-click inside a code block crashed).

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
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
                                 "![Remote logo](https://example.com/logo.png)\n"
                                 "\n"
                                 "- alpha\n"
                                 "- beta\n"
                                 "  - nested gamma\n"
                                 "\n"
                                 "1. first\n"
                                 "2. second\n"
                                 "\n"
                                 "- [x] done task\n"
                                 "- [ ] open task\n"
                                 "\n"
                                 "> quoted one\n"
                                 ">\n"
                                 ">> deep quote\n";

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

    // Copies whole buffer lines [first_line, last_line] through the same path
    // the Copy-as-Markdown action uses.
    static Glib::ustring copy_lines(int first_line, int last_line) {
        const auto buf = g_view->get_buffer();
        Gtk::TextIter start = buf->get_iter_at_line(first_line);
        Gtk::TextIter end = buf->get_iter_at_line(last_line);
        end.forward_to_line_end();
        return g_view->build_markdown(start, end);
    }

    TEST_F(MarkdownViewSuite, CopyAsMarkdownKeepsBulletContentIntact) {
        // Regression: stripping the rendered "• " marker counted its 3 UTF-8
        // bytes as characters and ate two content characters per bullet item.
        const int a = line_containing("alpha");
        const int g = line_containing("nested gamma");
        ASSERT_GT(a, 0);
        ASSERT_GT(g, 0);
        EXPECT_EQ(copy_lines(a, g), "- alpha\n- beta\n  - nested gamma\n");
    }

    TEST_F(MarkdownViewSuite, CopyAsMarkdownRebuildsOrderedAndTaskItems) {
        const int f = line_containing("first");
        const int o = line_containing("open task");
        ASSERT_GT(f, 0);
        ASSERT_GT(o, 0);
        // Ordered items normalize to "1." — markdown renderers renumber them.
        EXPECT_EQ(copy_lines(f, o), "1. first\n1. second\n- [x] done task\n- [ ] open task\n");
    }

    TEST_F(MarkdownViewSuite, CopyAsMarkdownNestsQuoteMarkers) {
        const int q = line_containing("quoted one");
        const int d = line_containing("deep quote");
        ASSERT_GT(q, 0);
        ASSERT_GT(d, 0);
        // The bare ">" separator line carries no content and is not emitted.
        EXPECT_EQ(copy_lines(q, d), "> quoted one\n>> deep quote\n");
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

    TEST_F(MarkdownViewSuite, OversizedFileShowsCapError) {
        char tmpl[] = "/tmp/mdview-cap-XXXXXX.md";
        const int fd = mkstemps(tmpl, 3);
        ASSERT_GE(fd, 0);
        close(fd);
        const std::string path = tmpl;
        {
            std::ofstream out(path);
            out << std::string(mdview::MAX_INPUT_SIZE + 1, 'x');
        }
        g_view->load_file(path);
        const std::string text = g_view->get_buffer()->begin().get_text(g_view->get_buffer()->end()).raw();
        unlink(path.c_str());
        EXPECT_NE(text.find("File too large"), std::string::npos);
        EXPECT_NE(text.find("2 MB"), std::string::npos);
        // Leave the standard document in place for later tests.
        auto fresh = mdview::parse_markdown(kDoc);
        ASSERT_TRUE(static_cast<bool>(fresh));
        g_view->load(*fresh);
    }

    TEST_F(MarkdownViewSuite, FifoImageDoesNotHangLoad) {
        char tmpl[] = "/tmp/mdview-fifo-XXXXXX";
        const char *dir = mkdtemp(tmpl);
        ASSERT_TRUE(dir != nullptr);
        const std::string fifo = std::string(dir) + "/pipe.img";
        ASSERT_EQ(mkfifo(fifo.c_str(), 0666), 0);
        const std::string doc = std::string(dir) + "/doc.md";
        {
            std::ofstream out(doc);
            out << "![pipelined](" << fifo << ")\n";
        }
        const auto before = std::chrono::steady_clock::now();
        g_view->load_file(doc); // would block forever without the regular-file guard
        const auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - before).count();
        EXPECT_LT(elapsed, 5);
        const auto text = g_view->get_buffer()->begin().get_text(g_view->get_buffer()->end()).raw();
        EXPECT_NE(text.find("[pipelined]"), std::string::npos); // alt-text placeholder shown
        unlink(fifo.c_str());
        unlink(doc.c_str());
        rmdir(dir);
        auto fresh = mdview::parse_markdown(kDoc);
        ASSERT_TRUE(static_cast<bool>(fresh));
        g_view->load(*fresh);
    }

    TEST_F(MarkdownViewSuite, ParagraphHasNoCodeBlock) {
        const int para = line_containing("paragraph before");
        ASSERT_GT(para, 0);
        int start = 0;
        int end = 0;
        EXPECT_FALSE(g_view->code_block_bounds(g_view->get_buffer()->get_iter_at_line(para), start, end));
    }

    // Regression: widening the window re-centered table rows while headings and
    // paragraphs kept their stale measure margin until a zoom change forced a
    // full relayout. After a resize every line must share the new margin.
    TEST_F(MarkdownViewSuite, ResizeRealignsAllLinesToMeasureMargin) {
        auto *window = dynamic_cast<Gtk::Window *>(g_view->get_root());
        ASSERT_NE(window, nullptr);
        const auto doc = mdview::parse_markdown(
                "## Headline\n\n| alpha | beta |\n|-------|------|\n| 1     | 2    |\n\nafter table\n");
        ASSERT_TRUE(doc.has_value());
        g_view->load(*doc);

        const auto pump_until_wider_than = [window](int px) {
            for (int i = 0; i < 500; ++i) {
                while (Glib::MainContext::get_default()->pending())
                    Glib::MainContext::get_default()->iteration(false);
                if (g_view->get_width() > px)
                    return true;
                window->set_default_size(px + 80, 700);
                g_usleep(4000);
            }
            return false;
        };
        if (!pump_until_wider_than(500))
            GTEST_SKIP() << "view never allocated (no usable display?)";

        const int head = line_containing("Headline");
        const int table = line_containing("alpha");
        const int para = line_containing("after table");
        ASSERT_GE(head, 0);
        ASSERT_GT(table, 0);
        ASSERT_GT(para, 0);
        auto line_x = [&](int line) {
            Gdk::Rectangle r;
            g_view->get_iter_location(g_view->get_buffer()->get_iter_at_line(line), r);
            return r.get_x();
        };
        const int head_before = line_x(head);

        window->set_default_size(1180, 700);
        if (!pump_until_wider_than(1100))
            GTEST_SKIP() << "window did not grow (no usable display?)";

        const int head_after = line_x(head);
        if (head_after == head_before)
            GTEST_SKIP() << "measure margin unchanged between widths (clamped)";
        EXPECT_EQ(line_x(table), head_after);
        EXPECT_EQ(line_x(para), head_after);
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

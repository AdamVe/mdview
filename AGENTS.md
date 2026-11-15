# mdview — Agent Instructions

## Build
```
meson setup build && ninja -C build
./build/frontends/gtk/mdview path/to/file.md
```

Build type defaults to `debugoptimized` (set in the project's `default_options`).
Use a single `build/` dir; any `build*/` variant is gitignored.

### Build options
| Option | Choices | Default |
|---|---|---|
| `-Dfrontend=` | `gtk`, `adwaita` | `gtk` |
| `-Dlink_cmark=` | `dynamic`, `static` | `dynamic` |

Example: `meson setup build -Dfrontend=adwaita -Dlink_cmark=static`

### Run tests
```
meson test -C build                      # all suites
meson test -C build --suite parser       # parser only (suites: parser, gtk, render)
```
The `render` suite rasterizes via GTK and needs a usable display — use
`xvfb-run meson test -C build` when headless. Known gap: if `DISPLAY` is set
but unusable (e.g. a stale X11 value on a Wayland session), `render_png` fails
instead of skipping (exit 77).

### Headless PNG render (no window — use this for visual checks)
```
./build/frontends/gtk/mdview --render-png out.png [--width 1280] [--zoom 1.0] file.md
```
Renders the full document height to a PNG via snapshot + GskRenderer (cairo backend forced). Needs any GdkDisplay (e.g. `DISPLAY=:95` under Xvfb); exits immediately after writing.

## Architecture
- `core/` — pure C++, ZERO platform dependencies. Parses markdown → DocModel via cmark-gfm. **Never add GTK includes here.**
- `frontends/gtk/` — GTK4 + gtkmm renderer. Converts DocModel → GtkTextBuffer via TextTags.

### Entry points
- `frontends/gtk/src/main.cpp` — `Gtk::Application::create()` → `App`
- `core/src/parser.cpp` — cmark AST walker → `mdview::DocModel`
- `frontends/gtk/src/markdown_view.cpp` — DocModel → GtkTextBuffer rendering

### Key types
- `DocModel` = `std::vector<DocNode>` where `DocNode` is a `std::variant` of block types (Heading, Paragraph, CodeBlock, BlockQuote, HRule, List, Table); `List` items are recursive (`ListEntry`: content, marker kind 'u'/'o'/'t', checked flag, children) so nested lists render at any depth
- `InlineNode` = `std::variant<TextRun, Link, Image>` where TextRun carries bold/italic/code/strikethrough flags;
  Image is rendered on its own line (reserved via a line-height tag, painted in `MarkdownView::snapshot_vfunc`);
  http(s) images are lazy (click-to-load placeholder, see `pending_image_at_point` / `load_remote_image`)

## Adding a new DocNode type
1. Add struct to `core/include/mdview/doc_model.hpp`, add to `DocNode` variant
2. Handle cmark node type in `core/src/parser.cpp` `parse_block_node()`
3. Add `std::visit` arm in `frontends/gtk/src/markdown_view.cpp` `insert_node()`
4. Add required TextTags in `markdown_view.cpp` `setup_tags()`

## Style
- **Clang-format:** LLVM-based, 4-space indent, 120-col limit, BreakBeforeBraces: Custom (K&R-ish), `InsertNewlineAtEOF: true`
- **C++ standard:** C++26 (meson `cpp_std=c++26`, GCC 15). In use: `std::expected` + `[[unlikely]]`
  error branches (core), `std::print` debug dumps, `std::flat_map`/`flat_set`, `std::byteswap`,
  `std::from_chars`, `starts_with`, designated initializers (gtk frontend)
- `core/` uses `std::expected` for error handling; frontend uses `sigc++` signals/slots

## cmark linking
- `dynamic` (default): requires system `libcmark-gfm` + extensions dev packages; configure fails fast when absent
- `static`: cmark fetched via Meson wrapdb (v0.29.0.gfm.13) and statically linked — binary is self-contained

## Gotchas
- `subprojects/cmark-gfm-*/`, `subprojects/googletest-*/` and `subprojects/packagecache/` are gitignored — don't commit them
- All `build*/` dirs are gitignored build artifacts
- The adwaita frontend only works on Linux (enforced at meson configure time)

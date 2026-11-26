# Building mdview on macOS

mdview builds natively on macOS against Homebrew's GTK4 stack. There is no
`.app` bundle yet — you run the binary from a terminal.

## Requirements

Homebrew packages (one line):

```bash
brew install llvm gtkmm4 libsoup@3 cmark-gfm meson ninja pkg-config
```

Apple's bundled clang is too old (it lacks `std::expected`); always compile
with Homebrew's LLVM:

```bash
export PATH="$(brew --prefix llvm)/bin:$PATH"
export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig:$(brew --prefix libsoup@3)/lib/pkgconfig"
```

(`libsoup@3` is keg-only, hence its explicit pkg-config path — it backs remote
image loading.)

## Build and run

```bash
meson setup build -Dfrontend=gtk   # adwaita is Linux-only
ninja -C build
./build/frontends/gtk/mdview README.md
```

Tests:

```bash
meson test -C build --suite parser   # always
meson test -C build --suite gtk      # needs a GUI session
```

## macOS 12 (Monterey) notes

Monterey is outside Homebrew's supported window, so treat everything below as
best-effort:

- **Expect builds from source.** Many formulae (especially `llvm`) may have no
  bottle for macOS 12 and will compile locally — LLVM alone can take hours on
  older hardware. Start it and go do something else.
- **Flat containers:** `std::flat_map`/`flat_set` need libc++ 20/21; the code
  falls back to `std::map`/`std::set` automatically, so an older LLVM still
  compiles the frontend.
- **OCLP machines:** if the patcher supports a newer macOS on your hardware,
  installing it turns the whole exercise from "hours of compiling" into a
  routine `brew install`.

## Known caveats on macOS

- Window icons/dock behavior are plain Quartz defaults; no bundle integration.
- Dark mode follows the system appearance through `Gtk::Settings`;
  `GTK_THEME=Adwaita:dark ./build/...` forces it.
- `--render-png` relies on GSK's software path and is untested on Quartz.
- The `.desktop` file and metainfo XML install harmlessly but are unused there.

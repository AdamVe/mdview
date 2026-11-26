# Building mdview on macOS

mdview builds natively on macOS against Homebrew's GTK4 stack. There is no
`.app` bundle yet — you run the binary from a terminal.

Note: Homebrew only ships prebuilt packages for Apple Silicon on currently
supported macOS versions. On Intel Macs or older system releases every
dependency is compiled from source (expect hours) — CI-built artifacts are
the practical alternative there.

## Requirements

```bash
brew install llvm gtkmm4 libsoup@3 cmark-gfm meson ninja pkg-config
```

Apple's bundled clang is too old (it lacks `std::expected`), so always take
the compiler from Homebrew's LLVM (`libsoup@3` is keg-only, hence its
explicit pkg-config path):

```bash
export PATH="$(brew --prefix llvm)/bin:$PATH"
export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig:$(brew --prefix libsoup@3)/lib/pkgconfig"
```

## Build and run

```bash
meson setup build -Dfrontend=gtk   # adwaita is Linux-only
ninja -C build
./build/frontends/gtk/mdview README.md
meson test -C build --suite parser
```

## Known caveats on macOS

- Window icons/dock behavior are plain Quartz defaults; no bundle integration.
- Dark mode follows the system appearance through `Gtk::Settings`;
  `GTK_THEME=Adwaita:dark ./build/...` forces it.
- `--render-png` relies on GSK's software path and is untested on Quartz.
- The `.desktop` file and metainfo XML install harmlessly but are unused there.

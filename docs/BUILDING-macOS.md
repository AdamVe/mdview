# Building mdview on macOS

mdview builds natively on macOS against a GTK4 stack from Homebrew (Apple
Silicon) or Nix (any Mac, including Intel). There is no `.app` bundle yet —
you run the binary from a terminal.

## Which package manager?

Homebrew only ships bottles for Apple Silicon these days, and its supported
macOS window excludes Monterey. On an **Intel Mac, use Nix** ([section
below](#intel-macs-use-nix)) — its binary cache serves prebuilt
`x86_64-darwin` packages regardless of OS version or CPU generation.

## Requirements (Homebrew, Apple Silicon)

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

## Intel Macs: use Nix

Nix installs into your home directory without admin rights and downloads
everything prebuilt:

```bash
# one-time install (user mode, no sudo)
sh <(curl -L https://nixos.org/nix/install) --no-daemon
```

Reopen the Terminal, then in the mdview checkout:

```bash
nix-shell -p clang gtkmm4 cmark-gfm libsoup3 meson ninja pkg-config
meson setup build -Dfrontend=gtk
ninja -C build
./build/frontends/gtk/mdview README.md
```

Nixpkgs' default clang is new enough for every C++ feature the project uses.
Caveat: nixpkgs sets a minimum macOS per release; Monterey is still supported
today, but old OS versions age out over time — pin a nixpkgs revision if that
ever bites you (`nix-shell -p ... -I nixpkgs=<rev>`).

## macOS 12 (Monterey) notes

Monterey is outside Homebrew's supported window. If you're on it because of
OCLP and your hardware supports a newer macOS, installing it first makes the
Homebrew path routine. On Intel (or if you'd rather not compile anything),
just use the [Nix path](#intel-macs-use-nix). If you insist on Homebrew on 12:

- **Expect builds from source.** Many formulae (especially `llvm`) have no
  bottle for macOS 12 and will compile locally — LLVM alone can take hours on
  older hardware. Start it and go do something else.
- **Flat containers:** `std::flat_map`/`flat_set` need libc++ 20/21; the code
  falls back to `std::map`/`std::set` automatically, so an older LLVM still
  compiles the frontend.

## Known caveats on macOS

- Window icons/dock behavior are plain Quartz defaults; no bundle integration.
- Dark mode follows the system appearance through `Gtk::Settings`;
  `GTK_THEME=Adwaita:dark ./build/...` forces it.
- `--render-png` relies on GSK's software path and is untested on Quartz.
- The `.desktop` file and metainfo XML install harmlessly but are unused there.

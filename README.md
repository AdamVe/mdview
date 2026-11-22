# mdview

A native Markdown viewer for GTK4, built with gtkmm.


## Building

Linux is the only currently tested platform.

### Dependencies

- `meson` and `ninja`
- `gtkmm-4.0` development headers — `libgtkmm-4.0-dev` (Debian/Ubuntu), `gtkmm4-devel` (Fedora)
- `libcmark-gfm` + extensions — `libcmark-gfm-dev libcmark-gfm-extensions-dev` (Debian/Ubuntu), `libcmark-gfm-devel` (Fedora)

Alternatively configure with `-Dlink_cmark=static` to fetch cmark via Meson
wraps; that build is self-contained too.

### Build and run

```bash
meson setup build
ninja -C build
./build/frontends/gtk/mdview path/to/file.md
```

## Installation

```bash
sudo ninja -C build install
```

Or into your home directory:

```bash
meson setup build --prefix=$HOME/.local
ninja -C build install
```

## AI Use Disclosure

This project was developed with the assistance of AI coding agents.

## License

MIT — see `LICENSE`.

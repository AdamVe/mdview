#!/usr/bin/env bash
# Build a self-contained mdview AppImage in the repo root.
# cmark is linked statically; linuxdeploy bundles gtkmm/GTK and their runtime data.
# Run locally or from .github/workflows/release.yml (same entrypoint either way).
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR=${BUILD_DIR:-build-appimage}
ARCH=${ARCH:-$(uname -m)} # linuxdeploy supports x86_64 and aarch64

if [ -f "$BUILD_DIR/build.ninja" ]; then
    meson setup --reconfigure "$BUILD_DIR" --prefix=/usr -Dlink_cmark=static
else
    meson setup "$BUILD_DIR" --prefix=/usr -Dlink_cmark=static
fi
ninja -C "$BUILD_DIR"

APPDIR="$PWD/$BUILD_DIR/AppDir"
DESTDIR="$APPDIR" ninja -C "$BUILD_DIR" install

# normalize Exec for linuxdeploy, then keep a pristine copy for the hook below
DESKTOP="$APPDIR/usr/share/applications/com.adamve.mdview.desktop"
sed -i 's|^Exec=.*|Exec=mdview %f|' "$DESKTOP"

# GNOME only shows dock/overview icons for registered .desktop entries and
# appimagelauncher is not a given; this AppRun hook self-registers on each
# launch (cheap, and keeps the path correct after the AppImage is moved)
mkdir -p "$APPDIR/apprun-hooks"
cat > "$APPDIR/apprun-hooks/60-mdview-register-desktop" <<'HOOK'
#!/usr/bin/env bash
[ -n "${APPIMAGE:-}" ] || exit 0
install -Dm644 "$APPDIR/usr/share/icons/hicolor/scalable/apps/com.adamve.mdview.svg" \
    "$HOME/.local/share/icons/hicolor/scalable/apps/com.adamve.mdview.svg" 2>/dev/null
sed "s|^Exec=.*|Exec=\"$APPIMAGE\" %f|" "$APPDIR/usr/share/applications/com.adamve.mdview.desktop" \
    > "$HOME/.local/share/applications/com.adamve.mdview.desktop" 2>/dev/null
gtk4-update-icon-cache -q -t -f "$HOME/.local/share/icons/hicolor" 2>/dev/null \
    || gtk-update-icon-cache -q -t -f "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
HOOK
chmod +x "$APPDIR/apprun-hooks/60-mdview-register-desktop"

TOOLS="$BUILD_DIR/tools"
mkdir -p "$TOOLS"
if [ ! -x "$TOOLS/linuxdeploy.AppImage" ]; then
    curl -fsSL -o "$TOOLS/linuxdeploy.AppImage" \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage"
    chmod +x "$TOOLS/linuxdeploy.AppImage"
fi

# the gtk plugin ships as a bare script on master (no release assets to auto-fetch);
# linuxdeploy discovers plugins named linuxdeploy-plugin-<name> via PATH
if [ ! -x "$TOOLS/linuxdeploy-plugin-gtk.sh" ]; then
    curl -fsSL -o "$TOOLS/linuxdeploy-plugin-gtk.sh" \
        https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh
    chmod +x "$TOOLS/linuxdeploy-plugin-gtk.sh"
fi

# run from BUILD_DIR so the produced AppImage lands there too
( cd "$BUILD_DIR" && PATH="$OLDPWD/$TOOLS:$PATH" ARCH="$ARCH" "../$TOOLS/linuxdeploy.AppImage" --appdir "$APPDIR" --plugin gtk --output appimage )

# appimagetool names the output after the desktop file's Name= field (MDView)
echo
echo "Built:"
ls -1 "$BUILD_DIR"/*.AppImage
echo "Try it:  ./$(basename "$(ls -1 "$BUILD_DIR"/*.AppImage | head -1)") some/file.md"

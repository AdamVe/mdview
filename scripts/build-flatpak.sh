#!/usr/bin/env bash
# Build the mdview Flatpak and export com.adamve.mdview.flatpak in the repo root.
# Run locally or from .github/workflows/release.yml (same entrypoint either way).
set -euo pipefail
cd "$(dirname "$0")/.."

APP_ID=com.adamve.mdview
RUNTIME=49
WORK=build-flatpak

for tool in flatpak flatpak-builder; do
    command -v "$tool" >/dev/null || {
        echo "error: '$tool' not found — install flatpak and flatpak-builder from your distro" >&2
        exit 1
    }
done

flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install -y --user flathub "org.gnome.Platform//$RUNTIME" "org.gnome.Sdk//$RUNTIME"

flatpak-builder --force-clean --repo="$WORK/repo" "$WORK/build" "packaging/flatpak/$APP_ID.yaml"
flatpak build-bundle "$WORK/repo" "$WORK/$APP_ID.flatpak" "$APP_ID"

echo
echo "Built: $PWD/$WORK/$APP_ID.flatpak"
echo "Try it:"
echo "  flatpak install -y --user ./$WORK/$APP_ID.flatpak"
echo "  flatpak run $APP_ID some/file.md"

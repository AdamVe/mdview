# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-11-26

Initial release of mdview, a fast native GTK4 markdown viewer.

### Added

- Rich rendering of CommonMark and GitHub-flavored markdown: headings,
  emphasis, strikethrough, autolinks, fenced code blocks, tables, block
  quotes with visible nesting, thematic breaks and YAML front matter
- Nested ordered, unordered and task lists rendered with real numbering
  and checkbox glyphs at any depth
- Full-text search with match highlighting and keyboard navigation
- Document outline popover generated from headings for quick jumping
- Inline image rendering, including lazy click-to-load of remote
  http(s) images
- Copy-as-markdown that reconstructs lists, quotes, tables and links
  from the selection
- Live reload when the file changes on disk
- Adjustable zoom with consistent scaling across text, code and images
- Automatic light/dark theme following the system preference
- Comfortable reading layout: pinned left gutter and capped line length
- Headless PNG export (`--render-png`) for scripting and regression tests
- Hardened parser and renderer: survives large and hostile documents
- Packaging: Flatpak manifest and AppImage build, published to GitHub
  Releases, plus a plain source build via meson/ninja

[Unreleased]: https://github.com/adamve/mdview/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/adamve/mdview/releases/tag/v0.1.0

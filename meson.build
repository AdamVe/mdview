project('mdview', 'cpp', 'c',
  version: '0.1.0-dev.0',
  default_options: [
    'cpp_std=c++20',
    'warning_level=2',
    'buildtype=debugoptimized'
  ]
)

# single source of truth for the version shown in the About dialog
add_project_arguments('-DMDVIEW_VERSION="@0@"'.format(meson.project_version()), language: 'cpp')

# Phase 1: Build test_parser - a CLI tool to test the core parser
# The GTK frontend will be added in Phase 2
enable_gtk = get_option('enable_gtk')

# Add preprocessor argument before any targets are declared
# This must come before subdir('core') which creates targets
if enable_gtk.enabled()
  add_project_arguments('-DUSE_ADWAITA', language: 'cpp')
endif

# Always set up core first (for the parser library)
subdir('core')

# Add GTK frontend
if enable_gtk.enabled()
  subdir('frontends/gtk')
endif
